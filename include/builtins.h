#ifndef BUILTINS_H
#define BUILTINS_H

#include "shell.h"

/**
 * @brief Checks whether a given command name corresponds to an internal shell built-in command.
 *
 * @param[in] name The command name string to check (e.g. "cd", "exit").
 * @return 1 (true) if the command is a recognized built-in, 0 (false) otherwise.
 */
int is_builtin_command(const char *name);

/**
 * @brief Executes a recognized built-in command directly in the shell process.
 *
 * Built-in commands modify the state of the shell itself (e.g., current directory via chdir)
 * and therefore cannot be run inside a separate child process.
 *
 * @param[in,out] ctx Pointer to the active ShellContext whose state may be modified.
 * @param[in] cmd Pointer to the Command structure containing the built-in name and arguments.
 * @return Exit status code of the built-in command (0 for success, non-zero on failure).
 */
int execute_builtin(ShellContext *ctx, const Command *cmd);

/**
 * @brief Executes a built-in command and captures its textual output into a buffer.
 *
 * Used by GUI mode and Process Mode to route built-in output to visual tabs or IPC pipes.
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] cmd Pointer to Command structure.
 * @param[out] out_buf Destination text buffer for formatted output.
 * @param[in] max_len Maximum bytes to write to out_buf.
 * @return Exit status code.
 */
int execute_builtin_captured(ShellContext *ctx, const Command *cmd, char *out_buf, size_t max_len);

/**
 * @brief Built-in command: Executes commands from a script file ('source').
 *
 * @param[in,out] ctx Pointer to the ShellContext.
 * @param[in] argv Argument array (argv[0]="source", argv[1]=script path).
 * @return 0 on success, non-zero on failure.
 */
int builtin_source(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Changes the current working directory ('cd').
 *
 * If no arguments are provided, changes directory to the user's HOME directory.
 * Also supports relative paths and tilde ('~') expansion.
 *
 * @param[in,out] ctx Pointer to the ShellContext to update with the new working directory.
 * @param[in] argv NULL-terminated array of arguments (argv[0]="cd", argv[1]=target directory).
 * @return 0 on success, or 1 if the directory change failed (e.g., path not found).
 */
int builtin_cd(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Exits the shell session ('exit').
 *
 * Sets the should_exit flag in the ShellContext and optionally records the specified exit code.
 *
 * @param[in,out] ctx Pointer to the ShellContext to mark for termination.
 * @param[in] argv NULL-terminated array of arguments (argv[0]="exit", optional argv[1]=exit code).
 * @return The integer exit status code requested by the user, or last_exit_status if none given.
 */
int builtin_exit(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Clears the terminal screen ('clear').
 *
 * In headless mode, prints ANSI escape sequences to clear the terminal.
 * In GUI mode, this is intercepted by tab_submit_command to wipe the tab buffer.
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] argv NULL-terminated array of arguments (argv[0]="clear").
 * @return 0 on success.
 */
int builtin_clear(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Runs commands in parallel with multiWatch.
 *
 * Syntax: multiWatch ["cmd1", "cmd2", ...]
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] argc Argument count.
 * @param[in] argv Argument array.
 * @return 0 on completion/interruption, 1 on error.
 */
int builtin_multiwatch(ShellContext *ctx, int argc, char **argv);

/**
 * @brief Built-in command: Lists active background and stopped jobs ('jobs').
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] argv Argument array.
 * @return 0 on success.
 */
int builtin_jobs(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Resumes a stopped job in the background ('bg').
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] argv Argument array (e.g. "bg", "%1").
 * @return 0 on success, non-zero on error.
 */
int builtin_bg(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Brings a job to the foreground ('fg').
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] argv Argument array (e.g. "fg", "%1").
 * @return 0 on success, non-zero on error.
 */
int builtin_fg(ShellContext *ctx, char **argv);

/**
 * @brief Built-in command: Displays the recent shared command history ('history').
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] argv Argument array.
 * @return 0 on success.
 */
int builtin_history(ShellContext *ctx, char **argv);

#endif /* BUILTINS_H */
