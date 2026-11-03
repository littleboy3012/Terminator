#include "script.h"
#include "parser.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>

int is_bash_script(const char *path) {
    if (!path || path[0] == '\0') return 0;
    if (strcmp(path, "bash") == 0) return 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }

    /* Has .sh extension */
    const char *dot = strrchr(path, '.');
    if (dot && strcmp(dot, ".sh") == 0) {
        return 1;
    }

    /* Check for shebang '#!' at the beginning of the file */
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char magic[2] = {0};
    size_t nr = fread(magic, 1, 2, fp);
    fclose(fp);

    if (nr == 2 && magic[0] == '#' && magic[1] == '!') {
        return 1;
    }

    return 0;
}

static void expand_test_tokens(ShellContext *ctx, const char *raw_expr, char **tokens, int *num_tokens, int max_tokens) {
    *num_tokens = 0;
    if (!raw_expr) return;

    const char *p = raw_expr;
    while (*p && *num_tokens < max_tokens) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        char buf[1024];
        int blen = 0;
        bool in_quotes = false;
        char qchar = '\0';

        while (*p && (in_quotes || !isspace((unsigned char)*p))) {
            if (!in_quotes && (*p == '\'' || *p == '"')) {
                in_quotes = true;
                qchar = *p++;
                continue;
            }
            if (in_quotes && *p == qchar) {
                in_quotes = false;
                qchar = '\0';
                p++;
                continue;
            }

            if (*p == '$' && qchar != '\'') {
                /* Variable expansion */
                p++;
                if (*p == '{') {
                    p++;
                    char vname[256];
                    int vi = 0;
                    while (*p && *p != '}' && vi < 255) vname[vi++] = *p++;
                    vname[vi] = '\0';
                    if (*p == '}') p++;

                    const char *val = NULL;
                    char num_buf[32];
                    if (vname[0] == '#' && vname[1] == '\0') {
                        snprintf(num_buf, sizeof(num_buf), "%d", ctx ? ctx->script_argc : 0);
                        val = num_buf;
                    } else if (vname[0] == '0' && vname[1] == '\0') {
                        val = (ctx && ctx->script_name[0]) ? ctx->script_name : "terminator";
                    } else if (isdigit((unsigned char)vname[0]) && vname[1] == '\0') {
                        int idx = vname[0] - '1';
                        if (ctx && ctx->script_argv && idx >= 0 && idx < ctx->script_argc) {
                            val = ctx->script_argv[idx];
                        }
                    } else if (vname[0] == '?' && vname[1] == '\0') {
                        snprintf(num_buf, sizeof(num_buf), "%d", ctx ? ctx->last_exit_status : 0);
                        val = num_buf;
                    } else {
                        val = getenv(vname);
                    }

                    if (val) {
                        for (int k = 0; val[k] && blen < (int)sizeof(buf) - 1; k++) {
                            buf[blen++] = val[k];
                        }
                    }
                } else if (*p == '#') {
                    p++;
                    char num_buf[32];
                    snprintf(num_buf, sizeof(num_buf), "%d", ctx ? ctx->script_argc : 0);
                    for (int k = 0; num_buf[k] && blen < (int)sizeof(buf) - 1; k++) buf[blen++] = num_buf[k];
                } else if (*p == '0') {
                    p++;
                    const char *val = (ctx && ctx->script_name[0]) ? ctx->script_name : "terminator";
                    for (int k = 0; val[k] && blen < (int)sizeof(buf) - 1; k++) buf[blen++] = val[k];
                } else if (*p == '?') {
                    p++;
                    char num_buf[32];
                    snprintf(num_buf, sizeof(num_buf), "%d", ctx ? ctx->last_exit_status : 0);
                    for (int k = 0; num_buf[k] && blen < (int)sizeof(buf) - 1; k++) buf[blen++] = num_buf[k];
                } else if (isdigit((unsigned char)*p)) {
                    int idx = *p - '1';
                    p++;
                    if (ctx && ctx->script_argv && idx >= 0 && idx < ctx->script_argc && ctx->script_argv[idx]) {
                        const char *val = ctx->script_argv[idx];
                        for (int k = 0; val[k] && blen < (int)sizeof(buf) - 1; k++) buf[blen++] = val[k];
                    }
                } else {
                    char vname[256];
                    int vi = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && vi < 255) {
                        vname[vi++] = *p++;
                    }
                    vname[vi] = '\0';
                    const char *val = getenv(vname);
                    if (val) {
                        for (int k = 0; val[k] && blen < (int)sizeof(buf) - 1; k++) {
                            buf[blen++] = val[k];
                        }
                    }
                }
            } else {
                if (blen < (int)sizeof(buf) - 1) {
                    buf[blen++] = *p;
                }
                p++;
            }
        }

        buf[blen] = '\0';
        tokens[(*num_tokens)++] = strdup(buf);
    }
}

