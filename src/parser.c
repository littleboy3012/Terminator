#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#define MAX_ARGS 128
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
    if (!line) {
        return 0;
    }

    int in_single_quote = 0;
    int in_double_quote = 0;
    int subshell_depth = 0;
    int in_backtick = 0;
    const char *p = line;
    const char *last_non_space = NULL;

    while (*p) {
        if (!isspace((unsigned char)*p)) {
            last_non_space = p;
        }

        /* Check for escaped character */
        if (*p == '\\') {
            if (*(p + 1) == '\0') {
                /* Trailing backslash at the very end of string */
                if (!in_single_quote) {
                    return 1;
                }
            } else if (*(p + 1) == '\n' && *(p + 2) == '\0') {
                /* Backslash immediately followed by newline at the end */
                if (!in_single_quote) {
                    return 1;
                }
            }
            if (!in_single_quote && *(p + 1) != '\0') {
                p += 2;
                continue;
            }
        }

        if (*p == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (*p == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (!in_single_quote && *p == '`') {
            in_backtick = !in_backtick;
        } else if (!in_single_quote && *p == '$' && *(p + 1) == '(') {
            subshell_depth++;
            p++;
        } else if (!in_single_quote && subshell_depth > 0 && *p == ')') {
            subshell_depth--;
        }
        p++;
    }

    if (in_single_quote || in_double_quote || in_backtick || subshell_depth > 0) {
        return 1;
    }

    /* Trailing pipe '|' also requests continuation */
    if (last_non_space && *last_non_space == '|' && !in_single_quote && !in_double_quote) {
        return 1;
    }

    return 0;
}

/**
 * @brief Executes an inner command string for command substitution ($(cmd) or `cmd`).
 *
 * Runs the inner command in a child process, captures its standard output via a pipe,
 * strips any trailing newlines (per POSIX bash specification), and copies into out_buf.
 *
 * @param[in] sub_cmd The inner command string to execute.
 * @param[in] ctx Pointer to active ShellContext.
 * @param[out] out_buf Destination buffer for captured output.
 * @param[in] max_len Maximum capacity of out_buf.
 * @return Number of characters written into out_buf.
 */
static int execute_substitution(const char *sub_cmd, const ShellContext *ctx,
                                char *out_buf, size_t max_len) {
    if (!sub_cmd || !out_buf || max_len == 0) {
        return 0;
    }
    out_buf[0] = '\0';

    int pipefds[2];
    if (pipe(pipefds) < 0) {
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefds[0]);
        close(pipefds[1]);
        return 0;
    }

    if (pid == 0) {
        /* Child process: redirect stdout to pipe write end */
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

    /* Parent process: read output from pipe read end */
    close(pipefds[1]);
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefds[0], out_buf + total, max_len - 1 - total)) > 0) {
        total += (size_t)n;
        if (total >= max_len - 1) {
            break;
        }
    }
    close(pipefds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Strip trailing newline characters per bash quoting/substitution rules */
    while (total > 0 && (out_buf[total - 1] == '\n' || out_buf[total - 1] == '\r')) {
        total--;
    }
    out_buf[total] = '\0';

    return (int)total;
}

/**
 * @brief Helper function to free an individual Command structure and its heap-allocated members.
 *
 * @param[in,out] cmd Pointer to the Command structure to free.
 *                    All allocated strings within argv, input_file, and output_file are freed.
 */
void free_command(Command *cmd) {
    if (!cmd) {
        return;
    }

    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++) {
            free(cmd->argv[i]);
        }
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
 *
 * Calls free_command() on each command in the pipeline and frees the commands array itself.
 *
 * @param[in,out] pipeline Pointer to the Pipeline structure to clean up.
 */
