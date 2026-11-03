#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

static SharedHistory *g_shared_history = NULL;
static int g_is_process_mode = 0;
static char g_shm_name[64] = "";
static char g_hist_file[1024] = "";

static void resolve_histfile_path(char *out_path, size_t max_len) {
    const char *hf = getenv("TERMINATOR_HISTFILE");
    if (hf && hf[0] != '\0') {
        snprintf(out_path, max_len, "%s", hf);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out_path, max_len, "%s/.terminator_history", home ? home : "/tmp");
}

size_t history_encode_line(const char *raw, char *enc, size_t max) {
    if (!raw || !enc || max < 2) return 0;
    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 3 < max; i++) {
        if (raw[i] == '\\') {
            enc[o++] = '\\';
            enc[o++] = '\\';
        } else if (raw[i] == '\n') {
            enc[o++] = '\\';
            enc[o++] = 'n';
        } else {
            enc[o++] = raw[i];
        }
    }
    enc[o++] = '\n';
    enc[o] = '\0';
    return o;
}

size_t history_decode_line(const char *enc, size_t len, char *dec, size_t max) {
    if (!enc || !dec || max == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < max; i++) {
        if (enc[i] == '\\' && i + 1 < len) {
            if (enc[i + 1] == 'n') {
                dec[o++] = '\n';
                i++;
            } else if (enc[i + 1] == '\\') {
                dec[o++] = '\\';
                i++;
            } else {
                dec[o++] = enc[i];
            }
        } else if (enc[i] != '\n' && enc[i] != '\r') {
            dec[o++] = enc[i];
        }
    }
    dec[o] = '\0';
    return o;
}

