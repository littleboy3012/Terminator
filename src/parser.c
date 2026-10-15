#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ARGS 128
#define MAX_TOKEN_LEN 4096

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

        /* Handle double quote: preserve whitespace, expand variables ($VAR, $?) */
        if (*p == '"') {
            in_token = 1;
            p++; /* skip opening quote */
            while (*p && *p != '"') {
                if (*p == '$') {
                    int consumed = 0;
                    char exp_buf[MAX_TOKEN_LEN];
                    int exp_len = expand_variable(p, exp_buf, sizeof(exp_buf), ctx, &consumed);
                    for (int j = 0; j < exp_len && token_len < MAX_TOKEN_LEN - 1; j++) {
                        token[token_len++] = exp_buf[j];
                    }
                    p += consumed;
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

    /* First pass: split line into command segments on unquoted '|' */
    int capacity = 8;
    char **segments = malloc(sizeof(char *) * capacity);
    if (!segments) {
        return -1;
    }

    int seg_count = 0;
    const char *start = line;
    const char *p = line;
    int in_single_quote = 0;
    int in_double_quote = 0;

    while (*p) {
        if (*p == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (*p == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (*p == '|' && !in_single_quote && !in_double_quote) {
            /* Found pipe operator outside quotes */
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

            for (int k = 0; k < seg_count; k++) free(segments[k]);
            free(segments);
            return -1;
        }
        free(segments[i]);
    }

    free(segments);
    return 0;
}
