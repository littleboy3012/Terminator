#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"
#include "parser.h"

#define MAX_LINE_LEN 4096

/**
 * @brief Prints the interactive Terminator shell prompt.
 *
 * As mandated by the specification, the prompt format is:
 * user@terminator> 
 */
static void print_prompt(void) {
    printf("user@terminator> ");
    fflush(stdout);
}

/**
 * @brief Entry point for the Terminator shell.
 *
 * Supports command-line flag evaluation (e.g. useProcess=True).
 * In Phase 1, runs the command execution engine directly to verify:
 * - Parsing and execution of external commands (fork + execvp + waitpid)
 * - Built-in commands (cd, exit) and $? status tracking
 * - File redirection (< and >)
 * - Arbitrary pipeline length (cmd1 | cmd2 | ... | cmdN)
 *
 * @param[in] argc Argument count passed from command line.
 * @param[in] argv Argument vector. argv[1] can optionally be "useProcess=True".
 * @return 0 on normal exit, or non-zero exit status code.
 */
int main(int argc, char *argv[]) {
    int use_process_mode = 0;

    /* Check launch arguments for execution mode */
    if (argc > 1 && strcmp(argv[1], "useProcess=True") == 0) {
        use_process_mode = 1;
        printf("[Terminator] Launching in Process Mode (useProcess=True)\n");
    } else {
        printf("[Terminator] Launching in Thread Mode (default)\n");
    }
    (void)use_process_mode;

    /* Initialize the active shell context */
    ShellContext ctx;
    if (shell_init(&ctx) != 0) {
        fprintf(stderr, "terminator: failed to initialize shell context\n");
        return 1;
    }

    char line_buffer[MAX_LINE_LEN];

    /* Phase 1 REPL (Read-Eval-Print Loop) */
    while (!ctx.should_exit) {
        print_prompt();

        /* Read one line of user input from stdin */
        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) {
            /* EOF encountered (e.g. Ctrl+D) */
            printf("\n");
            break;
        }

        /* Strip trailing newline character */
        size_t len = strlen(line_buffer);
        if (len > 0 && line_buffer[len - 1] == '\n') {
            line_buffer[len - 1] = '\0';
        }

        /* Skip empty lines */
        if (line_buffer[0] == '\0') {
            continue;
        }

        /* Parse line into pipeline structure */
        Pipeline pipeline;
        if (parse_line(line_buffer, &ctx, &pipeline) != 0) {
            ctx.last_exit_status = 2; /* Syntax error code */
            continue;
        }

        /* Execute the parsed pipeline */
        shell_execute_pipeline(&ctx, &pipeline);

        /* Clean up allocated pipeline resources */
        free_pipeline(&pipeline);
    }

    int exit_code = ctx.last_exit_status;
    shell_cleanup(&ctx);
    return exit_code;
}
