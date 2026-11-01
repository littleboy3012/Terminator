#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <glob.h>
#include <pwd.h>

#define MAX_ARGS 1024
#define MAX_TOKEN_LEN 4096

/**
 * @brief Checks if a command string is incomplete and requires continuation lines.
 *
 * Inspects:
 * - Trailing unescaped backslash ('\')
 * - Unclosed single quotes ('\'')
 * - Unclosed double quotes ('\"')
 * - Unclosed command substitution ("$(...)" or backticks)
 * - Trailing pipe ('|') waiting for a command
 *
 * @param[in] line The accumulated command string entered so far.
 * @return 1 if command is incomplete (continuation prompt needed), 0 if complete.
 */
int is_command_incomplete(const char *line) {
    if (!line) return 0;

    int in_sq = 0, in_dq = 0, in_bt = 0, sub_depth = 0;
    const char *p = line;
    const char *last_non_space = NULL;

    while (*p) {
        if (!isspace((unsigned char)*p)) {
            last_non_space = p;
        }

        if (*p == '\\') {
            if (*(p + 1) == '\0' || (*(p + 1) == '\n' && *(p + 2) == '\0')) {
                if (!in_sq) return 1;
            }
            if (!in_sq && *(p + 1) != '\0') {
                p += 2;
                continue;
            }
        }

        if (*p == '\'' && !in_dq) {
            in_sq = !in_sq;
        } else if (*p == '"' && !in_sq) {
            in_dq = !in_dq;
        } else if (!in_sq && *p == '`') {
            in_bt = !in_bt;
        } else if (!in_sq && *p == '$' && *(p + 1) == '(') {
            sub_depth++;
            p++;
        } else if (!in_sq && sub_depth > 0 && *p == ')') {
            sub_depth--;
        }
        p++;
    }

    if (in_sq || in_dq || in_bt || sub_depth > 0) return 1;
    if (last_non_space && *last_non_space == '|' && !in_sq && !in_dq) {
        if (strstr(line, "< |") || strstr(line, "<|") || strstr(line, "> |") || strstr(line, ">|")) return 0;
        const char *t = line;
        while (*t && isspace((unsigned char)*t)) t++;
        if (*t == '|') return 0;
        return 1;
    }
    return 0;
}

/**
 * @brief Executes an inner command string for command substitution ($(cmd) or `cmd`).
 *
 * Runs the inner command in a child process, captures its stdout via a pipe,
 * strips trailing newlines per POSIX specification, and writes into out_buf.
 */
static int execute_substitution(const char *sub_cmd, const ShellContext *ctx,
                                char *out_buf, size_t max_len) {
    if (!sub_cmd || !out_buf || max_len == 0) return 0;
    out_buf[0] = '\0';

    int pipefds[2];
    if (pipe(pipefds) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefds[0]);
        close(pipefds[1]);
        return 0;
    }

    if (pid == 0) {
        dup2(pipefds[1], STDOUT_FILENO);
        close(pipefds[0]);
        close(pipefds[1]);

        Pipeline pipeline;
        if (parse_line(sub_cmd, ctx, &pipeline) == 0) {
            int ret = shell_execute_pipeline((ShellContext *)ctx, &pipeline);
            free_pipeline(&pipeline);
            _exit(ret);
        }
        _exit(1);
    }

    close(pipefds[1]);
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefds[0], out_buf + total, max_len - 1 - total)) > 0) {
        total += (size_t)n;
        if (total >= max_len - 1) break;
    }
    close(pipefds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    while (total > 0 && (out_buf[total - 1] == '\n' || out_buf[total - 1] == '\r')) {
        total--;
    }
    out_buf[total] = '\0';
    return (int)total;
}

/**
 * @brief Helper function to free an individual Command structure.
 */
void free_command(Command *cmd) {
    if (!cmd) return;
    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++) free(cmd->argv[i]);
        free(cmd->argv);
        cmd->argv = NULL;
    }
    if (cmd->input_file) {
        free(cmd->input_file);
        cmd->input_file = NULL;
    }
    if (cmd->output_file) {
        free(cmd->output_file);
        cmd->output_file = NULL;
    }
    cmd->argc = 0;
}

/**
 * @brief Helper function to deallocate all resources inside a Pipeline structure.
 */
void free_pipeline(Pipeline *pipeline) {
    if (!pipeline) return;
    if (pipeline->commands) {
        for (int i = 0; i < pipeline->num_commands; i++) {
            free_command(&pipeline->commands[i]);
        }
        free(pipeline->commands);
        pipeline->commands = NULL;
    }
    pipeline->num_commands = 0;
}

