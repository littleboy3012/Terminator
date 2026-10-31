#include "shell.h"
#include "builtins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

/**
 * @brief Initializes a ShellContext structure to its default starting values.
 *
 * Captures the current working directory via getcwd() and resets exit flags.
 *
 * @param[out] ctx Pointer to the ShellContext to initialize.
 *                 Must point to valid, allocated memory.
 * @return 0 on success, or -1 if ctx is NULL or getcwd() fails.
 */
int shell_init(ShellContext *ctx) {
    if (!ctx) {
        return -1;
    }

    ctx->last_exit_status = 0;
    ctx->should_exit = 0;
    jobs_init(&ctx->job_table);
    ctx->foreground_pid = 0;
    ctx->foreground_cmd[0] = '\0';

    if (getcwd(ctx->cwd, sizeof(ctx->cwd)) == NULL) {
        perror("terminator: shell_init: getcwd failed");
        return -1;
    }

    return 0;
}

/**
 * @brief Releases any resources associated with a ShellContext before destruction.
 *
 * @param[in,out] ctx Pointer to the ShellContext to clean up.
 */
void shell_cleanup(ShellContext *ctx) {
    if (!ctx) {
        return;
    }
    jobs_kill_all(&ctx->job_table);
    ctx->should_exit = 1;
}

/**
 * @brief Applies I/O redirection for a command process using open() and dup2().
 *
 * Configures stdin if input_file is specified, and stdout if output_file is specified.
 * According to project specs: if an input file is missing, an error must be reported
 * and execution halted immediately (without running the command).
 *
 * @param[in] input_file Path to input redirection file (e.g. "< infile.txt"), or NULL.
 * @param[in] output_file Path to output redirection file (e.g. "> outfile.txt"), or NULL.
 * @return 0 on success, or -1 if opening either redirection file failed.
 */
int setup_redirections(const char *input_file, const char *output_file) {
    if (input_file) {
        int in_fd = open(input_file, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", input_file, strerror(errno));
            return -1;
        }
        if (dup2(in_fd, STDIN_FILENO) < 0) {
            perror("terminator: dup2 stdin failed");
            close(in_fd);
            return -1;
        }
        close(in_fd);
    }

    if (output_file) {
        int out_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", output_file, strerror(errno));
            return -1;
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            perror("terminator: dup2 stdout failed");
            close(out_fd);
            return -1;
        }
        close(out_fd);
    }

    return 0;
}

/**
 * @brief Executes a single Command, handling built-ins directly or external commands via fork/exec.
 *
 * Step-by-step logic:
 * 1. Validates that the command contains at least one argument.
 * 2. Checks if the command is a shell built-in (e.g. "cd", "exit"):
 *    - If no redirections are requested, runs the built-in directly in this process.
 *    - If redirections are present, temporarily duplicates original fds, applies redirections,
 *      executes the built-in, and restores the original fds with dup2().
 * 3. For external commands:
 *    - Forks a child process with fork().
 *    - In the child: applies I/O redirections (<, >), then calls execvp(argv[0], argv).
 *      If execvp fails, prints "terminator: <cmd>: command not found" and exits with 127.
 *    - In the parent: waits for the child with waitpid() and updates ctx->last_exit_status.
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] cmd Pointer to the Command structure to execute.
 * @return Exit status code of the command (0 for success, non-zero on error).
 */
