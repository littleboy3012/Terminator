#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

static SharedHistory *g_shared_history = NULL;
static int g_is_process_mode = 0;
static char g_shm_name[64] = "";

int history_init(int use_process_mode) {
    g_is_process_mode = use_process_mode;

    if (use_process_mode) {
        snprintf(g_shm_name, sizeof(g_shm_name), "/terminator_shm_%d", (int)getpid());
        /* Remove any stale SHM object with same name */
        shm_unlink(g_shm_name);

        int fd = shm_open(g_shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd < 0) {
            perror("terminator: shm_open failed");
            return -1;
        }

        if (ftruncate(fd, sizeof(SharedHistory)) != 0) {
            perror("terminator: ftruncate failed");
            close(fd);
            shm_unlink(g_shm_name);
            return -1;
        }

        g_shared_history = mmap(NULL, sizeof(SharedHistory),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        if (g_shared_history == MAP_FAILED) {
            perror("terminator: mmap failed");
            shm_unlink(g_shm_name);
            g_shared_history = NULL;
            return -1;
        }

        memset(g_shared_history, 0, sizeof(SharedHistory));
        if (sem_init(&g_shared_history->sem, 1, 1) != 0) {
            perror("terminator: sem_init (process-shared) failed");
            munmap(g_shared_history, sizeof(SharedHistory));
            shm_unlink(g_shm_name);
            g_shared_history = NULL;
            return -1;
        }
    } else {
        /* Thread Mode: allocate in shared process memory with thread-shared semaphore */
        g_shared_history = malloc(sizeof(SharedHistory));
        if (!g_shared_history) {
            perror("terminator: malloc for SharedHistory failed");
            return -1;
        }

        memset(g_shared_history, 0, sizeof(SharedHistory));
        if (sem_init(&g_shared_history->sem, 0, 1) != 0) {
            perror("terminator: sem_init (thread-shared) failed");
            free(g_shared_history);
            g_shared_history = NULL;
            return -1;
        }
    }

    g_shared_history->count = 0;
    g_shared_history->head = 0;
    return 0;
}

void history_cleanup(void) {
    if (!g_shared_history) return;

    sem_destroy(&g_shared_history->sem);

    if (g_is_process_mode) {
        munmap(g_shared_history, sizeof(SharedHistory));
        if (g_shm_name[0] != '\0') {
            shm_unlink(g_shm_name);
            g_shm_name[0] = '\0';
        }
    } else {
        free(g_shared_history);
    }
    g_shared_history = NULL;
}

void history_add(const char *cmd) {
    if (!g_shared_history || !cmd || cmd[0] == '\0') return;

    /* Trim leading and trailing whitespace */
    while (*cmd && (*cmd == ' ' || *cmd == '\t' || *cmd == '\n' || *cmd == '\r')) {
        cmd++;
    }
    if (cmd[0] == '\0') return;

    sem_wait(&g_shared_history->sem);

    /* Do not insert consecutive identical commands */
    if (g_shared_history->count > 0) {
        int prev_idx = (g_shared_history->head - 1 + MAX_HISTORY_COMMANDS) % MAX_HISTORY_COMMANDS;
        if (strncmp(g_shared_history->commands[prev_idx], cmd, MAX_HISTORY_CMD_LEN - 1) == 0) {
            sem_post(&g_shared_history->sem);
            return;
        }
    }

    int slot = g_shared_history->head;
    strncpy(g_shared_history->commands[slot], cmd, MAX_HISTORY_CMD_LEN - 1);
    g_shared_history->commands[slot][MAX_HISTORY_CMD_LEN - 1] = '\0';

    /* Strip trailing newlines */
    size_t len = strlen(g_shared_history->commands[slot]);
    while (len > 0 && (g_shared_history->commands[slot][len - 1] == '\n' ||
                       g_shared_history->commands[slot][len - 1] == '\r')) {
        g_shared_history->commands[slot][--len] = '\0';
    }

    g_shared_history->head = (g_shared_history->head + 1) % MAX_HISTORY_COMMANDS;
    if (g_shared_history->count < MAX_HISTORY_COMMANDS) {
        g_shared_history->count++;
    }

    sem_post(&g_shared_history->sem);
}

int history_get_relative(int offset_from_newest, char *out_buf, size_t max_len) {
    if (!g_shared_history || !out_buf || max_len == 0) return -1;

    sem_wait(&g_shared_history->sem);

    if (offset_from_newest < 0 || offset_from_newest >= g_shared_history->count) {
        sem_post(&g_shared_history->sem);
        return -1;
    }

    int idx = (g_shared_history->head - 1 - offset_from_newest + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
    strncpy(out_buf, g_shared_history->commands[idx], max_len - 1);
    out_buf[max_len - 1] = '\0';

    sem_post(&g_shared_history->sem);
    return 0;
}

int history_count(void) {
    if (!g_shared_history) return 0;
    sem_wait(&g_shared_history->sem);
    int count = g_shared_history->count;
    sem_post(&g_shared_history->sem);
    return count;
}

int history_format_recent(char *out_buf, size_t max_len, int max_cmds) {
    if (!g_shared_history || !out_buf || max_len == 0) return 0;

    sem_wait(&g_shared_history->sem);

    int total = g_shared_history->count;
    int show_count = total < max_cmds ? total : max_cmds;
    int start_offset = show_count - 1; /* from oldest to newest */

    size_t written = 0;
    out_buf[0] = '\0';

    for (int off = start_offset; off >= 0; off--) {
        int idx = (g_shared_history->head - 1 - off + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
        int num = total - off;
        int n = snprintf(out_buf + written, max_len - written, "%5d  %s\n", num, g_shared_history->commands[idx]);
        if (n < 0 || (size_t)n >= max_len - written) {
            break;
        }
        written += (size_t)n;
    }

    sem_post(&g_shared_history->sem);
    return (int)written;
}
