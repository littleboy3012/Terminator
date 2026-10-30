#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>
#include <stddef.h>

#define MAX_JOBS 32
#define MAX_CMD_LEN 512

/**
 * @brief Represents the lifecycle execution state of a background or suspended job.
 */
typedef enum {
    JOB_STATE_RUNNING,  /**< Actively executing in the background */
    JOB_STATE_STOPPED,  /**< Suspended via SIGTSTP (Ctrl+Z) */
    JOB_STATE_DONE      /**< Terminated and waiting to be reported to user */
} JobState;

/**
 * @brief Represents an individual tracked job.
 */
typedef struct {
    int id;                     /**< Sequential job identifier (1, 2, 3...) */
    pid_t pgid;                 /**< Process group ID (for group signaling with kill(-pgid, ...)) */
    pid_t pid;                  /**< Process ID of the group leader */
    JobState state;             /**< Current state: RUNNING, STOPPED, or DONE */
    char command[MAX_CMD_LEN];  /**< Command line string as typed by the user */
    int out_pipe_fd;            /**< Read-end of pipe capturing stdout/stderr (-1 if closed) */
    int exit_status;            /**< Exit code or terminating signal number when DONE */
} Job;

/**
 * @brief Table maintaining all active background and stopped jobs in a shell session or tab.
 */
typedef struct {
    Job jobs[MAX_JOBS];         /**< Array of active tracked jobs */
    int num_jobs;               /**< Current count of active tracked jobs */
    int current_job_id;         /**< ID of the most recent active job (marked with '+') */
    int previous_job_id;        /**< ID of the second most recent active job (marked with '-') */
} JobTable;

/**
 * @brief Initializes a JobTable structure.
 *
 * @param[out] table Pointer to JobTable to initialize.
 */
void jobs_init(JobTable *table);

/**
 * @brief Adds a new job to the table.
 *
 * Automatically assigns the lowest available positive integer ID and updates
 * current_job_id and previous_job_id.
 *
 * @param[in,out] table Pointer to JobTable.
 * @param[in] pgid Process group ID.
 * @param[in] pid Leader process ID.
 * @param[in] cmd Command string.
 * @param[in] state Initial state (JOB_STATE_RUNNING or JOB_STATE_STOPPED).
 * @param[in] pipe_fd Non-blocking pipe read-end (-1 if none).
 * @return Pointer to newly created Job, or NULL if table is full.
 */
Job *jobs_add(JobTable *table, pid_t pgid, pid_t pid, const char *cmd, JobState state, int pipe_fd);

/**
 * @brief Searches for a job by its numeric identifier.
 *
 * @param[in] table Pointer to JobTable.
 * @param[in] id Job ID (e.g. 1).
 * @return Pointer to matching Job, or NULL if not found.
 */
Job *jobs_find_by_id(JobTable *table, int id);

/**
 * @brief Searches for a job by its process group ID or leader PID.
 *
 * @param[in] table Pointer to JobTable.
 * @param[in] pid Process ID or PGID.
 * @return Pointer to matching Job, or NULL if not found.
 */
Job *jobs_find_by_pid(JobTable *table, pid_t pid);

/**
 * @brief Retrieves the current job (marked with '+').
 *
 * @param[in] table Pointer to JobTable.
 * @return Pointer to current Job, or NULL if table is empty.
 */
Job *jobs_get_current(JobTable *table);

/**
 * @brief Removes a job from the table by its ID.
 *
 * Re-evaluates current_job_id and previous_job_id accordingly.
 *
 * @param[in,out] table Pointer to JobTable.
 * @param[in] id Job ID to remove.
 * @return 0 on success, -1 if job was not found.
 */
int jobs_remove(JobTable *table, int id);

/**
 * @brief Terminates all active jobs in the table (e.g. on shell exit or tab close).
 *
 * Sends SIGTERM then SIGKILL to each job's process group and reaps child processes.
 *
 * @param[in,out] table Pointer to JobTable.
 */
void jobs_kill_all(JobTable *table);

/**
 * @brief Generates formatted output for the built-in 'jobs' command.
 *
 * Output format matches course specifications:
 * [1]+  Stopped                 sleep 100
 * [2]-  Running                 python3 app.py &
 *
 * @param[in] table Pointer to JobTable.
 * @param[out] out_buf Destination buffer for formatted text.
 * @param[in] max_len Capacity of out_buf.
 */
void jobs_print(const JobTable *table, char *out_buf, size_t max_len);

/**
 * @brief Non-blocking check of all jobs in the table using waitpid(..., WNOHANG | WUNTRACED | WCONTINUED).
 *
 * Updates job states, closes finished pipes, and formats notifications (e.g. "[1]+  Done  sleep 3\n")
 * into notification_buf for display to the user.
 *
 * @param[in,out] table Pointer to JobTable.
 * @param[out] notification_buf Destination buffer for termination notifications.
 * @param[in] max_len Capacity of notification_buf.
 */
void jobs_check_status(JobTable *table, char *notification_buf, size_t max_len);

/**
 * @brief Implements the 'jobs' built-in command.
 *
 * @param[in] table Pointer to active JobTable.
 * @param[in] argv Argument vector (argv[0]="jobs").
 * @param[out] out_buf Destination buffer for printed output.
 * @param[in] max_len Capacity of out_buf.
 * @return 0 on success.
 */
int jobs_builtin_jobs(JobTable *table, char **argv, char *out_buf, size_t max_len);

/**
 * @brief Implements the 'bg' built-in command to resume stopped jobs in the background.
 *
 * Sends SIGCONT to the specified job's process group, marks it as RUNNING,
 * and formats the output (e.g. "[1]+ sleep 100 &").
 *
 * @param[in,out] table Pointer to active JobTable.
 * @param[in] argv Argument vector (e.g. "bg", "%1").
 * @param[out] out_buf Destination buffer for output text.
 * @param[in] max_len Capacity of out_buf.
 * @return 0 on success, 1 on error (no such job).
 */
int jobs_builtin_bg(JobTable *table, char **argv, char *out_buf, size_t max_len);

/**
 * @brief Implements the 'fg' built-in command to bring a job to the foreground.
 *
 * Resumes the job with SIGCONT if stopped, copies its metadata into res_job,
 * and removes it from the background table.
 *
 * @param[in,out] table Pointer to active JobTable.
 * @param[in] argv Argument vector (e.g. "fg", "%1").
 * @param[out] out_buf Destination buffer for output text (e.g. error message).
 * @param[in] max_len Capacity of out_buf.
 * @param[out] res_job Pointer to receive the Job metadata for foreground execution.
 * @return 0 on success, 1 on error.
 */
int jobs_builtin_fg(JobTable *table, char **argv, char *out_buf, size_t max_len, Job *res_job);

#endif /* JOBS_H */
