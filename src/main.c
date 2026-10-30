#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "shell.h"
#include "parser.h"
#include "gui.h"
#include "hotkeys.h"
#include "history.h"
#include "jobs.h"

#define MAX_LINE_LEN 4096

static ShellContext *g_cli_ctx = NULL;

static void cli_sig_handler(int sig) {
    if (g_cli_ctx && g_cli_ctx->foreground_pid > 0) {
        kill(-g_cli_ctx->foreground_pid, sig);
        kill(g_cli_ctx->foreground_pid, sig);
    }
}

/**
 * @brief Prints the interactive Terminator shell prompt to standard output.
 *
 * Used in CLI fallback / headless testing mode.
 */
static void print_prompt(void) {
    printf("user@terminator> ");
    fflush(stdout);
}

/**
 * @brief Runs Terminator in text-only CLI mode.
 *
 * This mode is used when stdin is piped (e.g. automated test suites),
 * when the user explicitly passes the "--cli" flag, or when no X11 display is available.
 *
 * @param[in,out] ctx Pointer to initialized ShellContext.
 * @return Final exit code of the shell session.
 */
static int run_cli_mode(ShellContext *ctx) {
    char line_buffer[MAX_LINE_LEN];
    char accum[MAX_LINE_LEN * 4] = "";
    int is_multiline = 0;

    g_cli_ctx = ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cli_sig_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);

    while (!ctx->should_exit) {
        /* Check status of active background jobs before displaying prompt */
        char notif_buf[1024];
        notif_buf[0] = '\0';
        jobs_check_status(&ctx->job_table, notif_buf, sizeof(notif_buf));
        if (notif_buf[0] != '\0') {
            printf("%s", notif_buf);
            fflush(stdout);
        }

        if (is_multiline) {
            printf("> ");
        } else {
            print_prompt();
        }
        fflush(stdout);

        /* Read one line of user input from stdin */
        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) {
            /* EOF encountered (e.g. Ctrl+D or end of test input stream) */
            printf("\n");
            break;
        }

        /* Strip trailing newline character */
        size_t len = strlen(line_buffer);
        if (len > 0 && line_buffer[len - 1] == '\n') {
            line_buffer[len - 1] = '\0';
        }

        if (!is_multiline) {
            if (line_buffer[0] == '\0') {
                continue;
            }
            snprintf(accum, sizeof(accum), "%s", line_buffer);
        } else {
            size_t a_len = strlen(accum);
            snprintf(accum + a_len, sizeof(accum) - a_len, "\n%s", line_buffer);
        }

        /* Check for multiline continuation */
        if (is_command_incomplete(accum)) {
            is_multiline = 1;
            continue;
        }

        is_multiline = 0;

        /* Parse line into pipeline structure */
        Pipeline pipeline;
        if (parse_line(accum, ctx, &pipeline) != 0) {
            ctx->last_exit_status = 2; /* Syntax error code */
            accum[0] = '\0';
            continue;
        }

        history_add(accum);

        /* Execute the parsed pipeline */
        shell_execute_pipeline(ctx, &pipeline);

        /* Clean up allocated pipeline resources */
        free_pipeline(&pipeline);
        accum[0] = '\0';
    }

    return ctx->last_exit_status;
}

/**
 * @brief Entry point for Terminator.
 *
 * Evaluates execution flags:
 * - "useProcess=True": Selects process-based tab execution mode.
 * - "--cli": Forces text-only CLI execution (ideal for batch scripts and automated test suites).
 * - By default: launches the X11 GUI with hacker color scheme (red prompt, green input).
 *
 * @param[in] argc Number of command-line arguments.
 * @param[in] argv Array of argument strings.
 * @return 0 on normal exit, or non-zero exit status code.
 */
int main(int argc, char *argv[]) {
    int use_process_mode = 0;
    int force_cli = 0;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) {
            use_process_mode = 1;
        } else if (strcmp(argv[i], "--cli") == 0) {
            force_cli = 1;
        }
    }

    if (use_process_mode) {
        printf("[Terminator] Mode: Process Mode (useProcess=True)\n");
    } else {
        printf("[Terminator] Mode: Thread Mode (default)\n");
    }

    /* Initialize shared history in requested mode (Thread vs Process mode) */
    history_init(use_process_mode);

    /* Ensure ~/.terminatorrc exists and custom hotkeys are loaded */
    HotkeyConfig dummy_hk;
    hotkeys_init(&dummy_hk);

    /* If stdin is not a terminal (e.g. redirected from test file) or --cli requested, run CLI */
    if (force_cli || !isatty(STDIN_FILENO)) {
        ShellContext ctx;
        if (shell_init(&ctx) != 0) {
            fprintf(stderr, "terminator: failed to initialize shell context\n");
            history_cleanup();
            return 1;
        }
        int status = run_cli_mode(&ctx);
        shell_cleanup(&ctx);
        history_cleanup();
        return status;
    }

    /* Otherwise, launch the full X11 Graphical User Interface */
    GuiApp app;
    if (gui_init(&app, 900, 600, "Terminator [Hacker Shell]", use_process_mode) != 0) {
        fprintf(stderr, "terminator: X11 GUI initialization failed; falling back to CLI mode.\n");
        ShellContext ctx;
        if (shell_init(&ctx) != 0) {
            history_cleanup();
            return 1;
        }
        int status = run_cli_mode(&ctx);
        shell_cleanup(&ctx);
        history_cleanup();
        return status;
    }

    /* Run the non-blocking X11 event loop */
    gui_run_loop(&app);

    /* Free all X11 resources upon window closure */
    gui_cleanup(&app);
    history_cleanup();

    return 0;
}