int script_eval_test(ShellContext *ctx, const char *expr) {
    if (!expr) return 1;

    char *tokens[64];
    int ntokens = 0;
    expand_test_tokens(ctx, expr, tokens, &ntokens, 64);

    if (ntokens == 0) {
        return 1; /* Empty test evaluates to false */
    }

    bool invert = false;
    int cur = 0;
    if (strcmp(tokens[0], "!") == 0) {
        invert = true;
        cur++;
    }

    int remaining = ntokens - cur;
    int result = 1; /* 0 = true, 1 = false */

    if (remaining == 1) {
        /* [ string ]: true if non-empty */
        result = (tokens[cur][0] != '\0') ? 0 : 1;
    } else if (remaining == 2) {
        const char *op = tokens[cur];
        const char *arg = tokens[cur + 1];

        if (strcmp(op, "-z") == 0) {
            result = (arg[0] == '\0') ? 0 : 1;
        } else if (strcmp(op, "-n") == 0) {
            result = (arg[0] != '\0') ? 0 : 1;
        } else if (strcmp(op, "-f") == 0) {
            struct stat st;
            result = (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-d") == 0) {
            struct stat st;
            result = (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-e") == 0) {
            result = (access(arg, F_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-s") == 0) {
            struct stat st;
            result = (stat(arg, &st) == 0 && st.st_size > 0) ? 0 : 1;
        } else if (strcmp(op, "-r") == 0) {
            result = (access(arg, R_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-w") == 0) {
            result = (access(arg, W_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-x") == 0) {
            result = (access(arg, X_OK) == 0) ? 0 : 1;
        }
    } else if (remaining == 3) {
        const char *arg1 = tokens[cur];
        const char *op = tokens[cur + 1];
        const char *arg2 = tokens[cur + 2];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            result = (strcmp(arg1, arg2) == 0) ? 0 : 1;
        } else if (strcmp(op, "!=") == 0) {
            result = (strcmp(arg1, arg2) != 0) ? 0 : 1;
        } else if (strcmp(op, "-eq") == 0) {
            result = (atol(arg1) == atol(arg2)) ? 0 : 1;
        } else if (strcmp(op, "-ne") == 0) {
            result = (atol(arg1) != atol(arg2)) ? 0 : 1;
        } else if (strcmp(op, "-lt") == 0) {
            result = (atol(arg1) < atol(arg2)) ? 0 : 1;
        } else if (strcmp(op, "-le") == 0) {
            result = (atol(arg1) <= atol(arg2)) ? 0 : 1;
        } else if (strcmp(op, "-gt") == 0) {
            result = (atol(arg1) > atol(arg2)) ? 0 : 1;
        } else if (strcmp(op, "-ge") == 0) {
            result = (atol(arg1) >= atol(arg2)) ? 0 : 1;
        }
    }

    for (int i = 0; i < ntokens; i++) {
        free(tokens[i]);
    }

    if (invert) {
        result = (result == 0) ? 1 : 0;
    }
    return result;
}

static char *extract_bracket_expr(const char *line) {
    const char *open_b = strchr(line, '[');
    if (!open_b) return NULL;
    const char *close_b = strrchr(open_b, ']');
    if (!close_b || close_b <= open_b) return NULL;

    size_t len = (size_t)(close_b - open_b - 1);
    char *expr = (char *)malloc(len + 1);
    if (!expr) return NULL;
    memcpy(expr, open_b + 1, len);
    expr[len] = '\0';
    return expr;
}

typedef struct {
    bool parent_active;
    bool branch_matched;
    bool active;
} CondFrame;

int script_run(ShellContext *ctx, const char *script_path, int argc, char **argv) {
    if (!ctx || !script_path) return 1;

    FILE *fp = fopen(script_path, "r");
    if (!fp) {
        fprintf(stderr, "terminator: %s: %s\n", script_path, strerror(errno));
        ctx->last_exit_status = 127;
        return 127;
    }

    /* Read all lines into memory for pre-validation and sequential execution */
    char **lines = NULL;
    size_t num_lines = 0;
    size_t cap_lines = 64;
    lines = (char **)malloc(sizeof(char *) * cap_lines);

    char *buf = NULL;
    size_t buflen = 0;
    ssize_t nread;
    while ((nread = getline(&buf, &buflen, fp)) != -1) {
        if (num_lines >= cap_lines) {
            cap_lines *= 2;
            char **new_lines = (char **)realloc(lines, sizeof(char *) * cap_lines);
            if (!new_lines) break;
            lines = new_lines;
        }
        lines[num_lines++] = strdup(buf);
    }
    free(buf);
    fclose(fp);

    /* Pre-validation: ensure balanced if / fi */
    int if_count = 0, fi_count = 0;
    for (size_t i = 0; i < num_lines; i++) {
        const char *p = lines[i];
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "if ", 3) == 0 || strncmp(p, "if[", 3) == 0 || strcmp(p, "if") == 0) {
            if_count++;
        } else if (strncmp(p, "fi", 2) == 0 && (p[2] == '\0' || isspace((unsigned char)p[2]) || p[2] == ';')) {
            fi_count++;
        }
    }

    if (if_count != fi_count) {
        fprintf(stderr, "terminator: %s: syntax error: unexpected end of file (missing 'fi')\n", script_path);
        for (size_t i = 0; i < num_lines; i++) free(lines[i]);
        free(lines);
        ctx->last_exit_status = 2;
        return 2;
    }

    /* Save and set positional parameters */
    int old_argc = ctx->script_argc;
    char **old_argv = ctx->script_argv;
    char old_name[PATH_MAX];
    strncpy(old_name, ctx->script_name, sizeof(old_name));

    ctx->script_name[sizeof(ctx->script_name) - 1] = '\0';
    strncpy(ctx->script_name, script_path, sizeof(ctx->script_name) - 1);
    ctx->script_argc = (argc > 1) ? (argc - 1) : 0;
    ctx->script_argv = (argc > 1) ? (argv + 1) : NULL;

    CondFrame stack[64];
    int depth = 0;

    int last_status = 0;
    bool exit_script = false;

    for (size_t line_idx = 0; line_idx < num_lines && !exit_script; line_idx++) {
        char *line = lines[line_idx];
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        /* Conditionals */
        if (strncmp(p, "if ", 3) == 0 || strncmp(p, "if[", 3) == 0) {
            bool parent_act = (depth == 0) ? true : stack[depth - 1].active;
            bool condition_met = false;
            if (parent_act) {
                char *expr = extract_bracket_expr(p);
                if (expr) {
                    condition_met = (script_eval_test(ctx, expr) == 0);
                    free(expr);
                }
            }

            stack[depth].parent_active = parent_act;
            stack[depth].branch_matched = condition_met;
            stack[depth].active = parent_act && condition_met;
            depth++;
            continue;
        }

        if (strncmp(p, "elif ", 5) == 0 || strncmp(p, "elif[", 5) == 0) {
            if (depth > 0) {
                CondFrame *f = &stack[depth - 1];
                if (f->parent_active && !f->branch_matched) {
                    char *expr = extract_bracket_expr(p);
                    bool condition_met = false;
                    if (expr) {
                        condition_met = (script_eval_test(ctx, expr) == 0);
                        free(expr);
                    }
                    if (condition_met) {
                        f->branch_matched = true;
                        f->active = true;
                    } else {
                        f->active = false;
                    }
                } else {
                    f->active = false;
                }
            }
            continue;
        }

        if (strncmp(p, "else", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]) || p[4] == ';')) {
            if (depth > 0) {
                CondFrame *f = &stack[depth - 1];
                if (f->parent_active && !f->branch_matched) {
                    f->active = true;
                    f->branch_matched = true;
                } else {
                    f->active = false;
                }
            }
            continue;
        }

        if (strncmp(p, "fi", 2) == 0 && (p[2] == '\0' || isspace((unsigned char)p[2]) || p[2] == ';')) {
            if (depth > 0) depth--;
            continue;
        }

        if (strncmp(p, "then", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]) || p[4] == ';')) {
            continue;
        }

        /* Check if currently active in conditional execution */
        bool is_active = (depth == 0) || stack[depth - 1].active;
        if (!is_active) continue;

        /* Variable assignment: NAME=value (starts with letter/_, followed by [A-Za-z0-9_]*=) */
        char *eq = strchr(p, '=');
        if (eq && eq > p && !strchr(p, ' ') && !strchr(p, '\t')) {
            bool valid_ident = (isalpha((unsigned char)p[0]) || p[0] == '_');
            for (char *c = p; c < eq && valid_ident; c++) {
                if (!isalnum((unsigned char)*c) && *c != '_') valid_ident = false;
            }
            if (valid_ident) {
                *eq = '\0';
                const char *val_raw = eq + 1;
                char val_clean[1024];
                if ((val_raw[0] == '"' && val_raw[strlen(val_raw) - 1] == '"') ||
                    (val_raw[0] == '\'' && val_raw[strlen(val_raw) - 1] == '\'')) {
                    size_t vlen = strlen(val_raw) - 2;
                    strncpy(val_clean, val_raw + 1, vlen);
                    val_clean[vlen] = '\0';
                } else {
                    strncpy(val_clean, val_raw, sizeof(val_clean) - 1);
                    val_clean[sizeof(val_clean) - 1] = '\0';
                }
                setenv(p, val_clean, 1);
                continue;
            }
        }

        /* exit [n] command */
        if (strncmp(p, "exit", 4) == 0 && (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            const char *arg = p + 4;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            int code = ctx->last_exit_status;
            if (*arg != '\0') {
                code = atoi(arg);
            }
            last_status = code;
            ctx->last_exit_status = code;
            exit_script = true;
            break;
        }

        /* Normal command / pipeline */
        Pipeline pl;
        if (parse_line(p, ctx, &pl) == 0) {
            last_status = shell_execute_pipeline(ctx, &pl);
            ctx->last_exit_status = last_status;
            free_pipeline(&pl);
        }
    }

    for (size_t i = 0; i < num_lines; i++) {
        free(lines[i]);
    }
    free(lines);

    /* Restore previous positional parameters */
    ctx->script_argc = old_argc;
    ctx->script_argv = old_argv;
    snprintf(ctx->script_name, sizeof(ctx->script_name), "%s", old_name);

    ctx->last_exit_status = last_status;
    return last_status;
}