int shell_execute_command(ShellContext *ctx, const Command *cmd) {
    if (!ctx || !cmd || cmd->argc == 0 || !cmd->argv[0]) {
        return 0;
    }

    /* Check if command is a built-in */
    if (is_builtin_command(cmd->argv[0])) {
        /* If redirections are requested on a built-in, redirect stdout/stdin temporarily */
        if (cmd->input_file || cmd->output_file) {
            int saved_stdin = dup(STDIN_FILENO);
            int saved_stdout = dup(STDOUT_FILENO);

            if (setup_redirections(cmd->input_file, cmd->output_file) != 0) {
                ctx->last_exit_status = 1;
                close(saved_stdin);
                close(saved_stdout);
                return 1;
            }

            int status = execute_builtin(ctx, cmd);
            ctx->last_exit_status = status;

            dup2(saved_stdin, STDIN_FILENO);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdin);
            close(saved_stdout);
            return status;
        }

        int status = execute_builtin(ctx, cmd);
        ctx->last_exit_status = status;
        return status;
    }

    /* External command: fork and exec */
    pid_t pid = fork();
    if (pid < 0) {
        perror("terminator: fork failed");
        ctx->last_exit_status = 1;
        return 1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        /* Child process */
        if (setup_redirections(cmd->input_file, cmd->output_file) != 0) {
            _exit(1);
        }

        execvp(cmd->argv[0], cmd->argv);

        /* Reached only if execvp failed */
        fprintf(stderr, "terminator: %s: command not found\n", cmd->argv[0]);
        _exit(127);
    }

    setpgid(pid, pid);
    ctx->foreground_pid = pid;

    char full_cmd[MAX_CMD_LEN] = "";
    for (int a = 0; a < cmd->argc; a++) {
        if (a > 0) strncat(full_cmd, " ", sizeof(full_cmd) - strlen(full_cmd) - 1);
        strncat(full_cmd, cmd->argv[a], sizeof(full_cmd) - strlen(full_cmd) - 1);
    }
    snprintf(ctx->foreground_cmd, sizeof(ctx->foreground_cmd), "%s", full_cmd);

    /* Parent process: wait for child to finish or suspend */
    int status = 0;
    while (waitpid(pid, &status, WUNTRACED) < 0) {
        if (errno == EINTR) {
            int st = 0;
            if (waitpid(pid, &st, WUNTRACED | WNOHANG) > 0) {
                status = st;
                break;
            }
            continue;
        }
        perror("terminator: waitpid failed");
        ctx->last_exit_status = 1;
        ctx->foreground_pid = 0;
        return 1;
    }

    ctx->foreground_pid = 0;
    ctx->foreground_cmd[0] = '\0';

    if (WIFSTOPPED(status)) {
        Job *j = jobs_add(&ctx->job_table, pid, pid, full_cmd, JOB_STATE_STOPPED, -1);
        printf("\n[%d]+  Stopped                 %s\n", j ? j->id : 1, full_cmd);
        fflush(stdout);
        ctx->last_exit_status = 128 + WSTOPSIG(status);
        return ctx->last_exit_status;
    }

    int exit_code = 0;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    ctx->last_exit_status = exit_code;
    return exit_code;
}

/**
 * @brief Executes a pipeline of commands connected via UNIX pipes.
 *
 * For a pipeline with N commands:
 * 1. Allocates (N - 1) pipes.
 * 2. Forks N child processes:
 *    - Child i connects its stdin to pipe (i - 1)'s read end (unless i == 0).
 *    - Child i connects its stdout to pipe i's write end (unless i == N - 1).
 *    - Explicit file redirections (< infile, > outfile) override pipe connections.
 *    - Closes all pipe file descriptors in each child to prevent hanging readers.
 * 3. The parent closes all pipe ends and waits for all N children using waitpid().
 * 4. The exit status of the pipeline is the exit status of the rightmost command.
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] pipeline Pointer to the Pipeline containing commands and count.
 * @return Exit status code of the pipeline.
 */
