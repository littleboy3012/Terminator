#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "shell.h"
#include "parser.h"
#include "history.h"
#include "jobs.h"

#define MAX_LINE_LEN 4096

static ShellContext *g_test_ctx = NULL;

static void test_sig_handler(int sig) {
    if (g_test_ctx && g_test_ctx->foreground_pid > 0) {
        kill(-g_test_ctx->foreground_pid, sig);
        kill(g_test_ctx->foreground_pid, sig);
    }
}

int main(void) {
    ShellContext ctx;
    if (shell_init(&ctx) != 0) return 1;
    history_init(0);

    g_test_ctx = &ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = test_sig_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);

    char line[MAX_LINE_LEN];
    char accum[MAX_LINE_LEN * 4] = "";
    int is_multiline = 0;

    while (!ctx.should_exit) {
        char notif[1024] = "";
        jobs_check_status(&ctx.job_table, notif, sizeof(notif));
        if (notif[0]) {
            printf("%s", notif);
            fflush(stdout);
        }

        if (is_multiline) printf("> ");
        else printf("user@terminator> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (!is_multiline) {
            if (line[0] == '\0') continue;
            snprintf(accum, sizeof(accum), "%s", line);
        } else {
            size_t a_len = strlen(accum);
            snprintf(accum + a_len, sizeof(accum) - a_len, "\n%s", line);
        }

        if (is_command_incomplete(accum)) {
            is_multiline = 1;
            continue;
        }

        is_multiline = 0;
        Pipeline pl;
        if (parse_line(accum, &ctx, &pl) != 0) {
            ctx.last_exit_status = 2;
            accum[0] = '\0';
            continue;
        }

        history_add(accum);
        shell_execute_pipeline(&ctx, &pl);
        free_pipeline(&pl);
        accum[0] = '\0';
    }

    int ret = ctx.last_exit_status;
    shell_cleanup(&ctx);
    history_cleanup();
    return ret;
}