static void history_load_from_disk(void) {
    if (!g_shared_history || g_hist_file[0] == '\0') return;

    int fd = open(g_hist_file, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return;
    fchmod(fd, 0600);

    FILE *fp = fdopen(fd, "r+");
    if (!fp) {
        close(fd);
        return;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    size_t total_lines = 0;

    while ((linelen = getline(&line, &linecap, fp)) > 0) {
        total_lines++;
    }

    rewind(fp);
    size_t skip = 0;
    if (total_lines > 20000) {
        skip = total_lines - 10000;
    }

    size_t cur_line = 0;
    char dec[MAX_HISTORY_CMD_LEN];
    while ((linelen = getline(&line, &linecap, fp)) > 0) {
        cur_line++;
        if (cur_line <= skip) continue;
        history_decode_line(line, (size_t)linelen, dec, sizeof(dec));
        if (dec[0] == '\0') continue;

        int slot = g_shared_history->head;
        snprintf(g_shared_history->commands[slot], MAX_HISTORY_CMD_LEN, "%s", dec);
        g_shared_history->head = (g_shared_history->head + 1) % MAX_HISTORY_COMMANDS;
        if (g_shared_history->count < MAX_HISTORY_COMMANDS) {
            g_shared_history->count++;
        }
    }
    free(line);

    if (total_lines > 20000) {
        char tmp_path[1200];
        snprintf(tmp_path, sizeof(tmp_path), "%s.compact_tmp", g_hist_file);
        FILE *tfp = fopen(tmp_path, "w");
        if (tfp) {
            fchmod(fileno(tfp), 0600);
            for (int i = 0; i < g_shared_history->count; i++) {
                int idx = (g_shared_history->head - g_shared_history->count + i + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
                char enc[MAX_HISTORY_CMD_LEN * 2];
                size_t elen = history_encode_line(g_shared_history->commands[idx], enc, sizeof(enc));
                fwrite(enc, 1, elen, tfp);
            }
            fclose(tfp);
            rename(tmp_path, g_hist_file);
        }
    }

    fclose(fp);
}

int history_init(int use_process_mode) {
    g_is_process_mode = use_process_mode;
    resolve_histfile_path(g_hist_file, sizeof(g_hist_file));

    if (use_process_mode) {
        snprintf(g_shm_name, sizeof(g_shm_name), "/terminator_shm_%d", (int)getpid());
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

    history_load_from_disk();
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

    const char *p = cmd;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    if (*p == '\0') return;

    sem_wait(&g_shared_history->sem);

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

    size_t len = strlen(g_shared_history->commands[slot]);
    while (len > 0 && (g_shared_history->commands[slot][len - 1] == '\n' ||
                       g_shared_history->commands[slot][len - 1] == '\r')) {
        g_shared_history->commands[slot][--len] = '\0';
    }

    g_shared_history->head = (g_shared_history->head + 1) % MAX_HISTORY_COMMANDS;
    if (g_shared_history->count < MAX_HISTORY_COMMANDS) {
        g_shared_history->count++;
    }

    if (g_hist_file[0] != '\0') {
        int fd = open(g_hist_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            char enc[MAX_HISTORY_CMD_LEN * 2];
            size_t elen = history_encode_line(cmd, enc, sizeof(enc));
            if (write(fd, enc, elen) < 0) {}
            close(fd);
        }
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
    int start_offset = show_count - 1;

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

static int compute_lcs_len(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int l1 = (int)strlen(s1);
    int l2 = (int)strlen(s2);
    if (l1 == 0 || l2 == 0) return 0;

    int max_lcs = 0;
    static int dp[2][MAX_HISTORY_CMD_LEN + 1];
    memset(dp, 0, sizeof(dp));

    for (int i = 1; i <= l1; i++) {
        int cur = i % 2;
        int prev = (i - 1) % 2;
        for (int j = 1; j <= l2 && j <= MAX_HISTORY_CMD_LEN; j++) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[cur][j] = dp[prev][j - 1] + 1;
                if (dp[cur][j] > max_lcs) max_lcs = dp[cur][j];
            } else {
                dp[cur][j] = 0;
            }
        }
    }
    return max_lcs;
}

void hist_search_result_free(HistSearchResult *res) {
    if (!res) return;
    if (res->entries) {
        for (size_t i = 0; i < res->count; i++) {
            free(res->entries[i]);
        }
        free(res->entries);
        res->entries = NULL;
    }
    res->count = 0;
    res->match_type = HIST_MATCH_NONE;
    res->match_len = 0;
}

int history_search(const char *term, HistSearchResult *res) {
    if (!res) return -1;
    res->match_type = HIST_MATCH_NONE;
    res->match_len = 0;
    res->count = 0;
    res->entries = NULL;

    if (!g_shared_history || !term) return 0;
    while (*term && isspace((unsigned char)*term)) term++;
    if (*term == '\0') return 0;

    sem_wait(&g_shared_history->sem);
    if (g_shared_history->count == 0) {
        sem_post(&g_shared_history->sem);
        return 0;
    }

    /* Rule (a): Exact match (most recent) */
    for (int i = 0; i < g_shared_history->count; i++) {
        int idx = (g_shared_history->head - 1 - i + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
        if (strcmp(g_shared_history->commands[idx], term) == 0) {
            res->match_type = HIST_MATCH_EXACT;
            res->match_len = (int)strlen(term);
            res->count = 1;
            res->entries = (char **)malloc(sizeof(char *));
            if (res->entries) {
                res->entries[0] = strdup(g_shared_history->commands[idx]);
            }
            sem_post(&g_shared_history->sem);
            return 0;
        }
    }

    /* Rule (b): Longest Common Substring > 2 */
    if (g_shared_history->count <= 0) {
        sem_post(&g_shared_history->sem);
        return 0;
    }
    size_t total = (size_t)g_shared_history->count;
    int best_lcs = 0;
    int *lcs_arr = (int *)malloc(sizeof(int) * total);
    if (!lcs_arr) {
        sem_post(&g_shared_history->sem);
        return 0;
    }

    for (size_t i = 0; i < total; i++) {
        int idx = (g_shared_history->head - 1 - (int)i + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
        int lcs = compute_lcs_len(term, g_shared_history->commands[idx]);
        lcs_arr[i] = lcs;
        if (lcs > best_lcs) best_lcs = lcs;
    }

    if (best_lcs > 2) {
        res->match_type = HIST_MATCH_SUBSTRING;
        res->match_len = best_lcs;
        res->entries = (char **)malloc(sizeof(char *) * total);
        if (res->entries) {
            size_t o = 0;
            for (size_t i = 0; i < total; i++) {
                if (lcs_arr[i] == best_lcs) {
                    int idx = (g_shared_history->head - 1 - (int)i + (2 * MAX_HISTORY_COMMANDS)) % MAX_HISTORY_COMMANDS;
                    const char *cand = g_shared_history->commands[idx];
                    bool dup = false;
                    for (size_t k = 0; k < o; k++) {
                        if (strcmp(res->entries[k], cand) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        res->entries[o++] = strdup(cand);
                    }
                }
            }
            res->count = o;
        }
    } else {
        res->match_type = HIST_MATCH_NONE;
    }

    free(lcs_arr);
    sem_post(&g_shared_history->sem);
    return 0;
}