int shell_execute_pipeline(ShellContext *ctx, const Pipeline *pipeline) {
    if (!ctx || !pipeline || pipeline->num_commands <= 0) {
        return 0;
    }

    /* Background execution handling */
    if (pipeline->is_background) {
        pid_t bg_pid = fork();
        if (bg_pid < 0) {
            perror("terminator: fork failed");
            ctx->last_exit_status = 1;
            return 1;
        }

        if (bg_pid == 0) {
            setpgid(0, 0);

            /* In background, redirect stdin from /dev/null if not already redirected */
            if (pipeline->commands[0].input_file == NULL) {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    close(devnull);
                }
            }

            if (pipeline->num_commands == 1) {
                Command *c = &pipeline->commands[0];
                if (setup_redirections(c->input_file, c->output_file) != 0) {
                    _exit(1);
                }
                if (is_builtin_command(c->argv[0])) {
                    int ret = execute_builtin(ctx, c);
                    _exit(ret);
                } else {
                    execvp(c->argv[0], c->argv);
                    fprintf(stderr, "terminator: %s: command not found\n", c->argv[0]);
                    _exit(127);
                }
            }

            Pipeline fg_pl = *pipeline;
            fg_pl.is_background = 0;
            int ret = shell_execute_pipeline(ctx, &fg_pl);
            _exit(ret);
        }

        setpgid(bg_pid, bg_pid);
        char cmd_str[MAX_CMD_LEN] = "";
        for (int i = 0; i < pipeline->num_commands; i++) {
            if (i > 0) strncat(cmd_str, " | ", sizeof(cmd_str) - strlen(cmd_str) - 1);
            for (int a = 0; a < pipeline->commands[i].argc; a++) {
                if (a > 0) strncat(cmd_str, " ", sizeof(cmd_str) - strlen(cmd_str) - 1);
                strncat(cmd_str, pipeline->commands[i].argv[a], sizeof(cmd_str) - strlen(cmd_str) - 1);
            }
        }
        Job *j = jobs_add(&ctx->job_table, bg_pid, bg_pid, cmd_str, JOB_STATE_RUNNING, -1);
        printf("[%d] %d\n", j ? j->id : 1, (int)bg_pid);
        fflush(stdout);
        return 0;
    }

    /* Single command optimization */
    if (pipeline->num_commands == 1) {
        return shell_execute_command(ctx, &pipeline->commands[0]);
    }

    int num_cmds = pipeline->num_commands;
    int num_pipes = num_cmds - 1;

    /* Create N-1 pipes (2 file descriptors per pipe) */
    int *pipefds = malloc(sizeof(int) * 2 * num_pipes);
    if (!pipefds) {
        perror("terminator: malloc pipefds failed");
        ctx->last_exit_status = 1;
        return 1;
    }

    for (int i = 0; i < num_pipes; i++) {
        if (pipe(pipefds + (i * 2)) < 0) {
            perror("terminator: pipe creation failed");
            free(pipefds);
            ctx->last_exit_status = 1;
            return 1;
        }
    }

    pid_t *pids = malloc(sizeof(pid_t) * num_cmds);
    if (!pids) {
        perror("terminator: malloc pids failed");
        free(pipefds);
        ctx->last_exit_status = 1;
        return 1;
    }

    /* Fork each command in the pipeline */
    int children_spawned = 0;
    for (int i = 0; i < num_cmds; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("terminator: pipeline fork failed");
            break;
        }
        children_spawned++;

        if (pids[i] == 0) {
            pid_t pgid = (i == 0) ? 0 : pids[0];
            setpgid(0, pgid);

            /* Child process i: rewire stdin from previous pipe if not first */
            if (i > 0) {
                dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            }

            /* Rewire stdout to next pipe if not last */
            if (i < num_cmds - 1) {
                dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            }

            /* Explicit file redirection overrides pipe redirection */
            if (setup_redirections(pipeline->commands[i].input_file,
                                   pipeline->commands[i].output_file) != 0) {
                _exit(1);
            }

            /* Crucial: close all pipe file descriptors in child */
            for (int j = 0; j < 2 * num_pipes; j++) {
                close(pipefds[j]);
            }

            /* Execute built-in or external program */
            if (is_builtin_command(pipeline->commands[i].argv[0])) {
                int ret = execute_builtin(ctx, &pipeline->commands[i]);
                _exit(ret);
            } else {
                execvp(pipeline->commands[i].argv[0], pipeline->commands[i].argv);
                fprintf(stderr, "terminator: %s: command not found\n",
                        pipeline->commands[i].argv[0]);
                _exit(127);
            }
        } else {
            pid_t pgid = (i == 0) ? pids[0] : pids[0];
            setpgid(pids[i], pgid);
        }
    }

    /* Parent process must close ALL pipe file descriptors so children see EOF */
    for (int i = 0; i < 2 * num_pipes; i++) {
        close(pipefds[i]);
    }
    free(pipefds);

    /* Wait for all children to complete */
    int last_status = 0;
    if (children_spawned > 0) {
        ctx->foreground_pid = pids[0];
    }
    for (int i = 0; i < children_spawned; i++) {
        int status = 0;
        while (waitpid(pids[i], &status, WUNTRACED) < 0) {
            if (errno == EINTR) {
                int st = 0;
                if (waitpid(pids[i], &st, WUNTRACED | WNOHANG) > 0) {
                    status = st;
                    break;
                }
                continue;
            }
            break;
        }
        if (i == children_spawned - 1) {
            last_status = status;
        }
    }
    ctx->foreground_pid = 0;
    free(pids);

    int exit_code = 0;
    if (WIFEXITED(last_status)) {
        exit_code = WEXITSTATUS(last_status);
    } else if (WIFSIGNALED(last_status)) {
        exit_code = 128 + WTERMSIG(last_status);
    }

    ctx->last_exit_status = exit_code;
    return exit_code;
}
