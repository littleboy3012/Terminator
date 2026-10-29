#include "multiwatch.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

/**
 * @brief Represents a single command tracked by multiWatch.
 */
typedef struct {
    char name[MAX_LINE_CHARS];      /**< Original command string (e.g. "date +%T", "ls | wc -l") */
    pid_t pid;                      /**< PID of the currently executing child (0 if finished / idle) */
    int pipe_fd;                    /**< Non-blocking read-end of pipe capturing stdout/stderr (-1 if closed) */
    time_t last_finish_time;        /**< Timestamp when the child command finished */
    char output_buf[16384];         /**< Accumulated output text from this execution */
    int output_len;                 /**< Number of bytes stored in output_buf */
} WatchedCmd;

/* Signal flag indicating Ctrl+C received */
static volatile sig_atomic_t g_mw_interrupted = 0;

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 */
static void mw_sigint_handler(int sig) {
    (void)sig;
    g_mw_interrupted = 1;
}

/**
 * @brief Parses commands from multiWatch arguments.
 *
 * Handles formats such as:
 * - multiWatch ["date +%T", "ls | wc -l"]
 * - multiWatch [ "cmd1" , "cmd2" ]
 * - multiWatch ["cmd1", "cmd2", "cmd3"]
 *
 * @param[in] argc Number of arguments.
 * @param[in] argv Argument array.
 * @param[out] cmds Array of WatchedCmd to populate.
 * @param[in] max_cmds Maximum capacity of cmds array.
 * @return Number of commands parsed, or 0 on error.
 */
static int parse_multiwatch_commands(int argc, char **argv, WatchedCmd *cmds, int max_cmds) {
    int count = 0;

    for (int i = 1; i < argc; i++) {
        char buf[MAX_LINE_CHARS];
        strncpy(buf, argv[i], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *p = buf;

        /* Skip leading whitespace and '[' */
        while (*p && (isspace((unsigned char)*p) || *p == '[')) {
            p++;
        }

        /* Trim trailing whitespace, ']', and ',' */
        int len = (int)strlen(p);
        while (len > 0 && (isspace((unsigned char)p[len - 1]) || p[len - 1] == ']' || p[len - 1] == ',')) {
            p[len - 1] = '\0';
            len--;
        }

        if (len > 0) {
            if (count < max_cmds) {
                strncpy(cmds[count].name, p, sizeof(cmds[count].name) - 1);
                cmds[count].name[sizeof(cmds[count].name) - 1] = '\0';
                cmds[count].pid = 0;
                cmds[count].pipe_fd = -1;
                cmds[count].last_finish_time = 0;
                cmds[count].output_len = 0;
                cmds[count].output_buf[0] = '\0';
                count++;
            }
        }
    }

    return count;
}

/**
 * @brief Spawns a single watched command in a child process with a redirected pipe.
 *
 * @param[in,out] wcmd Pointer to the WatchedCmd to launch.
 * @param[in] ctx Pointer to the active ShellContext.
 * @return 0 on success, -1 on failure.
 */
static int spawn_watched_cmd(WatchedCmd *wcmd, const ShellContext *ctx) {
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        perror("terminator: multiWatch: pipe failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("terminator: multiWatch: fork failed");
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child process: become its own process group leader */
        setpgid(0, 0);

        /* Redirect standard output and standard error to the write-end of the pipe */
        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        close(pipefds[0]);
        close(pipefds[1]);

        /* Execute command using the shell pipeline parser and executor */
        Pipeline pl;
        if (parse_line(wcmd->name, ctx, &pl) == 0) {
            int ret = shell_execute_pipeline((ShellContext *)ctx, &pl);
            free_pipeline(&pl);
            _exit(ret);
        }
        _exit(1);
    }

    /* Parent process: close write-end and set read-end to non-blocking */
    close(pipefds[1]);
    int flags = fcntl(pipefds[0], F_GETFL, 0);
    fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);

    wcmd->pipe_fd = pipefds[0];
    wcmd->pid = pid;
    wcmd->output_len = 0;
    wcmd->output_buf[0] = '\0';

    return 0;
}

/**
 * @brief Prints the formatted output block according to Task 6 specification.
 *
 * Format:
 * "cmd1", <unix_timestamp>:
 * ----------------------------------------------------
 * <output of cmd1>
 * ----------------------------------------------------
 */
static void print_formatted_output(const WatchedCmd *wcmd) {
    time_t now = time(NULL);

    printf("\"%s\", %ld:\n", wcmd->name, (long)now);
    printf("----------------------------------------------------\n");
    if (wcmd->output_len > 0) {
        printf("%s", wcmd->output_buf);
        if (wcmd->output_buf[wcmd->output_len - 1] != '\n') {
            printf("\n");
        }
    }
    printf("----------------------------------------------------\n");
    fflush(stdout);
}