/**
 * @brief Appends up to len bytes from str into token, respecting capacity.
 */
static void append_token_str(char *token, int *token_len, const char *str, int len) {
    if (len <= 0) return;
    int available = (MAX_TOKEN_LEN - 1) - *token_len;
    if (available <= 0) return;
    int to_copy = (len < available) ? len : available;
    memcpy(token + *token_len, str, to_copy);
    *token_len += to_copy;
}

/**
 * @brief Expands environment variables, "$?", or "$(...)" into out_buf.
 */
static int expand_variable(const char *str, char *out_buf, size_t max_len,
                           const ShellContext *ctx, int *chars_consumed) {
    *chars_consumed = 1;

    if (str[1] == '?') {
        *chars_consumed = 2;
        int exit_val = ctx ? ctx->last_exit_status : 0;
        return snprintf(out_buf, max_len, "%d", exit_val);
    }

    if (str[1] == '$') {
        *chars_consumed = 2;
        return snprintf(out_buf, max_len, "%d", (int)getpid());
    }

    if (str[1] == '#') {
        *chars_consumed = 2;
        return snprintf(out_buf, max_len, "0");
    }

    if (str[1] == '0') {
        *chars_consumed = 2;
        return snprintf(out_buf, max_len, "terminator");
    }

    if (str[1] == '{') {
        int i = 2;
        while (str[i] && str[i] != '}') i++;
        if (str[i] == '}') {
            char var_name[256];
            int vlen = i - 2;
            if (vlen >= 255) vlen = 255;
            memcpy(var_name, str + 2, vlen);
            var_name[vlen] = '\0';
            *chars_consumed = i + 1;
            const char *val = getenv(var_name);
            if (val) {
                return snprintf(out_buf, max_len, "%s", val);
            }
            out_buf[0] = '\0';
            return 0;
        }
    }

    if (str[1] == '(') {
        int depth = 1;
        int i = 2;
        while (str[i] && depth > 0) {
            if (str[i] == '$' && str[i + 1] == '(') {
                depth++;
                i += 2;
                continue;
            } else if (str[i] == ')') {
                depth--;
                if (depth == 0) break;
            }
            i++;
        }
        if (depth == 0) {
            *chars_consumed = i + 1;
            char sub_cmd[MAX_TOKEN_LEN];
            int cmd_len = i - 2;
            if (cmd_len >= MAX_TOKEN_LEN) cmd_len = MAX_TOKEN_LEN - 1;
            strncpy(sub_cmd, str + 2, cmd_len);
            sub_cmd[cmd_len] = '\0';
            return execute_substitution(sub_cmd, ctx, out_buf, max_len);
        }
    }

    char var_name[256];
    int v_idx = 0, i = 1;
    while (str[i] && (isalnum((unsigned char)str[i]) || str[i] == '_') && v_idx < 255) {
        var_name[v_idx++] = str[i++];
    }
    var_name[v_idx] = '\0';
    *chars_consumed = i;

    if (v_idx == 0) {
        if (max_len > 1) {
            out_buf[0] = '$';
            out_buf[1] = '\0';
            return 1;
        }
        return 0;
    }

    const char *val = getenv(var_name);
    if (val) {
        return snprintf(out_buf, max_len, "%s", val);
    }
    out_buf[0] = '\0';
    return 0;
}

/**
 * @brief Helper to handle `...` backtick command substitutions.
 */
static void handle_backtick(const char **p_ptr, const ShellContext *ctx,
                            char *token, int *token_len) {
    const char *p = *p_ptr + 1;
    const char *start = p;
    while (*p && *p != '`') p++;

    int len = (int)(p - start);
    char sub_cmd[MAX_TOKEN_LEN];
    if (len >= MAX_TOKEN_LEN) len = MAX_TOKEN_LEN - 1;
    strncpy(sub_cmd, start, len);
    sub_cmd[len] = '\0';
    if (*p == '`') p++;

    char exp_buf[MAX_TOKEN_LEN];
    int exp_len = execute_substitution(sub_cmd, ctx, exp_buf, sizeof(exp_buf));
    append_token_str(token, token_len, exp_buf, exp_len);
    *p_ptr = p;
}

/**
 * @brief Helper to handle $VAR or $(...) expansion.
 */
