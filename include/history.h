#ifndef HISTORY_H
#define HISTORY_H

#include <stddef.h>
#include <semaphore.h>

#define MAX_HISTORY_COMMANDS 10000
#define MAX_HISTORY_CMD_LEN 1024

/**
 * @brief Layout of the shared history structure.
 *
 * In Process Mode, this struct resides in POSIX shared memory (shm_open + mmap).
 * In Thread Mode, it resides in shared process heap memory.
 * Synchronization across all threads or processes is guaranteed via a POSIX semaphore.
 */
typedef struct {
    sem_t sem;                                          /**< Process-shared or thread-shared POSIX semaphore */
    int count;                                          /**< Number of valid commands stored (up to MAX_HISTORY_COMMANDS) */
    int head;                                           /**< Next insertion index in the ring buffer */
    char commands[MAX_HISTORY_COMMANDS][MAX_HISTORY_CMD_LEN]; /**< Command history ring buffer */
} SharedHistory;

/**
 * @brief Initializes the shared history subsystem.
 *
 * @param[in] use_process_mode 1 for Process Mode (uses POSIX shm), 0 for Thread Mode (heap memory).
 * @return 0 on success, -1 on failure.
 */
int history_init(int use_process_mode);

/**
 * @brief Cleans up and unmaps/frees shared history resources.
 */
void history_cleanup(void);

/**
 * @brief Adds a command to the shared history ring buffer.
 *
 * Automatically skips consecutive identical commands.
 *
 * @param[in] cmd The command string to add.
 */
void history_add(const char *cmd);

/**
 * @brief Retrieves a command from shared history relative to the newest entry.
 *
 * @param[in] offset_from_newest 0 for most recent command, 1 for previous, etc.
 * @param[out] out_buf Destination buffer.
 * @param[in] max_len Capacity of out_buf.
 * @return 0 on success, -1 if offset is out of range.
 */
int history_get_relative(int offset_from_newest, char *out_buf, size_t max_len);

/**
 * @brief Returns the total number of commands currently stored in shared history.
 */
int history_count(void);

/**
 * @brief Formats up to `max_cmds` most recent history entries into out_buf.
 *
 * @param[out] out_buf Output buffer.
 * @param[in] max_len Maximum length of out_buf.
 * @param[in] max_cmds Maximum number of recent entries to print (e.g. 1000).
 * @return Number of bytes written.
 */
int history_format_recent(char *out_buf, size_t max_len, int max_cmds);

#endif /* HISTORY_H */
