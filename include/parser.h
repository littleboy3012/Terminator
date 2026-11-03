#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

/**
 * @brief Parses a raw command line string into an executable Pipeline structure.
 *
 * This function handles:
 * - Splitting on the pipe character ('|') into individual commands.
 * - Tokenizing arguments by whitespace while respecting quotes.
 * - Recognizing input redirection ('<') and output redirection ('>').
 * - Expanding special variables such as "$?" using the provided ShellContext.
 *
 * @param[in] line The raw input string typed by the user (or read from input).
 * @param[in] ctx Pointer to the active ShellContext (used for variable expansions like $?).
 * @param[out] pipeline Pointer to a Pipeline structure to be populated.
 * @return 0 on successful parsing, or -1 on syntax error (e.g., missing redirection target).
 */
int parse_line(const char *line, const ShellContext *ctx, Pipeline *pipeline);

/**
 * @brief Checks if a command string is incomplete and requires continuation lines.
 *
 * Checks for:
 * - Trailing unescaped backslash ('\\')
 * - Unclosed single quotes ('\'')
 * - Unclosed double quotes ('\"')
 * - Unclosed command substitution ("$(...)" or backticks)
 *
 * @param[in] line The accumulated command string entered so far.
 * @return 1 if command is incomplete (continuation prompt needed), 0 if complete.
 */
int is_command_incomplete(const char *line);

/**
 * @brief Expands environment variables, "$?", "$0", "$#", "$1".."$9", or "${VAR}".
 */
int expand_variable(const char *str, char *out_buf, size_t max_len,
                    const ShellContext *ctx, int *chars_consumed);

/**
 * @brief Deallocates all dynamically allocated memory within a Pipeline structure.
 *
 * Must be called after executing a pipeline to avoid memory leaks.
 *
 * @param[in,out] pipeline Pointer to the Pipeline whose memory should be freed.
 */
void free_pipeline(Pipeline *pipeline);

/**
 * @brief Deallocates all memory associated with an individual Command structure.
 *
 * @param[in,out] cmd Pointer to the Command structure to free.
 */
void free_command(Command *cmd);

#endif /* PARSER_H */