int builtin_multiwatch(ShellContext *ctx, int argc, char **argv) {
    if (!ctx || argc < 2 || !argv) {
        fprintf(stderr, "terminator: multiWatch: expected multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        return 1;
    }

    WatchedCmd cmds[MAX_MW_COMMANDS];
    int num_cmds = parse_multiwatch_commands(argc, argv, cmds, MAX_MW_COMMANDS);

    if (num_cmds == 0) {
        fprintf(stderr, "terminator: multiWatch: no valid commands found in argument list\n");
        return 1;
    }

    /* Install SIGINT handler to gracefully terminate on Ctrl+C */
    g_mw_interrupted = 0;
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mw_sigint_handler;
    sigaction(SIGINT, &sa, &old_sa);

    /* 1. Initial parallel launch: spawn all watched commands simultaneously */
    for (int i = 0; i < num_cmds; i++) {
        spawn_watched_cmd(&cmds[i], ctx);
    }

    /* 2. Main multiplexing loop using poll() */
    while (!g_mw_interrupted) {
        time_t now = time(NULL);

        /* Re-launch any commands that finished at least 2 seconds ago */
        for (int i = 0; i < num_cmds; i++) {
            if (cmds[i].pid == 0 && (now - cmds[i].last_finish_time >= 2)) {
                spawn_watched_cmd(&cmds[i], ctx);
            }
        }

        /* Build pollfd array for all active command pipes */
        struct pollfd pfds[MAX_MW_COMMANDS];
        int fd_to_cmd[MAX_MW_COMMANDS];
        int num_active = 0;

        for (int i = 0; i < num_cmds; i++) {
            if (cmds[i].pipe_fd >= 0) {
                pfds[num_active].fd = cmds[i].pipe_fd;
                pfds[num_active].events = POLLIN | POLLHUP;
                pfds[num_active].revents = 0;
                fd_to_cmd[num_active] = i;
                num_active++;
            }
        }

        /* Non-busy wait with poll() (100 ms timeout to regularly check 2s re-run timer) */
        int poll_ret = poll(pfds, num_active, 100);
        if (poll_ret < 0 && errno == EINTR) {
            break; /* Interrupted by Ctrl+C */
        }

        /* Read output from any ready pipes */
        if (poll_ret > 0) {
            for (int k = 0; k < num_active; k++) {
                int c_idx = fd_to_cmd[k];
                if (pfds[k].revents & (POLLIN | POLLHUP)) {
                    char temp[1024];
                    ssize_t n = read(cmds[c_idx].pipe_fd, temp, sizeof(temp) - 1);
                    if (n > 0) {
                        temp[n] = '\0';
                        int space_left = (int)sizeof(cmds[c_idx].output_buf) - cmds[c_idx].output_len - 1;
                        if (space_left > 0) {
                            int to_copy = (n < space_left) ? (int)n : space_left;
                            memcpy(cmds[c_idx].output_buf + cmds[c_idx].output_len, temp, to_copy);
                            cmds[c_idx].output_len += to_copy;
                            cmds[c_idx].output_buf[cmds[c_idx].output_len] = '\0';
                        }
                    }
                }
            }
        }

        /* Check for finished child processes */
        for (int i = 0; i < num_cmds; i++) {
            if (cmds[i].pid > 0) {
                int status = 0;
                pid_t w = waitpid(cmds[i].pid, &status, WNOHANG);
                if (w > 0) {
                    /* Child finished: drain any remaining bytes in pipe */
                    char temp[1024];
                    ssize_t n;
                    while ((n = read(cmds[i].pipe_fd, temp, sizeof(temp) - 1)) > 0) {
                        temp[n] = '\0';
                        int space_left = (int)sizeof(cmds[i].output_buf) - cmds[i].output_len - 1;
                        if (space_left > 0) {
                            int to_copy = (n < space_left) ? (int)n : space_left;
                            memcpy(cmds[i].output_buf + cmds[i].output_len, temp, to_copy);
                            cmds[i].output_len += to_copy;
                            cmds[i].output_buf[cmds[i].output_len] = '\0';
                        }
                    }

                    /* Close pipe and record finish time */
                    close(cmds[i].pipe_fd);
                    cmds[i].pipe_fd = -1;
                    cmds[i].pid = 0;
                    cmds[i].last_finish_time = time(NULL);

                    /* Print the complete formatted output block */
                    print_formatted_output(&cmds[i]);

                    /* Reset output buffer for next run */
                    cmds[i].output_len = 0;
                    cmds[i].output_buf[0] = '\0';
                }
            }
        }
    }

    /* 3. Termination: kill all currently running child processes */
    for (int i = 0; i < num_cmds; i++) {
        if (cmds[i].pid > 0) {
            kill(-cmds[i].pid, SIGTERM);
            kill(cmds[i].pid, SIGTERM);
            if (cmds[i].pipe_fd >= 0) {
                close(cmds[i].pipe_fd);
                cmds[i].pipe_fd = -1;
            }
            waitpid(cmds[i].pid, NULL, WNOHANG);
            cmds[i].pid = 0;
        }
    }

    /* Restore original SIGINT handler */
    sigaction(SIGINT, &old_sa, NULL);

    return 0;
}