void free_pipeline(Pipeline *pipeline) {
    if (!pipeline) {
        return;
    }

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
 * @brief Expands environment variables or special shell variables like "$?" into a buffer.
 *
 * Handles:
 * - "$?": Expands to ctx->last_exit_status as an integer string.
 * - "$VAR": Queries getenv("VAR") and appends its value if found.
 *
 * @param[in] str Pointer to the '$' character in the input string.
 * @param[out] out_buf Buffer where the expanded value should be written.
 * @param[in] max_len Maximum allowed capacity of out_buf.
 * @param[in] ctx Pointer to the active ShellContext providing last_exit_status.
 * @param[out] chars_consumed Number of characters read from str (e.g. length of variable name + 1).
 * @return Number of characters appended to out_buf.
 */
static int expand_variable(const char *str, char *out_buf, size_t max_len,
                           const ShellContext *ctx, int *chars_consumed) {
    *chars_consumed = 1; /* for the '$' */

    if (str[1] == '?') {
        *chars_consumed = 2;
        int exit_val = ctx ? ctx->last_exit_status : 0;
        return snprintf(out_buf, max_len, "%d", exit_val);
    }

    /* Command substitution $(...) */
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
                if (depth == 0) {
                    break;
                }
            }
            i++;
        }
        if (depth == 0) {
            *chars_consumed = i + 1; /* includes closing ')' */
            char sub_cmd[MAX_TOKEN_LEN];
            int cmd_len = i - 2;
            if (cmd_len >= MAX_TOKEN_LEN) {
                cmd_len = MAX_TOKEN_LEN - 1;
            }
            strncpy(sub_cmd, str + 2, cmd_len);
            sub_cmd[cmd_len] = '\0';
            return execute_substitution(sub_cmd, ctx, out_buf, max_len);
        }
    }

    /* Collect alphanumeric variable name */
    char var_name[256];
    int v_idx = 0;
    int i = 1;
    while (str[i] && (isalnum((unsigned char)str[i]) || str[i] == '_') && v_idx < 255) {
        var_name[v_idx++] = str[i++];
    }
    var_name[v_idx] = '\0';
    *chars_consumed = i;

    if (v_idx == 0) {
        /* Bare '$' without a valid name following it */
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

    /* Unset variables expand to an empty string */
    out_buf[0] = '\0';
    return 0;
}

/**
 * @brief Parses a single command segment into arguments and I/O redirection paths.
 *
 * Reads tokens from cmd_segment while honoring quoting rules:
 * - Single quotes ('...'): characters are treated completely literally (no variable expansion).
 * - Double quotes ("..."): whitespace is preserved, and variables ($VAR, $?) are expanded.
 * - Redirection operators: '<' (input redirection) and '>' (output redirection).
 *
 * @param[in] cmd_segment Substring containing a single command (without unquoted pipe characters).
 * @param[in] ctx Pointer to ShellContext for variable expansion ($?).
 * @param[out] cmd Pointer to Command structure to populate.
 * @return 0 on success, or -1 on syntax error (e.g., missing filename after redirection).
 */
