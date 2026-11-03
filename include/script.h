#ifndef SCRIPT_H
#define SCRIPT_H

#include "shell.h"

/**
 * @brief Checks if a given file path is a bash script that Terminator should interpret.
 *
 * @param[in] path File path.
 * @return 1 if it is a script file, 0 otherwise.
 */
int is_bash_script(const char *path);

/**
 * @brief Interprets and executes a bash script file internally without /bin/bash.
 *
 * Supports comments, variables, positional parameters ($0, $1..$9, $#), last exit status ($?),
 * nested if/elif/else/fi with [ ... ] tests, pipelines, redirections, and exit [n].
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] script_path Path to the script file.
 * @param[in] argc Number of arguments (including script path).
 * @param[in] argv Argument vector (argv[0]=script_path, argv[1..]=parameters).
 * @return Exit status of the script.
 */
int script_run(ShellContext *ctx, const char *script_path, int argc, char **argv);

/**
 * @brief Evaluates a '[ ... ]' test expression string.
 *
 * @param[in] ctx Pointer to the active ShellContext for variable expansion.
 * @param[in] expr Expression string between '[' and ']'.
 * @return 0 if true (success in bash), 1 if false, 2 on syntax error.
 */
int script_eval_test(ShellContext *ctx, const char *expr);

#endif /* SCRIPT_H */
