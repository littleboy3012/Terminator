#include "builtins.h"
#include "multiwatch.h"
#include "history.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

/**
 * @brief Checks whether a given command name corresponds to an internal shell built-in command.
 */
int is_builtin_command(const char *name) {
    if (!name) return 0;
    return (strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0 ||
            strcmp(name, "clear") == 0 || strcasecmp(name, "multiWatch") == 0 ||
            strcmp(name, "jobs") == 0 || strcmp(name, "fg") == 0 ||
            strcmp(name, "bg") == 0 || strcmp(name, "history") == 0 ||
            strcmp(name, "source") == 0);
}

/**
 * @brief Dispatches and executes an identified built-in command.
 */
int execute_builtin(ShellContext *ctx, const Command *cmd) {
    if (!ctx || !cmd || cmd->argc == 0 || !cmd->argv[0]) return 1;

    const char *name = cmd->argv[0];
    if (strcmp(name, "cd") == 0) return builtin_cd(ctx, cmd->argv);
    if (strcmp(name, "exit") == 0) return builtin_exit(ctx, cmd->argv);
    if (strcmp(name, "clear") == 0) return builtin_clear(ctx, cmd->argv);
    if (strcasecmp(name, "multiWatch") == 0) return builtin_multiwatch(ctx, cmd->argc, cmd->argv);
    if (strcmp(name, "jobs") == 0) return builtin_jobs(ctx, cmd->argv);
    if (strcmp(name, "bg") == 0) return builtin_bg(ctx, cmd->argv);
    if (strcmp(name, "fg") == 0) return builtin_fg(ctx, cmd->argv);
    if (strcmp(name, "history") == 0) return builtin_history(ctx, cmd->argv);
    if (strcmp(name, "source") == 0) return builtin_source(ctx, cmd->argv);

    return 1;
}

int execute_builtin_captured(ShellContext *ctx, const Command *cmd, char *out_buf, size_t max_len) {
    if (!ctx || !cmd || cmd->argc == 0 || !cmd->argv[0]) return 1;
    if (out_buf && max_len > 0) out_buf[0] = '\0';

    const char *name = cmd->argv[0];
    if (strcmp(name, "cd") == 0) {
        int ret = builtin_cd(ctx, cmd->argv);
        if (ret != 0 && out_buf && max_len > 0) {
            snprintf(out_buf, max_len, "terminator: cd: %s: No such file or directory\n",
                     cmd->argv[1] ? cmd->argv[1] : "");
        }
        return ret;
    }
    if (strcmp(name, "history") == 0) {
        if (out_buf && max_len > 0) {
            int n = history_format_recent(out_buf, max_len, 1000);
            return (n >= 0) ? 0 : 1;
        }
        return builtin_history(ctx, cmd->argv);
    }
    if (strcmp(name, "jobs") == 0) {
        if (out_buf && max_len > 0) {
            jobs_print(&ctx->job_table, out_buf, max_len);
            return 0;
        }
        return builtin_jobs(ctx, cmd->argv);
    }
    if (strcmp(name, "bg") == 0) {
        if (out_buf && max_len > 0) {
            return jobs_builtin_bg(&ctx->job_table, cmd->argv, out_buf, max_len);
        }
        return builtin_bg(ctx, cmd->argv);
    }
    if (strcmp(name, "source") == 0) {
        return builtin_source(ctx, cmd->argv);
    }
    return execute_builtin(ctx, cmd);
}

int builtin_source(ShellContext *ctx, char **argv) {
    if (!ctx || !argv || !argv[1]) {
        fprintf(stderr, "terminator: source: filename argument required\n");
        return 1;
    }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "terminator: source: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    char line[4096];
    int last_status = 0;
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        Pipeline pl;
        if (parse_line(line, ctx, &pl) == 0) {
            last_status = shell_execute_pipeline(ctx, &pl);
            free_pipeline(&pl);
        }
    }
    fclose(fp);
    ctx->last_exit_status = last_status;
    return last_status;
}

int builtin_history(ShellContext *ctx, char **argv) {
    (void)ctx;
    (void)argv;
    char hist_buf[65536];
    int n = history_format_recent(hist_buf, sizeof(hist_buf), 1000);
    if (n > 0) {
        if (write(STDOUT_FILENO, hist_buf, n) < 0) {}
    }
    return 0;
}

/**
 * @brief Changes the working directory of the shell session.
 *
 * Implements bash-like behavior:
 * - If no path argument is provided, navigates to the directory in the $HOME environment variable.
 * - Handles the tilde ('~') prefix, expanding it to the user's home directory.
 * - Updates ctx->cwd upon a successful directory change.
 * - Prints an error message to stderr if the directory does not exist or cannot be accessed.
 *
 * @param[in,out] ctx Pointer to the active ShellContext whose cwd field will be updated.
 * @param[in] argv Null-terminated argument vector:
 *                 - argv[0]: "cd"
 *                 - argv[1]: Optional destination path (or NULL for HOME).
 * @return 0 on success, 1 on failure.
 */