static void handle_dollar_var(const char **p_ptr, const ShellContext *ctx,
                              char *token, int *token_len, int *has_wildcard) {
    int consumed = 0;
    char exp_buf[MAX_TOKEN_LEN];
    int exp_len = expand_variable(*p_ptr, exp_buf, sizeof(exp_buf), ctx, &consumed);
    append_token_str(token, token_len, exp_buf, exp_len);
    if (has_wildcard && strpbrk(exp_buf, "*?[")) {
        *has_wildcard = 1;
    }
    *p_ptr += consumed;
}

/**
 * @brief Flushes accumulated token into argv or redirection target.
 *
 * Performs pathname expansion (globbing) on unquoted wildcards (*, ?, [) and tildes (~).
 */
static int flush_token(char *token, int *token_len, int *in_token,
                       int *has_wildcard, int *has_tilde, int *next_in, int *next_out,
                       Command *cmd, char **argv_list, int *argc, const ShellContext *ctx) {
    if (!*in_token) return 0;
    token[*token_len] = '\0';

    char tilde_buf[PATH_MAX];
    char *use_token = token;
    if (*has_tilde && token[0] == '~') {
        const char *home = getenv("HOME");
        if (token[1] == '\0') {
            if (home) {
                snprintf(tilde_buf, sizeof(tilde_buf), "%s", home);
                use_token = tilde_buf;
            }
        } else if (token[1] == '/') {
            if (home) {
                snprintf(tilde_buf, sizeof(tilde_buf), "%s%s", home, token + 1);
                use_token = tilde_buf;
            }
        } else {
            char uname[128] = {0};
            int u = 1;
            while (token[u] && token[u] != '/' && u < 127) {
                uname[u - 1] = token[u];
                u++;
            }
            uname[u - 1] = '\0';
            struct passwd *pw = getpwnam(uname);
            if (pw && pw->pw_dir) {
                snprintf(tilde_buf, sizeof(tilde_buf), "%s%s", pw->pw_dir, token + u);
                use_token = tilde_buf;
            }
        }
    }

    if (*next_in || *next_out) {
        char *target = NULL;
        if (*has_wildcard) {
            char pattern[PATH_MAX * 2];
            int is_rel = (ctx && ctx->cwd[0] && use_token[0] != '/');
            if (is_rel) {
                snprintf(pattern, sizeof(pattern), "%s/%s", ctx->cwd, use_token);
            } else {
                snprintf(pattern, sizeof(pattern), "%s", use_token);
            }
            glob_t g;
            if (glob(pattern, 0, NULL, &g) == 0) {
                if (g.gl_pathc == 1) {
                    size_t pre_len = is_rel ? (strlen(ctx->cwd) + 1) : 0;
                    target = strdup(g.gl_pathv[0] + pre_len);
                } else if (g.gl_pathc > 1) {
                    fprintf(stderr, "terminator: %s: ambiguous redirect\n", token);
                    globfree(&g);
                    return -1;
                }
                globfree(&g);
            }
        }
        if (!target) {
            target = strdup(use_token);
        }

        if (*next_in) {
            cmd->input_file = target;
            *next_in = 0;
        } else {
            cmd->output_file = target;
            *next_out = 0;
        }
    } else {
        if (*has_wildcard) {
            char pattern[PATH_MAX * 2];
            int is_rel = (ctx && ctx->cwd[0] && use_token[0] != '/');
            if (is_rel) {
                snprintf(pattern, sizeof(pattern), "%s/%s", ctx->cwd, use_token);
            } else {
                snprintf(pattern, sizeof(pattern), "%s", use_token);
            }
            glob_t g;
            int ret = glob(pattern, 0, NULL, &g);
            if (ret == 0 && g.gl_pathc > 0) {
                size_t pre_len = is_rel ? (strlen(ctx->cwd) + 1) : 0;
                for (size_t i = 0; i < g.gl_pathc && *argc < MAX_ARGS - 1; i++) {
                    const char *match = g.gl_pathv[i] + pre_len;
                    argv_list[(*argc)++] = strdup(match);
                }
                globfree(&g);
            } else {
                if (*argc < MAX_ARGS - 1) {
                    argv_list[(*argc)++] = strdup(use_token);
                }
            }
        } else {
            if (*argc < MAX_ARGS - 1) {
                argv_list[(*argc)++] = strdup(use_token);
            }
        }
    }

    *token_len = 0;
    *in_token = 0;
    *has_wildcard = 0;
    *has_tilde = 0;
    return 0;
}

/**
 * @brief Parses a single command segment into arguments and I/O redirection paths.
 */
