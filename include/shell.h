#ifndef SHELL_H
#define SHELL_H

#include <stddef.h>
#include <limits.h>
#include "jobs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_REDIRS 16

/**
 * @brief Redirection types supported by Terminator.
 */
typedef enum {
    REDIR_IN = 0,
    REDIR_OUT = 1,
    REDIR_APPEND = 2,
    REDIR_DUP = 3
} RedirType;

/**
 * @brief Represents an individual file descriptor redirection.
 */
typedef struct {
    RedirType type;
    int fd;
    int dup_target;
    char filename[PATH_MAX];
} Redir;

/**
 * @brief Structure representing a single parsed command,
 *        including its arguments and I/O redirection targets.
 */
typedef struct {
    char **argv;          /**< NULL-terminated array of argument strings (argv[0] is the command name) */
    int argc;             /**< Number of arguments in argv */
    char *input_file;     /**< Path to file for input redirection ('<'), or NULL if none */
    char *output_file;    /**< Path to file for output redirection ('>'), or NULL if none */
    int is_append;        /**< Non-zero if output redirection is in append mode ('>>') */
    int nredirs;          /**< Number of active redirections */
    Redir redirs[16];     /**< Array of individual redirections */
} Command;

/**
 * @brief Structure representing an entire pipeline of commands (e.g. cmd1 | cmd2 | cmd3).
 */
typedef struct {
    union {
        Command *commands;/**< Array of Command structures in the pipeline */
        Command *cmds;    /**< Alias for commands */
    };
    union {
        int num_commands; /**< Total number of commands connected by pipes */
        int n;            /**< Alias for num_commands */
    };
    int is_background;    /**< Non-zero if pipeline should execute in the background (trailing '&') */
} Pipeline;

/**
 * @brief Structure maintaining the persistent state of an individual shell instance / tab.
 */
typedef struct {
    int last_exit_status; /**< Exit status of the most recently executed foreground command ($?) */
    int should_exit;      /**< Non-zero if the shell received an 'exit' command and should shut down */
    char cwd[PATH_MAX];   /**< Current working directory of this shell session */
    char prev_cwd[PATH_MAX]; /**< Previous working directory for 'cd -' support */
    JobTable job_table;   /**< Active background and stopped jobs in this shell session (Task 8) */
    pid_t foreground_pid; /**< Active foreground process PID (0 if idle) */
    char foreground_cmd[MAX_CMD_LEN]; /**< Command line string of active foreground process */
    int script_argc;      /**< Positional parameter count ($#) for active bash script */
    char **script_argv;   /**< Positional parameter array ($1..$9) for active bash script */
    char script_name[PATH_MAX]; /**< Script name / path ($0) for active bash script */
} ShellContext;

/**
 * @brief Initializes a ShellContext structure to its default starting values.
 *
 * @param[out] ctx Pointer to the ShellContext to initialize. Must not be NULL.
 * @return 0 on success, or -1 if ctx is NULL or getting the initial cwd fails.
 */
int shell_init(ShellContext *ctx);

/**
 * @brief Cleans up any resources held by a ShellContext before destruction.
 *
 * @param[in,out] ctx Pointer to the ShellContext to clean up.
 */
void shell_cleanup(ShellContext *ctx);

/**
 * @brief Executes a complete pipeline of commands with support for I/O redirection and piping.
 *
 * @param[in,out] ctx Pointer to the active ShellContext (used for cwd, built-ins, and storing $?).
 * @param[in] pipeline Pointer to the Pipeline structure describing commands to run.
 * @return Exit status of the last command in the pipeline (or built-in status).
 */
int shell_execute_pipeline(ShellContext *ctx, const Pipeline *pipeline);

/**
 * @brief Executes a single Command, either as a built-in function or by forking an external executable.
 *
 * @param[in,out] ctx Pointer to the active ShellContext.
 * @param[in] cmd Pointer to the Command structure to execute.
 * @return Exit status code of the command (0 on success, non-zero on error).
 */
int shell_execute_command(ShellContext *ctx, const Command *cmd);

/**
 * @brief Applies I/O redirection for a command process using open() and dup2().
 *
 * @param[in] input_file Path to input redirection file ('<'), or NULL.
 * @param[in] output_file Path to output redirection file ('>'), or NULL.
 * @return 0 on success, or -1 on error.
 */
int setup_redirections(const char *input_file, const char *output_file);

/**
 * @brief Applies all redirections for a Command using its redirs array or legacy fields.
 *
 * @param[in] cmd Pointer to Command structure.
 * @return 0 on success, -1 on error.
 */
int setup_command_redirections(const Command *cmd);

/**
 * @brief Configures signal forwarding for the active foreground process.
 *
 * @param[in] fg_pid PID of the active foreground process (or 0 when idle).
 */
void shell_set_fg_signals(pid_t fg_pid);

#endif /* SHELL_H */