int builtin_cd(ShellContext *ctx, char **argv) {
    const char *target_dir = NULL;
    char resolved_path[PATH_MAX];

    /* If no argument was provided, default to $HOME */
    if (argv[1] == NULL) {
        target_dir = getenv("HOME");
        if (!target_dir) {
            fprintf(stderr, "terminator: cd: HOME not set\n");
            return 1;
        }
    } else {
        /* Check for tilde expansion (e.g., "~" or "~/some/path") */
        if (argv[1][0] == '~') {
            const char *home = getenv("HOME");
            if (!home) {
                target_dir = argv[1];
            } else if (argv[1][1] == '\0') {
                target_dir = home;
            } else if (argv[1][1] == '/') {
                snprintf(resolved_path, sizeof(resolved_path), "%s%s", home, argv[1] + 1);
                target_dir = resolved_path;
            } else {
                target_dir = argv[1];
            }
        } else {
            target_dir = argv[1];
        }
    }

    /* Perform the directory change using chdir() system call */
    if (chdir(target_dir) != 0) {
        fprintf(stderr, "terminator: cd: %s: %s\n", target_dir, strerror(errno));
        return 1;
    }

    /* Update the cached current working directory in the ShellContext */
    if (getcwd(ctx->cwd, sizeof(ctx->cwd)) == NULL) {
        perror("terminator: cd: getcwd failed");
        return 1;
    }

    return 0;
}

/**
 * @brief Exits the current shell session.
 *
 * Sets the ctx->should_exit flag so the outer loop terminates cleanly.
 * If an exit code is passed (e.g. "exit 2"), that code is parsed and used.
 * Otherwise, the exit code defaults to ctx->last_exit_status.
 *
 * @param[in,out] ctx Pointer to the ShellContext to mark as exiting.
 * @param[in] argv Null-terminated argument vector:
 *                 - argv[0]: "exit"
 *                 - argv[1]: Optional integer exit code string.
 * @return The final integer exit code to be returned by the shell.
 */
int builtin_exit(ShellContext *ctx, char **argv) {
    int exit_code = ctx->last_exit_status;

    if (argv[1] != NULL) {
        exit_code = atoi(argv[1]);
    }

    ctx->should_exit = 1;
    ctx->last_exit_status = exit_code;
    return exit_code;
}

/**
 * @brief Clears the terminal screen in headless / command-line mode.
 *
 * Emits standard ANSI escape sequences (\033[H\033[2J) to reset cursor and wipe screen.
 * In the X11 GUI mode, the screen is wiped directly by tab_submit_command.
 *
 * @param[in,out] ctx Pointer to active ShellContext (unused).
 * @param[in] argv Null-terminated argument vector (unused).
 * @return 0 on success.
 */
int builtin_clear(ShellContext *ctx, char **argv) {
    (void)ctx;
    (void)argv;
    printf("\033[H\033[2J");
    fflush(stdout);
    return 0;
}

/**
 * @brief Lists active background and stopped jobs in CLI mode.
 */
int builtin_jobs(ShellContext *ctx, char **argv) {
    if (!ctx) return 1;
    char out_buf[4096];
    jobs_builtin_jobs(&ctx->job_table, argv, out_buf, sizeof(out_buf));
    if (out_buf[0] != '\0') {
        printf("%s", out_buf);
        fflush(stdout);
    }
    return 0;
}

/**
 * @brief Resumes a stopped job in the background in CLI mode.
 */
int builtin_bg(ShellContext *ctx, char **argv) {
    if (!ctx) return 1;
    char out_buf[512];
    int ret = jobs_builtin_bg(&ctx->job_table, argv, out_buf, sizeof(out_buf));
    if (out_buf[0] != '\0') {
        printf("%s", out_buf);
        fflush(stdout);
    }
    return ret;
}

/**
 * @brief Brings a job to the foreground in CLI mode.
 */
int builtin_fg(ShellContext *ctx, char **argv) {
    if (!ctx) return 1;
    char out_buf[512];
    Job res_job;
    int ret = jobs_builtin_fg(&ctx->job_table, argv, out_buf, sizeof(out_buf), &res_job);
    if (ret != 0) {
        if (out_buf[0] != '\0') {
            fprintf(stderr, "%s", out_buf);
        }
        return ret;
    }

    /* Print command name being foregrounded */
    printf("%s\n", res_job.command);
    fflush(stdout);

    /* Send SIGCONT to resume if stopped */
    if (res_job.pgid > 0) {
        kill(-res_job.pgid, SIGCONT);
        kill(res_job.pid, SIGCONT);
    }

    ctx->foreground_pid = res_job.pid;
    snprintf(ctx->foreground_cmd, sizeof(ctx->foreground_cmd), "%s", res_job.command);

    int status = 0;
    waitpid(res_job.pid, &status, WUNTRACED);
    ctx->foreground_pid = 0;
    ctx->foreground_cmd[0] = '\0';

    if (WIFSTOPPED(status)) {
        Job *j = jobs_add(&ctx->job_table, res_job.pgid, res_job.pid, res_job.command, JOB_STATE_STOPPED, -1);
        printf("\n[%d]+  Stopped                 %s\n", j ? j->id : 1, res_job.command);
        fflush(stdout);
        ctx->last_exit_status = 128 + WSTOPSIG(status);
        return ctx->last_exit_status;
    }

    if (WIFEXITED(status)) {
        ctx->last_exit_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ctx->last_exit_status = 128 + WTERMSIG(status);
    }
    return ctx->last_exit_status;
}