static int parse_single_command(const char *cmd_segment, const ShellContext *ctx, Command *cmd) {
    memset(cmd, 0, sizeof(Command));

    char **argv_list = malloc(sizeof(char *) * MAX_ARGS);
    if (!argv_list) return -1;

    int argc = 0, token_len = 0, in_token = 0;
    int next_in = 0, next_out = 0;
    int has_wildcard = 0, has_tilde = 0;
    char token[MAX_TOKEN_LEN];
    const char *p = cmd_segment;

    while (*p) {
        if (isspace((unsigned char)*p)) {
            if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                for (int i = 0; i < argc; i++) free(argv_list[i]);
                free(argv_list);
                return -1;
            }
            p++;
            continue;
        }

        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            if (in_token) {
                if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                    for (int i = 0; i < argc; i++) free(argv_list[i]);
                    free(argv_list);
                    return -1;
                }
            }
            p += 4;
            continue;
        }

        if (p[0] == '2' && p[1] == '>') {
            if (in_token) {
                if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                    for (int i = 0; i < argc; i++) free(argv_list[i]);
                    free(argv_list);
                    return -1;
                }
            }
            next_out = 1;
            p += 2;
            continue;
        }

        if (p[0] == '>' && p[1] == '>') {
            if (in_token) {
                if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                    for (int i = 0; i < argc; i++) free(argv_list[i]);
                    free(argv_list);
                    return -1;
                }
            }
            next_out = 1;
            cmd->is_append = 1;
            p += 2;
            continue;
        }

        if (*p == '<') {
            if (in_token) {
                if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                    for (int i = 0; i < argc; i++) free(argv_list[i]);
                    free(argv_list);
                    return -1;
                }
            }
            next_in = 1;
            p++;
            continue;
        }

        if (*p == '>') {
            if (in_token) {
                if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                    for (int i = 0; i < argc; i++) free(argv_list[i]);
                    free(argv_list);
                    return -1;
                }
            }
            next_out = 1;
            p++;
            continue;
        }

        /* Single quotes: literal content until closing quote */
        if (*p == '\'') {
            in_token = 1;
            p++;
            const char *qstart = p;
            while (*p && *p != '\'') p++;
            append_token_str(token, &token_len, qstart, (int)(p - qstart));
            if (*p == '\'') p++;
            continue;
        }

        /* Line continuation \<newline> */
        if (*p == '\\' && *(p + 1) == '\n') {
            p += 2;
            continue;
        }

        /* Backtick command substitution */
        if (*p == '`') {
            in_token = 1;
            handle_backtick(&p, ctx, token, &token_len);
            continue;
        }

        /* Double quotes: variables & backticks expanded, whitespace preserved */
        if (*p == '"') {
            in_token = 1;
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1) == '\n') {
                    if (token_len < MAX_TOKEN_LEN - 1) token[token_len++] = '\n';
                    p += 2;
                } else if (*p == '\\' && (*(p + 1) == '$' || *(p + 1) == '`' || *(p + 1) == '"' || *(p + 1) == '\\')) {
                    p++;
                    if (token_len < MAX_TOKEN_LEN - 1) token[token_len++] = *p++;
                } else if (*p == '$') {
                    handle_dollar_var(&p, ctx, token, &token_len, NULL);
                } else if (*p == '`') {
                    handle_backtick(&p, ctx, token, &token_len);
                } else {
                    if (token_len < MAX_TOKEN_LEN - 1) token[token_len++] = *p;
                    p++;
                }
            }
            if (*p == '"') p++;
            continue;
        }

        /* Unquoted character */
        if (*p == '$') {
            int consumed = 0;
            char exp_buf[MAX_TOKEN_LEN];
            int exp_len = expand_variable(p, exp_buf, sizeof(exp_buf), ctx, &consumed);
            p += consumed;

            if (exp_len > 0) {
                in_token = 1;
                char *tok = strtok(exp_buf, " \t\n\r");
                if (tok) {
                    append_token_str(token, &token_len, tok, (int)strlen(tok));
                    char *next = strtok(NULL, " \t\n\r");
                    while (next) {
                        if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
                            for (int i = 0; i < argc; i++) free(argv_list[i]);
                            free(argv_list);
                            return -1;
                        }
                        in_token = 1;
                        append_token_str(token, &token_len, next, (int)strlen(next));
                        next = strtok(NULL, " \t\n\r");
                    }
                }
            }
            continue;
        }
        in_token = 1;
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
        } else {
            if (*p == '*' || *p == '?' || *p == '[') {
                has_wildcard = 1;
            } else if (*p == '~' && token_len == 0) {
                has_tilde = 1;
            }
        }
        if (token_len < MAX_TOKEN_LEN - 1) {
            token[token_len++] = *p;
        }
        p++;
    }

    if (flush_token(token, &token_len, &in_token, &has_wildcard, &has_tilde, &next_in, &next_out, cmd, argv_list, &argc, ctx) != 0) {
        for (int i = 0; i < argc; i++) free(argv_list[i]);
        free(argv_list);
        return -1;
    }

    if (next_in || next_out) {
        fprintf(stderr, "terminator: syntax error near unexpected token 'newline'\n");
        for (int i = 0; i < argc; i++) free(argv_list[i]);
        free(argv_list);
        return -1;
    }

    argv_list[argc] = NULL;
    cmd->argv = argv_list;
    cmd->argc = argc;
    return 0;
}