static int parse_single_command(const char *cmd_segment, const ShellContext *ctx, Command *cmd) {
    memset(cmd, 0, sizeof(Command));

    char **argv_list = malloc(sizeof(char *) * MAX_ARGS);
    if (!argv_list) {
        return -1;
    }

    int argc = 0;
    const char *p = cmd_segment;
    char token[MAX_TOKEN_LEN];
    int token_len = 0;
    int in_token = 0;
    int next_is_input_redirect = 0;
    int next_is_output_redirect = 0;

    while (*p) {
        /* Skip leading whitespace when not inside quotes or tokens */
        if (isspace((unsigned char)*p)) {
            if (in_token) {
                token[token_len] = '\0';
                if (next_is_input_redirect) {
                    cmd->input_file = strdup(token);
                    next_is_input_redirect = 0;
                } else if (next_is_output_redirect) {
                    cmd->output_file = strdup(token);
                    next_is_output_redirect = 0;
                } else {
                    if (argc < MAX_ARGS - 1) {
                        argv_list[argc++] = strdup(token);
                    }
                }
                token_len = 0;
                in_token = 0;
            }
            p++;
            continue;
        }

        /* Check for input redirection '<' */
        if (*p == '<' && !in_token) {
            next_is_input_redirect = 1;
            p++;
            continue;
        }

        /* Check for output redirection '>' */
        if (*p == '>' && !in_token) {
            next_is_output_redirect = 1;
            p++;
            continue;
        }

        /* Handle single quote: preserve everything literally until next single quote */
        if (*p == '\'') {
            in_token = 1;
            p++; /* skip opening quote */
            while (*p && *p != '\'') {
                if (token_len < MAX_TOKEN_LEN - 1) {
                    token[token_len++] = *p;
                }
                p++;
            }
            if (*p == '\'') {
                p++; /* skip closing quote */
            }
            continue;
        }

        /* Line continuation with trailing backslash outside quotes */
        if (*p == '\\' && *(p + 1) == '\n') {
            p += 2; /* Remove \<newline> continuation */
            continue;
        }

        /* Handle backticks outside quotes */
        if (*p == '`') {
            in_token = 1;
            p++; /* skip opening backtick */
            const char *start_sub = p;
            while (*p && *p != '`') {
                p++;
            }
            size_t sub_len = p - start_sub;
            char sub_cmd[MAX_TOKEN_LEN];
            if (sub_len >= MAX_TOKEN_LEN) {
                sub_len = MAX_TOKEN_LEN - 1;
            }
            strncpy(sub_cmd, start_sub, sub_len);
            sub_cmd[sub_len] = '\0';
            if (*p == '`') {
                p++; /* skip closing backtick */
            }

            char exp_buf[MAX_TOKEN_LEN];
            int exp_len = execute_substitution(sub_cmd, ctx, exp_buf, sizeof(exp_buf));
            for (int j = 0; j < exp_len && token_len < MAX_TOKEN_LEN - 1; j++) {
                token[token_len++] = exp_buf[j];
            }
            continue;
        }

        /* Handle double quote: preserve whitespace, expand variables ($VAR, $?) and $(...) */
        if (*p == '"') {
            in_token = 1;
            p++; /* skip opening quote */
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1) == '\n') {
                    p += 2; /* Line continuation inside double quotes */
                    continue;
                } else if (*p == '$') {
                    int consumed = 0;
                    char exp_buf[MAX_TOKEN_LEN];
                    int exp_len = expand_variable(p, exp_buf, sizeof(exp_buf), ctx, &consumed);
                    for (int j = 0; j < exp_len && token_len < MAX_TOKEN_LEN - 1; j++) {
                        token[token_len++] = exp_buf[j];
                    }
                    p += consumed;
                } else if (*p == '`') {
                    p++; /* skip opening backtick */
                    const char *start_sub = p;
                    while (*p && *p != '`') {
                        p++;
                    }
                    size_t sub_len = p - start_sub;
                    char sub_cmd[MAX_TOKEN_LEN];
                    if (sub_len >= MAX_TOKEN_LEN) {
                        sub_len = MAX_TOKEN_LEN - 1;
                    }
                    strncpy(sub_cmd, start_sub, sub_len);
                    sub_cmd[sub_len] = '\0';
                    if (*p == '`') {
                        p++;
                    }

                    char exp_buf[MAX_TOKEN_LEN];
                    int exp_len = execute_substitution(sub_cmd, ctx, exp_buf, sizeof(exp_buf));
                    for (int j = 0; j < exp_len && token_len < MAX_TOKEN_LEN - 1; j++) {
                        token[token_len++] = exp_buf[j];
                    }
                } else if (*p == '\\' && *(p + 1) != '\0') {
                    /* Escape sequence inside double quotes */
                    p++;
                    if (token_len < MAX_TOKEN_LEN - 1) {
                        token[token_len++] = *p++;
                    }
                } else {
                    if (token_len < MAX_TOKEN_LEN - 1) {
                        token[token_len++] = *p;
                    }
                    p++;
                }
            }
            if (*p == '"') {
                p++; /* skip closing quote */
            }
            continue;
        }

        /* Normal character processing */
        in_token = 1;

        /* Variable expansion outside quotes */
        if (*p == '$') {
            int consumed = 0;
            char exp_buf[MAX_TOKEN_LEN];
            int exp_len = expand_variable(p, exp_buf, sizeof(exp_buf), ctx, &consumed);
            for (int j = 0; j < exp_len && token_len < MAX_TOKEN_LEN - 1; j++) {
                token[token_len++] = exp_buf[j];
            }
            p += consumed;
            continue;
        }

        /* Check for escaped character */
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
        }

        if (token_len < MAX_TOKEN_LEN - 1) {
            token[token_len++] = *p;
        }
        p++;
    }

    /* Flush any pending token */
    if (in_token) {
        token[token_len] = '\0';
        if (next_is_input_redirect) {
            cmd->input_file = strdup(token);
            next_is_input_redirect = 0;
        } else if (next_is_output_redirect) {
            cmd->output_file = strdup(token);
            next_is_output_redirect = 0;
        } else {
            if (argc < MAX_ARGS - 1) {
                argv_list[argc++] = strdup(token);
            }
        }
    }

    /* Check for syntax error where redirection target was omitted */
    if (next_is_input_redirect || next_is_output_redirect) {
        fprintf(stderr, "terminator: syntax error near unexpected token 'newline'\n");
        for (int i = 0; i < argc; i++) {
            free(argv_list[i]);
        }
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
 * If found, strips the '&' from the buffer and returns 1.
 * Ignores '&&' or '&' inside quotes or command substitutions.
 *
 * @param[in,out] line_buf Buffer containing the command string.
 * @return 1 if background operator '&' was stripped, 0 otherwise.
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
            /* Check if not part of '&&' */
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
 *
 * Inspects the input line for pipe ('|') characters that are not inside quotes,
 * divides the string into segments, and calls parse_single_command() on each segment.
 *
 * @param[in] line The full raw input string typed by the user.
 * @param[in] ctx Pointer to the active ShellContext (used for variable expansions like $?).
 * @param[out] pipeline Pointer to a Pipeline structure that will receive the parsed commands.
 * @return 0 on success, or -1 on syntax error or empty command.
 */
int parse_line(const char *line, const ShellContext *ctx, Pipeline *pipeline) {
    if (!line || !pipeline) {
        return -1;
    }

    pipeline->commands = NULL;
    pipeline->num_commands = 0;
    pipeline->is_background = 0;

    char line_copy[MAX_TOKEN_LEN];
    snprintf(line_copy, sizeof(line_copy), "%s", line);

    if (check_and_strip_background(line_copy)) {
        pipeline->is_background = 1;
    }

    /* First pass: split line into command segments on unquoted '|' */
    int capacity = 8;
    char **segments = malloc(sizeof(char *) * capacity);
    if (!segments) {
        return -1;
    }

    int seg_count = 0;
    const char *start = line_copy;
    const char *p = line_copy;
    int in_single_quote = 0;
    int in_double_quote = 0;
    int in_backtick = 0;
    int subshell_depth = 0;

    while (*p) {
        if (*p == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (*p == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (!in_single_quote && *p == '`') {
            in_backtick = !in_backtick;
        } else if (!in_single_quote && *p == '$' && *(p + 1) == '(') {
            subshell_depth++;
            p++;
        } else if (!in_single_quote && subshell_depth > 0 && *p == ')') {
            subshell_depth--;
        } else if (*p == '|' && !in_single_quote && !in_double_quote && subshell_depth == 0 && !in_backtick) {
            /* Found pipe operator outside quotes and command substitutions */
            size_t len = p - start;
            char *seg = malloc(len + 1);
            if (len > 0) {
                memcpy(seg, start, len);
            }
            seg[len] = '\0';

            if (seg_count >= capacity) {
                capacity *= 2;
                segments = realloc(segments, sizeof(char *) * capacity);
            }
            segments[seg_count++] = seg;
            start = p + 1;
        }
        p++;
    }

    /* Append the trailing segment */
    size_t len = p - start;
    char *seg = malloc(len + 1);
    if (len > 0) {
        memcpy(seg, start, len);
    }
    seg[len] = '\0';

    if (seg_count >= capacity) {
        capacity *= 2;
        segments = realloc(segments, sizeof(char *) * capacity);
    }
    segments[seg_count++] = seg;

    /* Allocate pipeline command structures */
    pipeline->commands = malloc(sizeof(Command) * seg_count);
    if (!pipeline->commands) {
        for (int i = 0; i < seg_count; i++) free(segments[i]);
        free(segments);
        return -1;
    }
    pipeline->num_commands = seg_count;

    /* Parse each segment individually */
    for (int i = 0; i < seg_count; i++) {
        if (parse_single_command(segments[i], ctx, &pipeline->commands[i]) != 0) {
            /* On syntax error, free previously allocated commands */
            for (int j = 0; j <= i; j++) {
                free_command(&pipeline->commands[j]);
            }
            free(pipeline->commands);
            pipeline->commands = NULL;
            pipeline->num_commands = 0;

            for (int k = i; k < seg_count; k++) free(segments[k]);
            free(segments);
            return -1;
        }
        free(segments[i]);
    }

    free(segments);
    return 0;
}