/**
 * @brief Checks if a command line ends with an unquoted background operator '&'.
 *
 * If found, strips the '&' from line_buf and returns 1.
 */
static int check_and_strip_background(char *line_buf) {
    if (!line_buf) return 0;
    int in_sq = 0, in_dq = 0, in_bt = 0, sub_depth = 0;
    int last_amp_idx = -1;
    int len = (int)strlen(line_buf);

    for (int i = 0; i < len; i++) {
        char c = line_buf[i];
        if (c == '\\' && i + 1 < len) {
            i++;
            continue;
        }
        if (c == '\'' && !in_dq) {
            in_sq = !in_sq;
        } else if (c == '"' && !in_sq) {
            in_dq = !in_dq;
        } else if (c == '`' && !in_sq) {
            in_bt = !in_bt;
        } else if (c == '$' && i + 1 < len && line_buf[i + 1] == '(' && !in_sq) {
            sub_depth++;
            i++;
        } else if (c == ')' && sub_depth > 0 && !in_sq) {
            sub_depth--;
        } else if (c == '&' && !in_sq && !in_dq && !in_bt && sub_depth == 0) {
            if ((i == 0 || line_buf[i - 1] != '&') && (i + 1 >= len || line_buf[i + 1] != '&')) {
                last_amp_idx = i;
            }
        }
    }

    if (last_amp_idx >= 0) {
        int only_spaces = 1;
        for (int i = last_amp_idx + 1; i < len; i++) {
            if (!isspace((unsigned char)line_buf[i])) {
                only_spaces = 0;
                break;
            }
        }
        if (only_spaces) {
            line_buf[last_amp_idx] = '\0';
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Parses an entire command line string into a Pipeline of one or more commands.
 */
int parse_line(const char *line, const ShellContext *ctx, Pipeline *pipeline) {
    if (!line || !pipeline) return -1;

    pipeline->commands = NULL;
    pipeline->num_commands = 0;
    pipeline->is_background = 0;

    char line_copy[MAX_TOKEN_LEN];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    if (check_and_strip_background(line_copy)) {
        pipeline->is_background = 1;
    }

    /* Split command segments on unquoted '|' in place */
    const char *seg_ptrs[MAX_ARGS];
    int seg_count = 0;
    seg_ptrs[seg_count++] = line_copy;

    char *p = line_copy;
    int in_sq = 0, in_dq = 0, in_bt = 0, sub_depth = 0;

    while (*p) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p += 2;
            continue;
        }
        if (*p == '\'' && !in_dq) {
            in_sq = !in_sq;
        } else if (*p == '"' && !in_sq) {
            in_dq = !in_dq;
        } else if (*p == '`' && !in_sq) {
            in_bt = !in_bt;
        } else if (*p == '$' && *(p + 1) == '(' && !in_sq) {
            sub_depth++;
            p++;
        } else if (*p == ')' && sub_depth > 0 && !in_sq) {
            sub_depth--;
        } else if (*p == '|' && !in_sq && !in_dq && !in_bt && sub_depth == 0) {
            *p = '\0';
            if (seg_count < MAX_ARGS) {
                seg_ptrs[seg_count++] = p + 1;
            }
        }
        p++;
    }

    pipeline->commands = calloc(seg_count, sizeof(Command));
    if (!pipeline->commands) return -1;
    pipeline->num_commands = seg_count;

    for (int i = 0; i < seg_count; i++) {
        if (parse_single_command(seg_ptrs[i], ctx, &pipeline->commands[i]) != 0) {
            free_pipeline(pipeline);
            return -1;
        }
    }

    return 0;
}
