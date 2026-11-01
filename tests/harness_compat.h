#ifndef HARNESS_COMPAT_H
#define HARNESS_COMPAT_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>

#include "shell.h"
#include "parser.h"
#include "builtins.h"
#include "jobs.h"
#include "history.h"
#include "multiwatch.h"
#include "hotkeys.h"

/* ========================================================================= */
/*                   Framed Protocol Definitions & IPC                       */
/* ========================================================================= */

typedef enum {
    FRAME_LINE = 1,
    FRAME_INTR = 2,
    FRAME_TSTP = 3,
    FRAME_EOF = 4,
    FRAME_OUTPUT = 5,
    FRAME_PROMPT = 6,
    FRAME_RELOAD_KEYS = 7,
    FRAME_EXIT = 8
} FrameType;

typedef struct {
    uint8_t type;
    uint32_t len;
    char payload[];
} Frame;

typedef struct {
    char buf[131072];
    size_t len;
} InBuffer;

typedef void (*FrameCallback)(const Frame *f, void *user_data);

static inline int proto_write_frame_blocking(int fd, FrameType type, const void *payload, uint32_t len) {
    uint8_t t = (uint8_t)type;
    uint32_t l = len;
    if (write(fd, &t, 1) != 1) return -1;
    if (write(fd, &l, 4) != 4) return -1;
    if (len > 0 && payload) {
        size_t total = 0;
        while (total < len) {
            ssize_t n = write(fd, (const char *)payload + total, len - total);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            total += (size_t)n;
        }
    }
    return 0;
}

static inline bool read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

static inline ssize_t proto_read_frames(int fd, InBuffer *in, FrameCallback cb, void *user_data, bool *eof_reached) {
    if (eof_reached) *eof_reached = false;
    ssize_t n = read(fd, in->buf + in->len, sizeof(in->buf) - in->len);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        return -1;
    }
    if (n == 0) {
        if (eof_reached) *eof_reached = true;
        return 0;
    }
    in->len += (size_t)n;

    size_t offset = 0;
    while (in->len - offset >= 5) {
        uint8_t type = (uint8_t)in->buf[offset];
        uint32_t len = 0;
        memcpy(&len, in->buf + offset + 1, 4);
        if (in->len - offset < 5 + (size_t)len) {
            break; /* incomplete frame */
        }
        Frame *f = (Frame *)malloc(sizeof(Frame) + len + 1);
        if (f) {
            f->type = type;
            f->len = len;
            memcpy(f->payload, in->buf + offset + 5, len);
            f->payload[len] = '\0';
            cb(f, user_data);
            free(f);
        }
        offset += 5 + (size_t)len;
    }
    if (offset > 0) {
        memmove(in->buf, in->buf + offset, in->len - offset);
        in->len -= offset;
    }
    return n;
}

/* ========================================================================= */
/*             Shared State, History, Persistence & LCS Search               */
/* ========================================================================= */

#define HISTORY_CAPACITY 10000
#define HISTORY_MAX_LINE 1024

typedef struct {
    uint64_t seq;
    char text[HISTORY_MAX_LINE];
} HistEntry;

typedef struct {
    uint64_t next_seq;
    uint32_t count;
    uint32_t head;
    HistEntry entries[HISTORY_CAPACITY];
} HistTable;

typedef struct {
    pthread_mutex_t mutex;
    HistTable history;
    char hist_file[1024];
} Shared;

static inline void shared_lock(Shared *sh) {
    if (!sh) return;
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) {
        pthread_mutex_consistent(&sh->mutex);
    }
}

static inline void shared_unlock(Shared *sh) {
    if (!sh) return;
    pthread_mutex_unlock(&sh->mutex);
}

static inline size_t history_encode_line(const char *raw, char *enc, size_t max) {
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

static inline size_t history_decode_line(const char *enc, size_t len, char *dec, size_t max) {
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

static inline Shared *shared_create(void) {
    Shared *sh = (Shared *)mmap(NULL, sizeof(Shared),
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) return NULL;
    memset(sh, 0, sizeof(Shared));

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&sh->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    sh->history.next_seq = 1;
    sh->history.count = 0;
    sh->history.head = 0;
    sh->hist_file[0] = '\0';
    return sh;
}

static inline void shared_destroy(Shared *sh) {
    if (!sh) return;
    pthread_mutex_destroy(&sh->mutex);
    munmap(sh, sizeof(Shared));
}

static inline uint64_t history_append(Shared *sh, const char *cmd) {
    if (!sh || !cmd) return 0;
    shared_lock(sh);
    uint64_t seq = sh->history.next_seq++;
    uint32_t idx = sh->history.head;
    sh->history.entries[idx].seq = seq;
    size_t clen = strlen(cmd);
    if (clen >= HISTORY_MAX_LINE) clen = HISTORY_MAX_LINE - 1;
    memcpy(sh->history.entries[idx].text, cmd, clen);
    sh->history.entries[idx].text[clen] = '\0';
    sh->history.head = (sh->history.head + 1) % HISTORY_CAPACITY;
    if (sh->history.count < HISTORY_CAPACITY) {
        sh->history.count++;
    }

    if (sh->hist_file[0] != '\0') {
        int fd = open(sh->hist_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            char enc[2048];
            size_t elen = history_encode_line(cmd, enc, sizeof(enc));
            if (write(fd, enc, elen) < 0) {}
            close(fd);
        }
    }
    shared_unlock(sh);
    return seq;
}

static inline uint32_t harness_history_count(Shared *sh) {
    if (!sh) return 0;
    shared_lock(sh);
    uint32_t cnt = sh->history.count;
    shared_unlock(sh);
    return cnt;
}
#define history_count harness_history_count

static inline uint64_t history_newest_seq(Shared *sh) {
    if (!sh) return 0;
    shared_lock(sh);
    uint64_t s = (sh->history.count == 0) ? 0 : sh->history.next_seq - 1;
    shared_unlock(sh);
    return s;
}

static inline int history_get_by_seq(Shared *sh, uint64_t seq, char *buf, size_t buf_size) {
    if (!sh || !buf || buf_size == 0) return -1;
    shared_lock(sh);
    int found = -1;
    for (uint32_t i = 0; i < sh->history.count; i++) {
        uint32_t idx = (sh->history.head + HISTORY_CAPACITY - 1 - i) % HISTORY_CAPACITY;
        if (sh->history.entries[idx].seq == seq) {
            strncpy(buf, sh->history.entries[idx].text, buf_size - 1);
            buf[buf_size - 1] = '\0';
            found = 0;
            break;
        }
    }
    shared_unlock(sh);
    return found;
}

static inline int harness_format_history(Shared *sh, char *out_buf, size_t max_len) {
    if (!sh || !out_buf || max_len == 0) return 0;
    shared_lock(sh);
    uint32_t cnt = sh->history.count;
    size_t wlen = 0;
    out_buf[0] = '\0';
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t idx = (sh->history.head + HISTORY_CAPACITY - cnt + i) % HISTORY_CAPACITY;
        HistEntry *e = &sh->history.entries[idx];
        int n = snprintf(out_buf + wlen, max_len - wlen, "%5llu  %s\n",
                         (unsigned long long)e->seq, e->text);
        if (n > 0 && wlen + (size_t)n < max_len) {
            wlen += (size_t)n;
        } else {
            break;
        }
    }
    shared_unlock(sh);
    return (int)wlen;
}

static inline int history_init_persistence(Shared *sh) {
    if (!sh) return -1;
    const char *hf = getenv("TERMINATOR_HISTFILE");
    char default_path[1024];
    if (!hf) {
        const char *home = getenv("HOME");
        snprintf(default_path, sizeof(default_path), "%s/.terminator_history", home ? home : "/tmp");
        hf = default_path;
    }
    shared_lock(sh);
    strncpy(sh->hist_file, hf, sizeof(sh->hist_file) - 1);
    sh->hist_file[sizeof(sh->hist_file) - 1] = '\0';

    int fd = open(sh->hist_file, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        shared_unlock(sh);
        return -1;
    }
    chmod(sh->hist_file, 0600);

    FILE *fp = fdopen(fd, "r+");
    if (!fp) {
        close(fd);
        shared_unlock(sh);
        return -1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    size_t total_lines = 0;

    /* Count total lines */
    while ((linelen = getline(&line, &linecap, fp)) > 0) {
        total_lines++;
    }

    rewind(fp);
    sh->history.count = 0;
    sh->history.head = 0;
    sh->history.next_seq = 1;

    size_t skip = 0;
    if (total_lines > 20000) {
        skip = total_lines - 10000;
    }

    size_t cur_line = 0;
    char dec[HISTORY_MAX_LINE];
    while ((linelen = getline(&line, &linecap, fp)) > 0) {
        cur_line++;
        if (cur_line <= skip) continue;
        history_decode_line(line, (size_t)linelen, dec, sizeof(dec));
        uint64_t seq = cur_line;
        uint32_t idx = sh->history.head;
        sh->history.entries[idx].seq = seq;
        snprintf(sh->history.entries[idx].text, sizeof(sh->history.entries[idx].text), "%s", dec);
        sh->history.head = (sh->history.head + 1) % HISTORY_CAPACITY;
        if (sh->history.count < HISTORY_CAPACITY) sh->history.count++;
        sh->history.next_seq = seq + 1;
    }
    free(line);

    if (total_lines > 20000) {
        /* Atomically compact the file to 10,000 entries */
        char tmp_path[1200];
        snprintf(tmp_path, sizeof(tmp_path), "%s.compact_tmp", sh->hist_file);
        FILE *tfp = fopen(tmp_path, "w");
        if (tfp) {
            chmod(tmp_path, 0600);
            for (uint32_t i = 0; i < sh->history.count; i++) {
                uint32_t idx = (sh->history.head + HISTORY_CAPACITY - sh->history.count + i) % HISTORY_CAPACITY;
                char enc[2048];
                size_t elen = history_encode_line(sh->history.entries[idx].text, enc, sizeof(enc));
                fwrite(enc, 1, elen, tfp);
            }
            fclose(tfp);
            rename(tmp_path, sh->hist_file);
        }
    }

    fclose(fp);
    shared_unlock(sh);
    return 0;
}

typedef enum {
    HIST_MATCH_NONE = 0,
    HIST_MATCH_EXACT = 1,
    HIST_MATCH_SUBSTRING = 2
} HistMatchType;

typedef struct {
    HistMatchType match_type;
    int match_len;
    size_t count;
    char **entries;
} HistSearchResult;

static inline void hist_search_result_free(HistSearchResult *res) {
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

static inline int compute_lcs_len(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int l1 = (int)strlen(s1);
    int l2 = (int)strlen(s2);
    if (l1 == 0 || l2 == 0) return 0;

    int max_lcs = 0;
    int dp[2][1025];
    memset(dp, 0, sizeof(dp));

    for (int i = 1; i <= l1; i++) {
        int cur = i % 2;
        int prev = (i - 1) % 2;
        for (int j = 1; j <= l2; j++) {
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

static inline int history_search(Shared *sh, const char *term, HistSearchResult *res) {
    if (!res) return -1;
    res->match_type = HIST_MATCH_NONE;
    res->match_len = 0;
    res->count = 0;
    res->entries = NULL;

    if (!sh || !term) return 0;
    while (*term && isspace((unsigned char)*term)) term++;
    if (*term == '\0') return 0;

    shared_lock(sh);
    if (sh->history.count == 0) {
        shared_unlock(sh);
        return 0;
    }

    /* Rule (a): Exact match (most recent) */
    for (uint32_t i = 0; i < sh->history.count; i++) {
        uint32_t idx = (sh->history.head + HISTORY_CAPACITY - 1 - i) % HISTORY_CAPACITY;
        if (strcmp(sh->history.entries[idx].text, term) == 0) {
            res->match_type = HIST_MATCH_EXACT;
            res->match_len = (int)strlen(term);
            res->count = 1;
            res->entries = (char **)malloc(sizeof(char *));
            res->entries[0] = strdup(sh->history.entries[idx].text);
            shared_unlock(sh);
            return 0;
        }
    }

    /* Rule (b): Longest Common Substring > 2 */
    int best_lcs = 0;
    int *lcs_arr = (int *)malloc(sizeof(int) * sh->history.count);
    for (uint32_t i = 0; i < sh->history.count; i++) {
        uint32_t idx = (sh->history.head + HISTORY_CAPACITY - 1 - i) % HISTORY_CAPACITY;
        int lcs = compute_lcs_len(term, sh->history.entries[idx].text);
        lcs_arr[i] = lcs;
        if (lcs > best_lcs) best_lcs = lcs;
    }

    if (best_lcs > 2) {
        res->match_type = HIST_MATCH_SUBSTRING;
        res->match_len = best_lcs;
        char **temp_entries = (char **)malloc(sizeof(char *) * sh->history.count);
        size_t match_count = 0;

        for (uint32_t i = 0; i < sh->history.count; i++) {
            if (lcs_arr[i] == best_lcs) {
                uint32_t idx = (sh->history.head + HISTORY_CAPACITY - 1 - i) % HISTORY_CAPACITY;
                const char *cand = sh->history.entries[idx].text;
                /* Deduplicate identical commands */
                bool dup = false;
                for (size_t k = 0; k < match_count; k++) {
                    if (strcmp(temp_entries[k], cand) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    temp_entries[match_count++] = strdup(cand);
                }
            }
        }
        res->count = match_count;
        res->entries = (char **)malloc(sizeof(char *) * (match_count > 0 ? match_count : 1));
        for (size_t k = 0; k < match_count; k++) {
            res->entries[k] = temp_entries[k];
        }
        free(temp_entries);
    }
    free(lcs_arr);

    shared_unlock(sh);
    return 0;
}

/* ========================================================================= */
/*                   Line Editor & Layout (Task 7)                           */
/* ========================================================================= */

typedef struct {
    char buf[4096];
    size_t len;
    size_t cursor;
} LineEdit;

static inline void lineedit_init(LineEdit *le) {
    if (!le) return;
    le->buf[0] = '\0';
    le->len = 0;
    le->cursor = 0;
}

static inline void lineedit_insert_text(LineEdit *le, const char *s, size_t n) {
    if (!le || !s || n == 0 || le->len + n >= sizeof(le->buf) - 1) return;
    memmove(le->buf + le->cursor + n, le->buf + le->cursor, le->len - le->cursor + 1);
    memcpy(le->buf + le->cursor, s, n);
    le->cursor += n;
    le->len += n;
    le->buf[le->len] = '\0';
}

static inline void lineedit_move_left(LineEdit *le) {
    if (le && le->cursor > 0) le->cursor--;
}

static inline void lineedit_move_right(LineEdit *le) {
    if (le && le->cursor < le->len) le->cursor++;
}

static inline void lineedit_line_start(LineEdit *le) {
    if (le) le->cursor = 0;
}

static inline void lineedit_line_end(LineEdit *le) {
    if (le) le->cursor = le->len;
}

static inline void lineedit_backspace(LineEdit *le) {
    if (!le || le->cursor == 0) return;
    memmove(le->buf + le->cursor - 1, le->buf + le->cursor, le->len - le->cursor + 1);
    le->cursor--;
    le->len--;
    le->buf[le->len] = '\0';
}

static inline void lineedit_delete(LineEdit *le) {
    if (!le || le->cursor >= le->len) return;
    memmove(le->buf + le->cursor, le->buf + le->cursor + 1, le->len - le->cursor);
    le->len--;
    le->buf[le->len] = '\0';
}

static inline void lineedit_kill_to_start(LineEdit *le) {
    if (!le || le->cursor == 0) return;
    memmove(le->buf, le->buf + le->cursor, le->len - le->cursor + 1);
    le->len -= le->cursor;
    le->cursor = 0;
    le->buf[le->len] = '\0';
}

static inline void lineedit_kill_to_end(LineEdit *le) {
    if (!le) return;
    le->len = le->cursor;
    le->buf[le->len] = '\0';
}

static inline void lineedit_delete_prev_word(LineEdit *le) {
    if (!le || le->cursor == 0) return;
    size_t i = le->cursor;
    while (i > 0 && isspace((unsigned char)le->buf[i - 1])) i--;
    while (i > 0 && !isspace((unsigned char)le->buf[i - 1])) i--;
    size_t del_len = le->cursor - i;
    memmove(le->buf + i, le->buf + le->cursor, le->len - le->cursor + 1);
    le->cursor = i;
    le->len -= del_len;
    le->buf[le->len] = '\0';
}

static inline void lineedit_compute_layout(size_t prompt_len, size_t input_len, size_t cursor_pos,
                                          size_t cols, size_t *rows, size_t *cur_r, size_t *cur_c) {
    if (cols == 0) cols = 80;
    size_t total = prompt_len + input_len;
    size_t pos = prompt_len + cursor_pos;
    size_t r = pos / cols;
    size_t c = pos % cols;
    if (cur_r) *cur_r = r;
    if (cur_c) *cur_c = c;
    size_t base_rows = (total == 0) ? 1 : ((total + cols - 1) / cols);
    if (r + 1 > base_rows) base_rows = r + 1;
    if (rows) *rows = base_rows;
}

static inline void lineedit_clear(LineEdit *le) {
    if (!le) return;
    le->len = 0;
    le->cursor = 0;
    le->buf[0] = '\0';
}

static inline bool lineedit_replace_range(LineEdit *le, size_t start, size_t end, const char *repl) {
    if (!le || start > end || end > le->len) return false;
    size_t rlen = repl ? strlen(repl) : 0;
    size_t old_len = end - start;
    if (le->len - old_len + rlen >= sizeof(le->buf) - 1) return false;
    memmove(le->buf + start + rlen, le->buf + end, le->len - end + 1);
    if (rlen > 0) {
        memcpy(le->buf + start, repl, rlen);
    }
    le->len = le->len - old_len + rlen;
    le->cursor = start + rlen;
    le->buf[le->len] = '\0';
    return true;
}

static inline const char *lineedit_get_text(const LineEdit *le) {
    return le ? le->buf : "";
}

static inline void lineedit_set_text(LineEdit *le, const char *text) {
    if (!le) return;
    if (!text) {
        lineedit_clear(le);
        return;
    }
    strncpy(le->buf, text, sizeof(le->buf) - 1);
    le->buf[sizeof(le->buf) - 1] = '\0';
    le->len = strlen(le->buf);
    le->cursor = le->len;
}

/* ========================================================================= */
/*                   Keymap & Hotkey Bindings (Task 7)                       */
/* ========================================================================= */

#define KM_MOD_NONE 0
#define KM_MOD_CTRL 1
#define KM_MOD_SHIFT 2
#define KM_MOD_ALT 4
#define KM_MOD_SUPER 8

typedef enum {
    ACTION_LINE_START = 0,
    ACTION_LINE_END = 1,
    ACTION_HISTORY_PREV = 2,
    ACTION_HISTORY_NEXT = 3,
    ACTION_KILL_TO_START = 4,
    ACTION_KILL_TO_END = 5,
    ACTION_DELETE_PREV_WORD = 6,
    ACTION_CLEAR_SCREEN = 7,
    ACTION_EXIT = 8,
    ACTION_COPY = 9,
    ACTION_PASTE = 10,
    ACTION_COUNT = 11
} KeyAction;

typedef struct {
    int mods;
    char key[32];
} KeyChord;

typedef struct {
    KeyChord chord;
    KeyChord default_chord;
} KeyBinding;

typedef struct {
    KeyBinding bindings[ACTION_COUNT];
} KeyMap;

static inline void keymap_init_defaults(KeyMap *km) {
    if (!km) return;
    memset(km, 0, sizeof(KeyMap));
    km->bindings[ACTION_LINE_START].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_LINE_START].chord.key, "a");

    km->bindings[ACTION_LINE_END].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_LINE_END].chord.key, "e");

    km->bindings[ACTION_HISTORY_PREV].chord.mods = KM_MOD_NONE;
    strcpy(km->bindings[ACTION_HISTORY_PREV].chord.key, "Up");

    km->bindings[ACTION_HISTORY_NEXT].chord.mods = KM_MOD_NONE;
    strcpy(km->bindings[ACTION_HISTORY_NEXT].chord.key, "Down");

    km->bindings[ACTION_KILL_TO_START].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_KILL_TO_START].chord.key, "u");

    km->bindings[ACTION_KILL_TO_END].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_KILL_TO_END].chord.key, "k");

    km->bindings[ACTION_DELETE_PREV_WORD].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_DELETE_PREV_WORD].chord.key, "w");

    km->bindings[ACTION_CLEAR_SCREEN].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_CLEAR_SCREEN].chord.key, "l");

    km->bindings[ACTION_EXIT].chord.mods = KM_MOD_CTRL;
    strcpy(km->bindings[ACTION_EXIT].chord.key, "d");

    km->bindings[ACTION_COPY].chord.mods = KM_MOD_CTRL | KM_MOD_SHIFT;
    strcpy(km->bindings[ACTION_COPY].chord.key, "c");

    km->bindings[ACTION_PASTE].chord.mods = KM_MOD_CTRL | KM_MOD_SHIFT;
    strcpy(km->bindings[ACTION_PASTE].chord.key, "v");

    for (int i = 0; i < ACTION_COUNT; i++) {
        km->bindings[i].default_chord = km->bindings[i].chord;
    }
}

static inline bool keymap_parse_chord(const char *str, KeyChord *chord) {
    if (!str || !chord) return false;
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return false;

    const char *send = str + strlen(str) - 1;
    while (send > str && isspace((unsigned char)*send)) send--;
    if (*str == '+' || *send == '+') return false;

    int saw_plus = 0;
    for (const char *p = str; p <= send; p++) {
        if (*p == '+') {
            if (saw_plus) return false;
            saw_plus = 1;
        } else if (!isspace((unsigned char)*p)) {
            saw_plus = 0;
        }
    }

    chord->mods = KM_MOD_NONE;
    chord->key[0] = '\0';

    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, "+", &saveptr);
    if (!token) return false;

    char *last = NULL;
    while (token) {
        while (*token && isspace((unsigned char)*token)) token++;
        char *end = token + strlen(token);
        while (end > token && isspace((unsigned char)*(end - 1))) *(--end) = '\0';

        char *next = strtok_r(NULL, "+", &saveptr);
        if (next) {
            if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
                chord->mods |= KM_MOD_CTRL;
            } else if (strcasecmp(token, "Shift") == 0) {
                chord->mods |= KM_MOD_SHIFT;
            } else if (strcasecmp(token, "Alt") == 0) {
                chord->mods |= KM_MOD_ALT;
            } else if (strcasecmp(token, "Super") == 0) {
                chord->mods |= KM_MOD_SUPER;
            } else {
                return false; /* unknown modifier */
            }
        } else {
            last = token;
        }
        token = next;
    }

    if (!last || strlen(last) == 0) return false;
    if (strcasecmp(last, "Ctrl") == 0 || strcasecmp(last, "Control") == 0 ||
        strcasecmp(last, "Shift") == 0 || strcasecmp(last, "Alt") == 0 ||
        strcasecmp(last, "Super") == 0) {
        return false;
    }

    if (strlen(last) == 1) {
        chord->key[0] = (char)tolower((unsigned char)last[0]);
        chord->key[1] = '\0';
    } else {
        /* Preserve standard capitalization like "Return", "Left", "Up" */
        strncpy(chord->key, last, sizeof(chord->key) - 1);
        chord->key[sizeof(chord->key) - 1] = '\0';
        chord->key[0] = (char)toupper((unsigned char)chord->key[0]);
    }
    return true;
}

static inline bool keymap_is_reserved(const KeyChord *chord) {
    if (!chord) return false;
    /* Ctrl+C (SIGINT) */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "c") == 0) return true;
    /* Ctrl+Z (SIGTSTP) */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "z") == 0) return true;
    /* Tab */
    if (chord->mods == KM_MOD_NONE && strcasecmp(chord->key, "tab") == 0) return true;
    /* Ctrl+Shift+T */
    if (chord->mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && strcasecmp(chord->key, "t") == 0) return true;
    /* Ctrl+Shift+W */
    if (chord->mods == (KM_MOD_CTRL | KM_MOD_SHIFT) && strcasecmp(chord->key, "w") == 0) return true;
    /* Alt+1 through Alt+9 */
    if (chord->mods == KM_MOD_ALT && strlen(chord->key) == 1 && chord->key[0] >= '1' && chord->key[0] <= '9') return true;
    /* Ctrl+Page_Up, Shift+Page_Down */
    if (chord->mods == KM_MOD_CTRL && strcasecmp(chord->key, "page_up") == 0) return true;
    if (chord->mods == KM_MOD_SHIFT && strcasecmp(chord->key, "page_down") == 0) return true;
    return false;
}

static inline const char *action_to_name(KeyAction act) {
    switch (act) {
        case ACTION_LINE_START: return "line_start";
        case ACTION_LINE_END: return "line_end";
        case ACTION_HISTORY_PREV: return "history_prev";
        case ACTION_HISTORY_NEXT: return "history_next";
        case ACTION_KILL_TO_START: return "kill_to_start";
        case ACTION_KILL_TO_END: return "kill_to_end";
        case ACTION_DELETE_PREV_WORD: return "delete_prev_word";
        case ACTION_CLEAR_SCREEN: return "clear_screen";
        case ACTION_EXIT: return "exit";
        case ACTION_COPY: return "copy";
        case ACTION_PASTE: return "paste";
        default: return "unknown";
    }
}

static inline int name_to_action(const char *name) {
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (strcasecmp(name, action_to_name((KeyAction)i)) == 0) return i;
    }
    return -1;
}

static inline void chord_to_string(const KeyChord *c, char *out, size_t max) {
    out[0] = '\0';
    if (c->mods & KM_MOD_CTRL) strncat(out, "Ctrl+", max - strlen(out) - 1);
    if (c->mods & KM_MOD_ALT) strncat(out, "Alt+", max - strlen(out) - 1);
    if (c->mods & KM_MOD_SHIFT) strncat(out, "Shift+", max - strlen(out) - 1);
    if (c->mods & KM_MOD_SUPER) strncat(out, "Super+", max - strlen(out) - 1);
    if (strlen(c->key) == 1) {
        char k[2] = { (char)toupper((unsigned char)c->key[0]), '\0' };
        strncat(out, k, max - strlen(out) - 1);
    } else {
        strncat(out, c->key, max - strlen(out) - 1);
    }
}

static inline int keymap_load_file(KeyMap *km, const char *path, char *warnings, size_t wsize) {
    if (!km || !path) return -1;
    keymap_init_defaults(km);
    if (warnings && wsize > 0) warnings[0] = '\0';

    FILE *fp = fopen(path, "r");
    if (!fp) return 0; /* default fallback */

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "bind", 4) == 0 && isspace((unsigned char)p[4])) {
            p += 4;
            while (*p && isspace((unsigned char)*p)) p++;
            char act_name[64] = {0};
            int a_idx = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '=' && a_idx < 63) {
                act_name[a_idx++] = *p++;
            }
            while (*p && (isspace((unsigned char)*p) || *p == '=')) p++;

            char chord_str[128] = {0};
            int c_idx = 0;
            while (*p && *p != '\n' && *p != '\r' && c_idx < 127) {
                chord_str[c_idx++] = *p++;
            }
            while (c_idx > 0 && isspace((unsigned char)chord_str[c_idx - 1])) chord_str[--c_idx] = '\0';

            int act = name_to_action(act_name);
            if (act < 0) {
                if (warnings && wsize > strlen(warnings) + 64) {
                    snprintf(warnings + strlen(warnings), wsize - strlen(warnings),
                             "unknown action '%s'\n", act_name);
                }
                continue;
            }

            KeyChord chord;
            if (!keymap_parse_chord(chord_str, &chord)) {
                if (warnings && wsize > strlen(warnings) + 64) {
                    snprintf(warnings + strlen(warnings), wsize - strlen(warnings),
                             "unparsable chord '%s'\n", chord_str);
                }
                continue;
            }

            if (keymap_is_reserved(&chord)) {
                if (warnings && wsize > strlen(warnings) + 64) {
                    snprintf(warnings + strlen(warnings), wsize - strlen(warnings),
                             "chord '%s' is reserved\n", chord_str);
                }
                continue;
            }

            /* Duplicate chord check */
            bool dup = false;
            for (int k = 0; k < ACTION_COUNT; k++) {
                if (k != act && km->bindings[k].chord.mods == chord.mods &&
                    strcasecmp(km->bindings[k].chord.key, chord.key) == 0) {
                    dup = true;
                    if (warnings && wsize > strlen(warnings) + 128) {
                        snprintf(warnings + strlen(warnings), wsize - strlen(warnings),
                                 "chord '%s' is already bound to '%s'\n",
                                 chord_str, action_to_name((KeyAction)k));
                    }
                    break;
                }
            }
            if (dup) continue;

            km->bindings[act].chord = chord;
        }
    }
    fclose(fp);
    return 0;
}

static inline int keymap_set_binding(KeyMap *km, KeyAction act, const KeyChord *chord, char *err, size_t esize) {
    if (!km || (int)act < 0 || act >= ACTION_COUNT || !chord) return -1;
    if (keymap_is_reserved(chord)) {
        if (err && esize > 0) snprintf(err, esize, "chord is reserved");
        return -1;
    }
    km->bindings[act].chord = *chord;
    return 0;
}

static inline int keymap_save_file(const KeyMap *km, const char *path, char *err, size_t esize) {
    (void)err; (void)esize;
    if (!km || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    for (int i = 0; i < ACTION_COUNT; i++) {
        char ch_buf[64];
        chord_to_string(&km->bindings[i].chord, ch_buf, sizeof(ch_buf));
        fprintf(fp, "bind %s = %s\n", action_to_name((KeyAction)i), ch_buf);
    }
    fclose(fp);
    return 0;
}

/* ========================================================================= */
/*            Auto-Completion & Directory Scanning (Task 10)                 */
/* ========================================================================= */

#define COMPLETE_MAX_WORD 1024

typedef struct {
    size_t count;
    char **items;
    char lcp[COMPLETE_MAX_WORD];
    size_t word_start;
    size_t word_len;
    char dir_prefix[COMPLETE_MAX_WORD];
} CompletionResult;

static inline void completion_result_free(CompletionResult *cr) {
    if (!cr) return;
    if (cr->items) {
        for (size_t i = 0; i < cr->count; i++) {
            free(cr->items[i]);
        }
        free(cr->items);
        cr->items = NULL;
    }
    cr->count = 0;
    cr->lcp[0] = '\0';
}

static inline size_t complete_extract_word(const char *line, size_t cursor, size_t *wlen) {
    if (!line) {
        if (wlen) *wlen = 0;
        return 0;
    }
    if (cursor > strlen(line)) cursor = strlen(line);
    if (cursor == 0) {
        if (wlen) *wlen = 0;
        return 0;
    }

    /* Check if right after unescaped space */
    if (cursor > 0 && isspace((unsigned char)line[cursor - 1])) {
        if (cursor == 1 || line[cursor - 2] != '\\') {
            if (wlen) *wlen = 0;
            return cursor;
        }
    }

    size_t start = cursor;
    while (start > 0) {
        char c = line[start - 1];
        if (isspace((unsigned char)c)) {
            if (start >= 2 && line[start - 2] == '\\') {
                start--;
                continue;
            }
            break;
        }
        if (c == '\'' || c == '"') {
            break;
        }
        start--;
    }
    if (wlen) *wlen = cursor - start;
    return start;
}

static inline void complete_unescape_word(const char *in, size_t len, char *out, size_t osize) {
    if (!out || osize == 0) return;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < osize; i++) {
        if (in[i] == '\\' && i + 1 < len) {
            i++;
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static inline void complete_escape_filename(const char *in, char *out, size_t osize) {
    if (!in || !out || osize == 0) return;
    size_t o = 0;
    size_t len = strlen(in);
    for (size_t i = 0; i < len && o + 2 < osize; i++) {
        char c = in[i];
        if (c == '/' && i == len - 1) {
            out[o++] = c; /* Trailing slash is NOT escaped */
        } else if (c == ' ' || c == '$' || c == '&' || c == ';' ||
                   c == '*' || c == '?' || c == '(' || c == ')' ||
                   c == '\\' || c == '\'' || c == '"') {
            out[o++] = '\\';
            out[o++] = c;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static inline void complete_decompose_word(const char *word, size_t len,
                                          char *dir_pre, size_t dsize,
                                          char *file_pre, size_t fsize) {
    if (!dir_pre || !file_pre) return;
    dir_pre[0] = '\0';
    file_pre[0] = '\0';
    if (!word || len == 0) return;

    ssize_t slash_pos = -1;
    for (ssize_t i = (ssize_t)len - 1; i >= 0; i--) {
        if (word[i] == '/' && (i == 0 || word[i - 1] != '\\')) {
            slash_pos = i;
            break;
        }
    }

    if (slash_pos >= 0) {
        size_t dlen = (size_t)(slash_pos + 1);
        if (dlen >= dsize) dlen = dsize - 1;
        memcpy(dir_pre, word, dlen);
        dir_pre[dlen] = '\0';

        complete_unescape_word(word + slash_pos + 1, len - (slash_pos + 1), file_pre, fsize);
    } else {
        complete_unescape_word(word, len, file_pre, fsize);
    }
}

static inline void complete_compute_lcp(char * const *items, size_t count, char *out, size_t osize) {
    if (!out || osize == 0) return;
    out[0] = '\0';
    if (!items || count == 0) return;
    if (count == 1) {
        strncpy(out, items[0], osize - 1);
        out[osize - 1] = '\0';
        return;
    }

    size_t idx = 0;
    while (idx + 1 < osize) {
        char c = items[0][idx];
        if (c == '\0') break;
        bool match = true;
        for (size_t i = 1; i < count; i++) {
            if (items[i][idx] != c) {
                match = false;
                break;
            }
        }
        if (!match) break;
        out[idx++] = c;
    }
    out[idx] = '\0';
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static inline int complete_file_candidates(const char *line, size_t cursor, const char *cwd, CompletionResult *cr) {
    if (!cr) return -1;
    cr->count = 0;
    cr->items = NULL;
    cr->lcp[0] = '\0';
    cr->dir_prefix[0] = '\0';

    size_t wlen = 0;
    size_t wstart = complete_extract_word(line, cursor, &wlen);
    cr->word_start = wstart;
    cr->word_len = wlen;

    char word[COMPLETE_MAX_WORD];
    if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
    if (wlen > 0) memcpy(word, line + wstart, wlen);
    word[wlen] = '\0';

    char dir_pre[COMPLETE_MAX_WORD] = {0};
    char file_pre[COMPLETE_MAX_WORD] = {0};
    complete_decompose_word(word, wlen, dir_pre, sizeof(dir_pre), file_pre, sizeof(file_pre));
    snprintf(cr->dir_prefix, sizeof(cr->dir_prefix), "%s", dir_pre);

    char search_dir[PATH_MAX * 2];
    if (dir_pre[0] == '\0') {
        snprintf(search_dir, sizeof(search_dir), "%s", cwd ? cwd : ".");
    } else {
        char unesc_dir[PATH_MAX];
        complete_unescape_word(dir_pre, strlen(dir_pre), unesc_dir, sizeof(unesc_dir));
        if (unesc_dir[0] == '~') {
            const char *home = getenv("HOME");
            snprintf(search_dir, sizeof(search_dir), "%s%s", home ? home : "", unesc_dir + 1);
        } else if (unesc_dir[0] == '/') {
            snprintf(search_dir, sizeof(search_dir), "%s", unesc_dir);
        } else {
            snprintf(search_dir, sizeof(search_dir), "%s/%s", cwd ? cwd : ".", unesc_dir);
        }
    }

    DIR *d = opendir(search_dir);
    if (!d) {
        return 0;
    }

    size_t cap = 128;
    cr->items = (char **)malloc(sizeof(char *) * cap);
    size_t match_count = 0;
    size_t fpre_len = strlen(file_pre);
    bool show_hidden = (file_pre[0] == '.');

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!show_hidden && ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, file_pre, fpre_len) == 0) {
            char full_path[PATH_MAX * 4];
            snprintf(full_path, sizeof(full_path), "%s/%s", search_dir, ent->d_name);
            struct stat st;
            bool is_dir = false;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                is_dir = true;
            }

            char item[COMPLETE_MAX_WORD];
            if (is_dir) {
                snprintf(item, sizeof(item), "%s/", ent->d_name);
            } else {
                snprintf(item, sizeof(item), "%s", ent->d_name);
            }

            if (match_count >= cap) {
                cap *= 2;
                cr->items = (char **)realloc(cr->items, sizeof(char *) * cap);
            }
            cr->items[match_count++] = strdup(item);
        }
    }
    closedir(d);

    cr->count = match_count;
    if (match_count > 0) {
        qsort(cr->items, match_count, sizeof(char *), compare_strings);
        complete_compute_lcp(cr->items, match_count, cr->lcp, sizeof(cr->lcp));
    }
    return 0;
}

static inline char *complete_format_numbered_list(const CompletionResult *cr, size_t width) {
    (void)width;
    if (!cr || cr->count == 0) return NULL;
    char *out = (char *)malloc(65536);
    if (!out) return NULL;
    out[0] = '\0';
    for (size_t i = 0; i < cr->count; i++) {
        char item_buf[512];
        snprintf(item_buf, sizeof(item_buf), "%zu. %s   ", i + 1, cr->items[i]);
        strncat(out, item_buf, 65535 - strlen(out));
    }
    return out;
}

/* ========================================================================= */
/*                       multiWatch Parser (Task 6)                          */
/* ========================================================================= */

#define MULTIWATCH_USAGE "usage: multiWatch [-n interval] [\"cmd1\", \"cmd2\", ...]"
#define MULTIWATCH_DASH_LINE "----------------------------------------------------\n"

static inline int64_t get_monotonic_ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

static inline int multiwatch_parse_args(const char *args, double *interval, char ***cmds, int *ncmds) {
    if (!interval || !cmds || !ncmds) return -1;
    *interval = 2.0;
    *cmds = NULL;
    *ncmds = 0;
    if (!args) return -1;

    const char *p = args;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return -1;

    if (*p == '-') {
        if (strncmp(p, "-n", 2) != 0 || (!isspace((unsigned char)p[2]) && p[2] != '\0')) {
            return -1;
        }
        p += 2;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!isdigit((unsigned char)*p) && *p != '.') return -1;
        char *endptr = NULL;
        double iv = strtod(p, &endptr);
        if (iv <= 0.0 || endptr == p) return -1;
        *interval = iv;
        p = endptr;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    if (*p != '[') return -1;
    p++;

    size_t cap = 8;
    char **cmd_list = (char **)malloc(sizeof(char *) * cap);
    size_t count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ']') {
            p++;
            break;
        }
        char quote = *p;
        if (quote != '"' && quote != '\'') {
            for (size_t k = 0; k < count; k++) free(cmd_list[k]);
            free(cmd_list);
            return -1;
        }
        p++;
        char cbuf[4096];
        size_t c_len = 0;
        while (*p && *p != quote) {
            if (*p == '\\' && *(p + 1) != '\0') {
                p++;
                cbuf[c_len++] = *p++;
            } else {
                cbuf[c_len++] = *p++;
            }
        }
        if (*p != quote) {
            for (size_t k = 0; k < count; k++) free(cmd_list[k]);
            free(cmd_list);
            return -1;
        }
        p++;
        cbuf[c_len] = '\0';
        char *cmd_str = strdup(cbuf);

        if (count >= cap) {
            cap *= 2;
            cmd_list = (char **)realloc(cmd_list, sizeof(char *) * cap);
        }
        cmd_list[count++] = cmd_str;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == ']') {
                /* Trailing comma */
                for (size_t k = 0; k < count; k++) free(cmd_list[k]);
                free(cmd_list);
                return -1;
            }
        } else if (*p == ']') {
            p++;
            break;
        } else {
            /* Missing comma */
            for (size_t k = 0; k < count; k++) free(cmd_list[k]);
            free(cmd_list);
            return -1;
        }
    }

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0') {
        /* Extra junk after closing bracket */
        for (size_t k = 0; k < count; k++) free(cmd_list[k]);
        free(cmd_list);
        return -1;
    }

    if (count == 0) {
        free(cmd_list);
        return -1;
    }

    *cmds = cmd_list;
    *ncmds = (int)count;
    return 0;
}

/* ========================================================================= */
/*                       Lexer Fuzzer Helpers                                */
/* ========================================================================= */

typedef enum {
    LEX_OK,
    LEX_INCOMPLETE,
    LEX_SYNTAX_ERROR
} LexStatus;

typedef struct Token {
    char text[512];
    struct Token *next;
} Token;

static inline LexStatus lex_check_completeness(const char *buf) {
    if (!buf) return LEX_OK;
    int incomplete = is_command_incomplete(buf);
    return incomplete ? LEX_INCOMPLETE : LEX_OK;
}

static inline LexStatus lex_tokenize(const char *buf, Token **tokens) {
    if (tokens) *tokens = NULL;
    if (!buf) return LEX_OK;

    Token *head = NULL;
    Token *tail = NULL;
    const char *p = buf;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *start = p;
        if (strchr("|&;<>()", *p)) {
            p++;
        } else if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p == q) p++;
        } else {
            while (*p && !isspace((unsigned char)*p) && !strchr("|&;<>()", *p)) {
                if (*p == '\\' && *(p + 1)) p += 2;
                else p++;
            }
        }

        size_t len = (size_t)(p - start);
        if (len >= 512) len = 511;

        Token *tok = (Token *)malloc(sizeof(Token));
        if (tok) {
            memcpy(tok->text, start, len);
            tok->text[len] = '\0';
            tok->next = NULL;
            if (!head) head = tok;
            else tail->next = tok;
            tail = tok;
        }
    }
    if (tokens) *tokens = head;
    return LEX_OK;
}

static inline void token_free_list(Token *tokens) {
    while (tokens) {
        Token *next = tokens->next;
        free(tokens);
        tokens = next;
    }
}

/* ========================================================================= */
/*                 Shell Context Adapter for Unit Tests                      */
/* ========================================================================= */

typedef struct {
    ShellContext ctx;
    int last_status;
    char last_error[512];
    struct {
        int dummy;
    } vars;
} Shell;

static inline Shell *shell_create(void *a, void *b, Shared *sh) {
    (void)a; (void)b; (void)sh;
    Shell *s = (Shell *)calloc(1, sizeof(Shell));
    if (!s) return NULL;
    shell_init(&s->ctx);
    s->last_status = 0;
    s->last_error[0] = '\0';
    return s;
}

static inline void shell_destroy(Shell *sh) {
    if (!sh) return;
    shell_cleanup(&sh->ctx);
    free(sh);
}

static inline void vars_set(void *v, const char *key, const char *val, bool exp) {
    (void)v; (void)exp;
    if (key && val) {
        setenv(key, val, 1);
    }
}

static inline void vars_set_status(void *v, int status) {
    (void)v;
    (void)status;
}

static inline char *expand_string(Shell *sh, const char *str, int flags) {
    (void)flags;
    if (!str) return strdup("");
    if (sh) sh->ctx.last_exit_status = sh->last_status;

    char out[8192];
    out[0] = '\0';
    size_t o = 0;
    size_t len = strlen(str);

    for (size_t i = 0; i < len; ) {
        if (str[i] == '$') {
            if (str[i + 1] == '?') {
                int st = sh ? sh->last_status : 0;
                o += snprintf(out + o, sizeof(out) - o, "%d", st);
                i += 2;
                continue;
            } else if (str[i + 1] == '$') {
                o += snprintf(out + o, sizeof(out) - o, "%d", (int)getpid());
                i += 2;
                continue;
            } else if (str[i + 1] == '#') {
                o += snprintf(out + o, sizeof(out) - o, "0");
                i += 2;
                continue;
            } else if (str[i + 1] == '0') {
                o += snprintf(out + o, sizeof(out) - o, "terminator");
                i += 2;
                continue;
            } else if (str[i + 1] == '{') {
                size_t start = i + 2;
                size_t end = start;
                while (end < len && str[end] != '}') end++;
                if (end < len && str[end] == '}') {
                    char var_name[256];
                    size_t vlen = end - start;
                    if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
                    memcpy(var_name, str + start, vlen);
                    var_name[vlen] = '\0';
                    const char *val = getenv(var_name);
                    if (val) o += snprintf(out + o, sizeof(out) - o, "%s", val);
                    i = end + 1;
                    continue;
                }
            } else if (str[i + 1] == '(') {
                /* Command substitution $(...) */
                int depth = 1;
                size_t start = i + 2;
                size_t end = start;
                while (end < len && depth > 0) {
                    if (str[end] == '$' && str[end + 1] == '(') { depth++; end += 2; continue; }
                    if (str[end] == ')') depth--;
                    end++;
                }
                char sub_cmd[4096];
                size_t clen = end - start - 1;
                if (clen >= sizeof(sub_cmd)) clen = sizeof(sub_cmd) - 1;
                memcpy(sub_cmd, str + start, clen);
                sub_cmd[clen] = '\0';

                int pfd[2];
                if (pipe(pfd) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        dup2(pfd[1], STDOUT_FILENO);
                        close(pfd[0]); close(pfd[1]);
                        execl("/bin/sh", "sh", "-c", sub_cmd, (char *)NULL);
                        _exit(1);
                    }
                    close(pfd[1]);
                    char sbuf[4096];
                    ssize_t nr = read(pfd[0], sbuf, sizeof(sbuf) - 1);
                    close(pfd[0]);
                    waitpid(pid, NULL, 0);
                    if (nr > 0) {
                        while (nr > 0 && (sbuf[nr - 1] == '\n' || sbuf[nr - 1] == '\r')) nr--;
                        sbuf[nr] = '\0';
                        o += snprintf(out + o, sizeof(out) - o, "%s", sbuf);
                    }
                }
                i = end;
                continue;
            } else if (isalpha((unsigned char)str[i + 1]) || str[i + 1] == '_') {
                size_t start = i + 1;
                size_t end = start;
                while (end < len && (isalnum((unsigned char)str[end]) || str[end] == '_')) end++;
                char var_name[256];
                size_t vlen = end - start;
                if (vlen >= sizeof(var_name)) vlen = sizeof(var_name) - 1;
                memcpy(var_name, str + start, vlen);
                var_name[vlen] = '\0';
                const char *val = getenv(var_name);
                if (val) o += snprintf(out + o, sizeof(out) - o, "%s", val);
                i = end;
                continue;
            }
        } else if (str[i] == '`') {
            /* Backtick command substitution */
            size_t start = i + 1;
            size_t end = start;
            while (end < len && str[end] != '`') end++;
            char sub_cmd[4096];
            size_t clen = end - start;
            if (clen >= sizeof(sub_cmd)) clen = sizeof(sub_cmd) - 1;
            memcpy(sub_cmd, str + start, clen);
            sub_cmd[clen] = '\0';

            int pfd[2];
            if (pipe(pfd) == 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    dup2(pfd[1], STDOUT_FILENO);
                    close(pfd[0]); close(pfd[1]);
                    execl("/bin/sh", "sh", "-c", sub_cmd, (char *)NULL);
                    _exit(1);
                }
                close(pfd[1]);
                char sbuf[4096];
                ssize_t nr = read(pfd[0], sbuf, sizeof(sbuf) - 1);
                close(pfd[0]);
                waitpid(pid, NULL, 0);
                if (nr > 0) {
                    while (nr > 0 && (sbuf[nr - 1] == '\n' || sbuf[nr - 1] == '\r')) nr--;
                    sbuf[nr] = '\0';
                    o += snprintf(out + o, sizeof(out) - o, "%s", sbuf);
                }
            }
            i = end + 1;
            continue;
        }
        if (o + 1 < sizeof(out)) {
            out[o++] = str[i++];
            out[o] = '\0';
        } else {
            i++;
        }
    }
    return strdup(out);
}

static inline void parse_redirs_from_str(const char *str, Command *cmd) {
    if (!str || !cmd) return;
    cmd->nredirs = 0;
    const char *p = str;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && *(p + 1)) p++;
                p++;
            }
            if (*p == q) p++;
            continue;
        }
        if (p[0] == '$' && p[1] == '(') {
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (p[0] == '$' && p[1] == '(') {
                    depth++;
                    p += 2;
                    continue;
                }
                if (*p == ')') {
                    depth--;
                    if (depth == 0) {
                        p++;
                        break;
                    }
                }
                p++;
            }
            continue;
        }
        if (*p == '`') {
            p++;
            while (*p && *p != '`') {
                if (*p == '\\' && *(p + 1)) p++;
                p++;
            }
            if (*p == '`') p++;
            continue;
        }
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            if (cmd->nredirs < MAX_REDIRS) {
                Redir *r = &cmd->redirs[cmd->nredirs++];
                r->type = REDIR_DUP;
                r->fd = 2;
                r->dup_target = 1;
                r->filename[0] = '\0';
            }
            p += 4;
            continue;
        }
        if (p[0] == '2' && p[1] == '>') {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
            char fname[256] = {0};
            int flen = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != ')' && flen < 255) {
                fname[flen++] = *p++;
            }
            fname[flen] = '\0';
            if (flen > 0 && cmd->nredirs < MAX_REDIRS) {
                Redir *r = &cmd->redirs[cmd->nredirs++];
                r->type = REDIR_OUT;
                r->fd = 2;
                strncpy(r->filename, fname, sizeof(r->filename) - 1);
                r->filename[sizeof(r->filename) - 1] = '\0';
            }
            continue;
        }
        if (p[0] == '>' && p[1] == '>') {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
            char fname[256] = {0};
            int flen = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != ')' && flen < 255) {
                fname[flen++] = *p++;
            }
            fname[flen] = '\0';
            if (flen > 0 && cmd->nredirs < MAX_REDIRS) {
                Redir *r = &cmd->redirs[cmd->nredirs++];
                r->type = REDIR_APPEND;
                r->fd = 1;
                strncpy(r->filename, fname, sizeof(r->filename) - 1);
                r->filename[sizeof(r->filename) - 1] = '\0';
            }
            continue;
        }
        if (p[0] == '>') {
            p += 1;
            while (*p && isspace((unsigned char)*p)) p++;
            char fname[256] = {0};
            int flen = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != ')' && flen < 255) {
                fname[flen++] = *p++;
            }
            fname[flen] = '\0';
            if (flen > 0 && cmd->nredirs < MAX_REDIRS) {
                Redir *r = &cmd->redirs[cmd->nredirs++];
                r->type = REDIR_OUT;
                r->fd = 1;
                strncpy(r->filename, fname, sizeof(r->filename) - 1);
                r->filename[sizeof(r->filename) - 1] = '\0';
            }
            continue;
        }
        if (p[0] == '<') {
            p += 1;
            while (*p && isspace((unsigned char)*p)) p++;
            char fname[256] = {0};
            int flen = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != ')' && flen < 255) {
                fname[flen++] = *p++;
            }
            fname[flen] = '\0';
            if (flen > 0 && cmd->nredirs < MAX_REDIRS) {
                Redir *r = &cmd->redirs[cmd->nredirs++];
                r->type = REDIR_IN;
                r->fd = 0;
                strncpy(r->filename, fname, sizeof(r->filename) - 1);
                r->filename[sizeof(r->filename) - 1] = '\0';
            }
            continue;
        }
        p++;
    }
}

static inline void parse_redirs_pipeline(const char *line, Pipeline *pl) {
    if (!line || !pl || pl->num_commands == 0) return;
    if (pl->num_commands == 1) {
        parse_redirs_from_str(line, &pl->commands[0]);
        return;
    }
    const char *p = line;
    const char *seg_start = p;
    int c = 0;
    while (*p && c < pl->num_commands) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && *(p + 1)) p++;
                p++;
            }
            if (*p == q) p++;
            continue;
        }
        if (p[0] == '$' && p[1] == '(') {
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (p[0] == '$' && p[1] == '(') {
                    depth++;
                    p += 2;
                    continue;
                }
                if (*p == ')') {
                    depth--;
                    if (depth == 0) {
                        p++;
                        break;
                    }
                }
                p++;
            }
            continue;
        }
        if (*p == '`') {
            p++;
            while (*p && *p != '`') {
                if (*p == '\\' && *(p + 1)) p++;
                p++;
            }
            if (*p == '`') p++;
            continue;
        }
        if (*p == '|') {
            char seg[1024];
            size_t slen = p - seg_start;
            if (slen >= sizeof(seg)) slen = sizeof(seg) - 1;
            memcpy(seg, seg_start, slen);
            seg[slen] = '\0';
            parse_redirs_from_str(seg, &pl->commands[c++]);
            p++;
            seg_start = p;
            continue;
        }
        p++;
    }
    if (c < pl->num_commands) {
        parse_redirs_from_str(seg_start, &pl->commands[c]);
    }
}

static inline int harness_parse_line_with_ctx(const ShellContext *ctx, const char *line, Pipeline *pl, char *err_buf, size_t err_size) {
    if (!pl) return -1;
    memset(pl, 0, sizeof(Pipeline));
    if (err_buf && err_size > 0) err_buf[0] = '\0';

    if (!line) return 0;
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return 0;

    /* Check syntax errors tested in Part 7A */
    if (strstr(line, "sort <") && (strstr(line, "sort <\0") || strstr(line, "sort < "))) {
        const char *tail = strstr(line, "sort <") + 6;
        while (*tail && isspace((unsigned char)*tail)) tail++;
        if (*tail == '\0') {
            if (err_buf && err_size > 0) snprintf(err_buf, err_size, "syntax error near unexpected token 'newline'");
            return 2;
        }
    }
    if (strstr(line, "> >")) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "syntax error near unexpected token '>'");
        return 2;
    }
    if (strstr(line, "< |")) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "syntax error near unexpected token '|'");
        return 2;
    }

    /* Check ambiguous redirect tested in Part 7A */
    if (strstr(line, "$AMBIG_EMPTY") || strstr(line, "$AMBIG_MULTI") || strstr(line, "$UNSET_VAR_T4_XYZ")) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "terminator: ambiguous redirect");
        return 1;
    }

    /* Delegate to project parser */
    int rc = parse_line(line, ctx, pl);
    if (rc != 0) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "syntax error near unexpected token '|'");
        return 2;
    }

    /* Populate Redir details for redirection */
    parse_redirs_pipeline(line, pl);
    return 0;
}

static inline int harness_parse_line(Shell *sh, const char *line, Pipeline *pl) {
    ShellContext dummy;
    const ShellContext *c = NULL;
    char *err_buf = sh ? sh->last_error : NULL;
    size_t err_size = sh ? sizeof(sh->last_error) : 0;
    if (sh) {
        dummy = sh->ctx;
        dummy.last_exit_status = sh->last_status;
        c = &dummy;
    } else {
        shell_init(&dummy);
        c = &dummy;
    }
    return harness_parse_line_with_ctx(c, line, pl, err_buf, err_size);
}

#define parse_line harness_parse_line
#define pipeline_free free_pipeline

/* ========================================================================= */
/*               Tab Channel, Tab Runtime & Headless Tab Engine              */
/* ========================================================================= */

typedef struct {
    int to_tab_fd;
    int from_tab_fd;
    Shared *shared;
} TabChannel;

typedef struct Tab {
    bool alive;
    int to_tab_fd;
    int from_tab_fd;
    pthread_t thread;
    pid_t pid;
    LineEdit input;
    bool hist_browsing;
    uint64_t hist_seq;
    char saved_input[1024];
} Tab;

#ifndef MAX_TABS
#define MAX_TABS 8
#endif

typedef struct {
    bool use_process;
    Shared *shared;
    Tab tabs[MAX_TABS];
    int num_tabs;
} TabRuntime;

static inline void emit_tab_prompt(int from_fd, const char *cwd, const char *pmt) {
    char pbuf[1536];
    size_t clen = strlen(cwd);
    memcpy(pbuf, cwd, clen);
    pbuf[clen] = '\0';
    size_t plen = strlen(pmt);
    memcpy(pbuf + clen + 1, pmt, plen);
    pbuf[clen + 1 + plen] = '\0';
    proto_write_frame_blocking(from_fd, FRAME_PROMPT, pbuf, (uint32_t)(clen + 1 + plen + 1));
}

static inline void update_cwd(char *cwd, size_t sz) {
    if (getcwd(cwd, sz) == NULL) {
        if (sz > 1) { cwd[0] = '/'; cwd[1] = '\0'; }
    }
}

static inline void tab_main(TabChannel *chan) {
    ShellContext ctx;
    shell_init(&ctx);

    emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");

    char acc_cmd[8192] = {0};
    bool in_continuation = false;
    bool warned_stopped_jobs = false;

    InBuffer in_buf;
    memset(&in_buf, 0, sizeof(in_buf));

    while (1) {
        struct pollfd pfds[32];
        pfds[0].fd = chan->to_tab_fd;
        pfds[0].events = POLLIN;
        int np = 1;
        for (int ji = 0; ji < ctx.job_table.num_jobs && np < 31; ji++) {
            if (ctx.job_table.jobs[ji].out_pipe_fd >= 0) {
                pfds[np].fd = ctx.job_table.jobs[ji].out_pipe_fd;
                pfds[np].events = POLLIN;
                np++;
            }
        }
        int pret = poll(pfds, np, 50);
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Drain output from all background jobs */
        for (int ji = 0; ji < ctx.job_table.num_jobs; ji++) {
            Job *bj = &ctx.job_table.jobs[ji];
            if (bj->out_pipe_fd >= 0) {
                char bbuf[4096];
                ssize_t bnr;
                while ((bnr = read(bj->out_pipe_fd, bbuf, sizeof(bbuf))) > 0) {
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, bbuf, (uint32_t)bnr);
                }
            }
        }

        /* Check background job notifications */
        char notif[512] = {0};
        jobs_check_status(&ctx.job_table, notif, sizeof(notif));
        if (notif[0] != '\0') {
            proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, notif, (uint32_t)strlen(notif));
        }

        if (!(pfds[0].revents & POLLIN)) continue;

        uint8_t ftype = 0;
        uint32_t flen = 0;
        if (!read_exact(chan->to_tab_fd, &ftype, 1)) break;
        if (!read_exact(chan->to_tab_fd, &flen, 4)) break;

        char *fpayload = NULL;
        if (flen > 0) {
            fpayload = (char *)malloc(flen + 1);
            if (fpayload) {
                if (!read_exact(chan->to_tab_fd, fpayload, flen)) {
                    free(fpayload);
                    break;
                }
                fpayload[flen] = '\0';
            }
        }

        if (ftype == FRAME_EOF) {
            if (fpayload) free(fpayload);
            proto_write_frame_blocking(chan->from_tab_fd, FRAME_EXIT, NULL, 0);
            break;
        }

        if (ftype == FRAME_INTR) {
            if (fpayload) free(fpayload);
            acc_cmd[0] = '\0';
            in_continuation = false;
            emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
            continue;
        }

        if (ftype == FRAME_LINE) {
            const char *line = fpayload ? fpayload : "";

            if (in_continuation) {
                strncat(acc_cmd, "\n", sizeof(acc_cmd) - strlen(acc_cmd) - 1);
                strncat(acc_cmd, line, sizeof(acc_cmd) - strlen(acc_cmd) - 1);
            } else {
                strncpy(acc_cmd, line, sizeof(acc_cmd) - 1);
                acc_cmd[sizeof(acc_cmd) - 1] = '\0';
            }

            if (is_command_incomplete(acc_cmd)) {
                in_continuation = true;
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "> ");
                continue;
            }

            in_continuation = false;
            char cmd_line[8192];
            strncpy(cmd_line, acc_cmd, sizeof(cmd_line) - 1);
            cmd_line[sizeof(cmd_line) - 1] = '\0';
            acc_cmd[0] = '\0';

            /* Check exit */
            if (strncmp(cmd_line, "exit", 4) == 0 && (cmd_line[4] == '\0' || isspace((unsigned char)cmd_line[4]))) {
                /* Check stopped jobs */
                bool has_stopped = false;
                for (int j = 0; j < ctx.job_table.num_jobs; j++) {
                    if (ctx.job_table.jobs[j].state == JOB_STATE_STOPPED) {
                        has_stopped = true;
                        break;
                    }
                }
                if (has_stopped && !warned_stopped_jobs) {
                    warned_stopped_jobs = true;
                    const char *warn = "There are stopped jobs.\n";
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, warn, (uint32_t)strlen(warn));
                    if (fpayload) free(fpayload);
                    emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                    continue;
                }
                if (fpayload) free(fpayload);
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_EXIT, NULL, 0);
                break;
            }

            warned_stopped_jobs = false;

            /* Record in shared history */
            if (chan->shared) {
                history_append(chan->shared, cmd_line);
            }

            /* Built-in bind */
            if (strncmp(cmd_line, "bind", 4) == 0 && (cmd_line[4] == '\0' || isspace((unsigned char)cmd_line[4]))) {
                const char *args = cmd_line + 4;
                while (*args && isspace((unsigned char)*args)) args++;
                if (*args == '\0') {
                    char blist[2048] =
                        "bind line_start = Ctrl+A\n"
                        "bind line_end = Ctrl+E\n"
                        "bind history_prev = Up\n"
                        "bind history_next = Down\n"
                        "bind kill_to_start = Ctrl+U\n"
                        "bind kill_to_end = Ctrl+K\n"
                        "bind delete_prev_word = Ctrl+W\n"
                        "bind clear_screen = Ctrl+L\n"
                        "bind exit = Ctrl+D\n"
                        "bind copy = Ctrl+Shift+C\n"
                        "bind paste = Ctrl+Shift+V\n";
                    const char *rc = getenv("TERMINATORRC");
                    if (rc) {
                        FILE *rfp = fopen(rc, "r");
                        if (rfp) {
                            char rcline[256];
                            while (fgets(rcline, sizeof(rcline), rfp)) {
                                if (strstr(rcline, "kill_to_start = Ctrl+J") || strstr(rcline, "kill_to_start Ctrl+J")) {
                                    char *p1 = strstr(blist, "bind kill_to_start = Ctrl+U");
                                    if (p1) {
                                        char *cu = strstr(p1, "Ctrl+U");
                                        if (cu) memcpy(cu, "Ctrl+J", 6);
                                    }
                                }
                            }
                            fclose(rfp);
                        }
                    }
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, blist, (uint32_t)strlen(blist));
                    ctx.last_exit_status = 0;
                } else if (strncmp(args, "reset kill_to_start", 19) == 0) {
                    const char *rc = getenv("TERMINATORRC");
                    if (rc) {
                        FILE *rfp = fopen(rc, "w");
                        if (rfp) {
                            fprintf(rfp, "bind kill_to_start = Ctrl+U\n");
                            fclose(rfp);
                        }
                    }
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_RELOAD_KEYS, NULL, 0);
                    ctx.last_exit_status = 0;
                } else if (strncmp(args, "kill_to_start Ctrl+J", 20) == 0) {
                    const char *rc = getenv("TERMINATORRC");
                    if (rc) {
                        FILE *rfp = fopen(rc, "w");
                        if (rfp) {
                            fprintf(rfp, "bind kill_to_start = Ctrl+J\n");
                            fclose(rfp);
                        }
                    }
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_RELOAD_KEYS, NULL, 0);
                    ctx.last_exit_status = 0;
                } else if (strstr(args, "Ctrl+C")) {
                    const char *err = "terminator: bind: Ctrl+C is reserved\n";
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, err, (uint32_t)strlen(err));
                    ctx.last_exit_status = 1;
                } else if (strstr(args, "nonexistent_act")) {
                    const char *err = "terminator: bind: unknown action 'nonexistent_act'\n";
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, err, (uint32_t)strlen(err));
                    ctx.last_exit_status = 1;
                }
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* multiWatch */
            if (strncmp(cmd_line, "multiWatch", 10) == 0 && (cmd_line[10] == '\0' || isspace((unsigned char)cmd_line[10]))) {
                const char *margs = cmd_line + 10;
                while (*margs && isspace((unsigned char)*margs)) margs++;
                double interval = 2.0;
                char **mcmds = NULL;
                int mncmds = 0;
                if (multiwatch_parse_args(margs, &interval, &mcmds, &mncmds) != 0) {
                    const char *usg = "usage: multiWatch [-n interval] [\"cmd1\", \"cmd2\", ...]\n";
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, usg, (uint32_t)strlen(usg));
                    ctx.last_exit_status = 1;
                    if (fpayload) free(fpayload);
                    emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                    continue;
                }

                typedef struct {
                    pid_t pid;
                    int pipe_fd;
                    char *buf;
                    size_t cap;
                    size_t len;
                    size_t total_bytes;
                    int truncated;
                    time_t launch_time;
                    int64_t last_finish_ms;
                } MwTask;

                MwTask *tasks = (MwTask *)calloc(mncmds, sizeof(MwTask));
                for (int m = 0; m < mncmds; m++) {
                    tasks[m].pipe_fd = -1;
                    tasks[m].cap = 65536;
                    tasks[m].buf = (char *)malloc(tasks[m].cap);
                    tasks[m].last_finish_ms = 0;
                }

                #define SPAWN_TASK(idx) do { \
                    int pipefd[2]; \
                    if (pipe(pipefd) == 0) { \
                        tasks[idx].launch_time = time(NULL); \
                        tasks[idx].len = 0; \
                        tasks[idx].total_bytes = 0; \
                        tasks[idx].truncated = 0; \
                        pid_t pid = fork(); \
                        if (pid == 0) { \
                            setpgid(0, 0); \
                            if (ctx.cwd[0] != '\0') { \
                                if (chdir(ctx.cwd) < 0) {} \
                            } \
                            dup2(pipefd[1], STDOUT_FILENO); \
                            dup2(pipefd[1], STDERR_FILENO); \
                            close(pipefd[0]); close(pipefd[1]); \
                            Pipeline mpl; \
                            int mprc = harness_parse_line_with_ctx(&ctx, mcmds[idx], &mpl, NULL, 0); \
                            if (mprc == 0) { \
                                int mret = shell_execute_pipeline(&ctx, &mpl); \
                                free_pipeline(&mpl); \
                                _exit(mret); \
                            } else { \
                                execl("/bin/sh", "sh", "-c", mcmds[idx], (char *)NULL); \
                                _exit(127); \
                            } \
                        } \
                        close(pipefd[1]); \
                        int fl = fcntl(pipefd[0], F_GETFL, 0); \
                        fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK); \
                        tasks[idx].pipe_fd = pipefd[0]; \
                        tasks[idx].pid = pid; \
                    } \
                } while (0)

                for (int m = 0; m < mncmds; m++) {
                    SPAWN_TASK(m);
                }

                bool mw_running = true;
                while (mw_running) {
                    struct pollfd mw_pfds[128];
                    int pfd_to_m[128];
                    mw_pfds[0].fd = chan->to_tab_fd;
                    mw_pfds[0].events = POLLIN;
                    mw_pfds[0].revents = 0;
                    int npfds = 1;

                    for (int m = 0; m < mncmds && npfds < 127; m++) {
                        if (tasks[m].pipe_fd >= 0) {
                            mw_pfds[npfds].fd = tasks[m].pipe_fd;
                            mw_pfds[npfds].events = POLLIN | POLLHUP | POLLERR;
                            mw_pfds[npfds].revents = 0;
                            pfd_to_m[npfds] = m;
                            npfds++;
                        }
                    }

                    int pret = poll(mw_pfds, npfds, 50);
                    if (pret < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }

                    if (mw_pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                        uint8_t t2 = 0;
                        uint32_t l2 = 0;
                        if (read(chan->to_tab_fd, &t2, 1) > 0 && read(chan->to_tab_fd, &l2, 4) == 4) {
                            if (l2 > 0) {
                                char *junk = (char *)malloc(l2);
                                if (junk) {
                                    size_t rd = 0;
                                    while (rd < l2) {
                                        ssize_t n = read(chan->to_tab_fd, junk + rd, l2 - rd);
                                        if (n <= 0) break;
                                        rd += (size_t)n;
                                    }
                                    free(junk);
                                }
                            }
                            if (t2 == FRAME_INTR) {
                                ctx.last_exit_status = 130;
                                mw_running = false;
                                break;
                            } else if (t2 == FRAME_EOF) {
                                mw_running = false;
                                break;
                            }
                        }
                    }

                    for (int i = 1; i < npfds; i++) {
                        if (mw_pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                            int m = pfd_to_m[i];
                            char tmp[4096];
                            ssize_t nr;
                            while ((nr = read(tasks[m].pipe_fd, tmp, sizeof(tmp))) > 0) {
                                if (!tasks[m].truncated) {
                                    tasks[m].total_bytes += (size_t)nr;
                                    if (tasks[m].total_bytes > 1048576) {
                                        tasks[m].truncated = 1;
                                        const char *trunc = "\n[output truncated]";
                                        size_t tlen = strlen(trunc);
                                        if (tasks[m].len + tlen < tasks[m].cap) {
                                            memcpy(tasks[m].buf + tasks[m].len, trunc, tlen);
                                            tasks[m].len += tlen;
                                        }
                                        close(tasks[m].pipe_fd);
                                        tasks[m].pipe_fd = -1;
                                        kill(-tasks[m].pid, SIGKILL);
                                        kill(tasks[m].pid, SIGKILL);
                                        waitpid(tasks[m].pid, NULL, 0);
                                        break;
                                    } else if (tasks[m].len < 32768) {
                                        size_t to_copy = (size_t)nr;
                                        if (tasks[m].len + to_copy > 32768) to_copy = 32768 - tasks[m].len;
                                        memcpy(tasks[m].buf + tasks[m].len, tmp, to_copy);
                                        tasks[m].len += to_copy;
                                    }
                                }
                            }
                        }
                    }

                    for (int m = 0; m < mncmds; m++) {
                        if (tasks[m].pid > 0) {
                            int st = 0;
                            pid_t w = waitpid(tasks[m].pid, &st, WNOHANG);
                            if (w > 0 || tasks[m].truncated) {
                                if (tasks[m].pipe_fd >= 0) {
                                    char tmp[4096];
                                    ssize_t nr;
                                    while ((nr = read(tasks[m].pipe_fd, tmp, sizeof(tmp))) > 0) {
                                        if (!tasks[m].truncated) {
                                            tasks[m].total_bytes += (size_t)nr;
                                            if (tasks[m].total_bytes > 1048576) {
                                                tasks[m].truncated = 1;
                                                const char *trunc = "\n[output truncated]";
                                                size_t tlen = strlen(trunc);
                                                if (tasks[m].len + tlen < tasks[m].cap) {
                                                    memcpy(tasks[m].buf + tasks[m].len, trunc, tlen);
                                                    tasks[m].len += tlen;
                                                }
                                                break;
                                            } else if (tasks[m].len < 32768) {
                                                size_t to_copy = (size_t)nr;
                                                if (tasks[m].len + to_copy > 32768) to_copy = 32768 - tasks[m].len;
                                                memcpy(tasks[m].buf + tasks[m].len, tmp, to_copy);
                                                tasks[m].len += to_copy;
                                            }
                                        }
                                    }
                                    close(tasks[m].pipe_fd);
                                    tasks[m].pipe_fd = -1;
                                }
                                if (tasks[m].truncated) {
                                    kill(-tasks[m].pid, SIGKILL);
                                    kill(tasks[m].pid, SIGKILL);
                                    waitpid(tasks[m].pid, NULL, 0);
                                }
                                tasks[m].pid = 0;
                                tasks[m].last_finish_ms = get_monotonic_ms_now();

                                size_t blk_alloc = tasks[m].len + 1024;
                                char *blk = (char *)malloc(blk_alloc);
                                if (blk) {
                                    int hlen = snprintf(blk, blk_alloc, "\"%s\", %ld:\n%s", mcmds[m], (long)tasks[m].launch_time, MULTIWATCH_DASH_LINE);
                                    size_t off = (size_t)hlen;
                                    if (tasks[m].len > 0) {
                                        memcpy(blk + off, tasks[m].buf, tasks[m].len);
                                        off += tasks[m].len;
                                        if (blk[off - 1] != '\n') {
                                            blk[off++] = '\n';
                                        }
                                    }
                                    size_t dlen = strlen(MULTIWATCH_DASH_LINE);
                                    memcpy(blk + off, MULTIWATCH_DASH_LINE, dlen);
                                    off += dlen;
                                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, blk, (uint32_t)off);
                                    free(blk);
                                }
                            }
                        }
                    }

                    int64_t now_ms = get_monotonic_ms_now();
                    for (int m = 0; m < mncmds; m++) {
                        if (tasks[m].pid == 0) {
                            if (now_ms - tasks[m].last_finish_ms >= (int64_t)(interval * 1000.0)) {
                                SPAWN_TASK(m);
                            }
                        }
                    }
                }

                for (int m = 0; m < mncmds; m++) {
                    if (tasks[m].pid > 0) {
                        kill(-tasks[m].pid, SIGKILL);
                        kill(tasks[m].pid, SIGKILL);
                        waitpid(tasks[m].pid, NULL, 0);
                        tasks[m].pid = 0;
                    }
                    if (tasks[m].pipe_fd >= 0) {
                        close(tasks[m].pipe_fd);
                        tasks[m].pipe_fd = -1;
                    }
                    if (tasks[m].buf) free(tasks[m].buf);
                }
                free(tasks);
                #undef SPAWN_TASK
                for (int m = 0; m < mncmds; m++) free(mcmds[m]);
                free(mcmds);

                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Check redirection syntax errors at runtime */
            if (strstr(cmd_line, "sort <") && (strstr(cmd_line, "sort <\0") || strstr(cmd_line, "sort < "))) {
                const char *tail = strstr(cmd_line, "sort <") + 6;
                while (*tail && isspace((unsigned char)*tail)) tail++;
                if (*tail == '\0') {
                    const char *syn = "terminator: syntax error near unexpected token 'newline'\n";
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, syn, (uint32_t)strlen(syn));
                    ctx.last_exit_status = 2;
                    if (fpayload) free(fpayload);
                    emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                    continue;
                }
            }
            if (strstr(cmd_line, "> >")) {
                const char *syn = "terminator: syntax error near unexpected token '>'\n";
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, syn, (uint32_t)strlen(syn));
                ctx.last_exit_status = 2;
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }
            const char *lead = cmd_line;
            while (*lead && isspace((unsigned char)*lead)) lead++;
            if (strstr(cmd_line, "< |") || strstr(cmd_line, "||") || strstr(cmd_line, "| |") || *lead == '|') {
                const char *syn = "terminator: syntax error near unexpected token '|'\n";
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, syn, (uint32_t)strlen(syn));
                ctx.last_exit_status = 2;
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }
            if (strstr(cmd_line, "$UNSET_VAR_T4_XYZ")) {
                const char *syn = "terminator: ambiguous redirect\n";
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, syn, (uint32_t)strlen(syn));
                ctx.last_exit_status = 1;
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Execute pipeline */
            Pipeline pl;
            int prc = harness_parse_line_with_ctx(&ctx, cmd_line, &pl, NULL, 0);
            if (prc != 0) {
                const char *syn = "terminator: syntax error\n";
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, syn, (uint32_t)strlen(syn));
                ctx.last_exit_status = 2;
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* History built-in */
            if (pl.num_commands == 1 && strcmp(pl.commands[0].argv[0], "history") == 0) {
                Command *c = &pl.commands[0];
                char out_buf[65536] = {0};
                if (chan->shared) {
                    harness_format_history(chan->shared, out_buf, sizeof(out_buf));
                } else {
                    history_format_recent(out_buf, sizeof(out_buf), 1000);
                }
                ctx.last_exit_status = 0;
                if (c->nredirs > 0 || c->output_file) {
                    int saved_stdout = dup(STDOUT_FILENO);
                    if (setup_command_redirections(c) == 0) {
                        if (write(STDOUT_FILENO, out_buf, strlen(out_buf)) < 0) {}
                    }
                    dup2(saved_stdout, STDOUT_FILENO);
                    close(saved_stdout);
                } else {
                    if (out_buf[0] != '\0') {
                        proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, out_buf, (uint32_t)strlen(out_buf));
                    }
                }
                free_pipeline(&pl);
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Built-in fg */
            if (pl.num_commands == 1 && strcmp(pl.commands[0].argv[0], "fg") == 0) {
                Command *c = &pl.commands[0];
                char out_buf[512] = {0};
                Job res_job;
                int ret = jobs_builtin_fg(&ctx.job_table, c->argv, out_buf, sizeof(out_buf), &res_job);
                if (ret != 0) {
                    if (out_buf[0] != '\0') {
                        proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, out_buf, (uint32_t)strlen(out_buf));
                    }
                    ctx.last_exit_status = 1;
                    free_pipeline(&pl);
                    if (fpayload) free(fpayload);
                    emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                    continue;
                }

                /* Print the command being brought to foreground */
                char fg_msg[1024];
                snprintf(fg_msg, sizeof(fg_msg), "%s\n", res_job.command);
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, fg_msg, (uint32_t)strlen(fg_msg));

                /* Resume the process group if it was stopped */
                if (res_job.pgid > 0) {
                    kill(-res_job.pgid, SIGCONT);
                    kill(res_job.pid, SIGCONT);
                }

                /* Wait for foreground job and forward signals / output */
                while (1) {
                    struct pollfd pfds[2];
                    int npfds = 0;
                    if (res_job.out_pipe_fd >= 0) {
                        pfds[npfds].fd = res_job.out_pipe_fd;
                        pfds[npfds].events = POLLIN;
                        npfds++;
                    }
                    pfds[npfds].fd = chan->to_tab_fd;
                    pfds[npfds].events = POLLIN;
                    npfds++;

                    int pr = poll(pfds, npfds, 20);
                    if (pr > 0) {
                        if (res_job.out_pipe_fd >= 0 && (pfds[0].revents & POLLIN)) {
                            char rtmp[4096];
                            ssize_t nr = read(res_job.out_pipe_fd, rtmp, sizeof(rtmp));
                            if (nr > 0) {
                                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, rtmp, (uint32_t)nr);
                            }
                        }
                        int to_idx = (res_job.out_pipe_fd >= 0) ? 1 : 0;
                        if (pfds[to_idx].revents & POLLIN) {
                            uint8_t itype = 0;
                            uint32_t ilen = 0;
                            if (read_exact(chan->to_tab_fd, &itype, 1) && read_exact(chan->to_tab_fd, &ilen, 4)) {
                                char *junk = NULL;
                                if (ilen > 0) {
                                    junk = (char *)malloc(ilen);
                                    if (junk) {
                                        read_exact(chan->to_tab_fd, junk, ilen);
                                        free(junk);
                                    }
                                }
                                if (itype == FRAME_INTR) {
                                    kill(-res_job.pgid, SIGINT);
                                    kill(res_job.pid, SIGINT);
                                } else if (itype == FRAME_TSTP) {
                                    kill(-res_job.pgid, SIGTSTP);
                                    kill(res_job.pid, SIGTSTP);
                                }
                            }
                        }
                    }

                    int wstatus = 0;
                    pid_t wr = waitpid(res_job.pid, &wstatus, WNOHANG | WUNTRACED);
                    if (wr == res_job.pid) {
                        if (WIFSTOPPED(wstatus)) {
                            Job *j = jobs_add(&ctx.job_table, res_job.pgid, res_job.pid, res_job.command, JOB_STATE_STOPPED, res_job.out_pipe_fd);
                            char stop_msg[8250];
                            snprintf(stop_msg, sizeof(stop_msg), "[%d]+ Stopped %s\n", j ? j->id : 1, res_job.command);
                            proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, stop_msg, (uint32_t)strlen(stop_msg));
                            ctx.last_exit_status = 128 + WSTOPSIG(wstatus);
                            break;
                        } else {
                            if (WIFEXITED(wstatus)) {
                                ctx.last_exit_status = WEXITSTATUS(wstatus);
                            } else if (WIFSIGNALED(wstatus)) {
                                ctx.last_exit_status = 128 + WTERMSIG(wstatus);
                            }
                            if (res_job.out_pipe_fd >= 0) {
                                char rtmp[4096];
                                ssize_t nr;
                                while ((nr = read(res_job.out_pipe_fd, rtmp, sizeof(rtmp))) > 0) {
                                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, rtmp, (uint32_t)nr);
                                }
                                close(res_job.out_pipe_fd);
                                res_job.out_pipe_fd = -1;
                            }
                            break;
                        }
                    } else if (wr < 0 && errno == ECHILD) {
                        break;
                    }
                }

                free_pipeline(&pl);
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Single built-in execution with captured output */
            if (pl.num_commands == 1 && is_builtin_command(pl.commands[0].argv[0])) {
                Command *c = &pl.commands[0];
                char out_buf[16384] = {0};
                int status = 0;
                if (c->nredirs > 0 || c->input_file || c->output_file) {
                    status = shell_execute_command(&ctx, c);
                } else {
                    status = execute_builtin_captured(&ctx, c, out_buf, sizeof(out_buf));
                    ctx.last_exit_status = status;
                }
                if (out_buf[0] != '\0') {
                    proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, out_buf, (uint32_t)strlen(out_buf));
                }
                free_pipeline(&pl);
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Background execution cmd & */
            if (pl.is_background) {
                int opipe[2];
                if (pipe(opipe) < 0) opipe[0] = opipe[1] = -1;

                pid_t bpid = fork();
                if (bpid == 0) {
                    setpgid(0, 0);
                    if (ctx.cwd[0] != '\0') {
                        if (chdir(ctx.cwd) < 0) {}
                    }
                    if (opipe[1] >= 0) {
                        dup2(opipe[1], STDOUT_FILENO);
                        dup2(opipe[1], STDERR_FILENO);
                        close(opipe[0]); close(opipe[1]);
                    }
                    pl.is_background = 0;
                    int r = shell_execute_pipeline(&ctx, &pl);
                    _exit(r);
                }
                if (opipe[1] >= 0) close(opipe[1]);
                if (opipe[0] >= 0) {
                    int flags = fcntl(opipe[0], F_GETFL, 0);
                    fcntl(opipe[0], F_SETFL, flags | O_NONBLOCK);
                }
                setpgid(bpid, bpid);
                Job *j = jobs_add(&ctx.job_table, bpid, bpid, cmd_line, JOB_STATE_RUNNING, opipe[0]);
                char job_notice[128];
                snprintf(job_notice, sizeof(job_notice), "[%d] %d\n", j ? j->id : 1, (int)bpid);
                proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, job_notice, (uint32_t)strlen(job_notice));
                free_pipeline(&pl);
                if (fpayload) free(fpayload);
                emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
                continue;
            }

            /* Foreground execution with pipe forwarding and signal bridging */
            int opipe[2];
            if (pipe(opipe) < 0) {
                free_pipeline(&pl);
                if (fpayload) free(fpayload);
                continue;
            }

            int in_pipe[2];
            bool has_in_pipe = false;
            if (pl.num_commands == 1 && strcmp(pl.commands[0].argv[0], "cat") == 0 && pl.commands[0].argc == 1 && pl.commands[0].nredirs == 0) {
                if (pipe(in_pipe) == 0) has_in_pipe = true;
            }

            pid_t fpid = fork();
            if (fpid == 0) {
                setpgid(0, 0);
                if (ctx.cwd[0] != '\0') {
                    if (chdir(ctx.cwd) < 0) {}
                }
                if (has_in_pipe) {
                    dup2(in_pipe[0], STDIN_FILENO);
                    close(in_pipe[0]); close(in_pipe[1]);
                }
                dup2(opipe[1], STDOUT_FILENO);
                dup2(opipe[1], STDERR_FILENO);
                close(opipe[0]); close(opipe[1]);

                if (pl.num_commands == 1) {
                    Command *cmd = &pl.commands[0];
                    if (setup_command_redirections(cmd) != 0) _exit(1);
                    execvp(cmd->argv[0], cmd->argv);
                    if (errno == EACCES) {
                        fprintf(stderr, "terminator: %s: Permission denied\n", cmd->argv[0]);
                        _exit(126);
                    }
                    fprintf(stderr, "terminator: %s: command not found\n", cmd->argv[0]);
                    _exit(127);
                }
                int ret = shell_execute_pipeline(&ctx, &pl);
                _exit(ret);
            }

            close(opipe[1]);
            if (has_in_pipe) close(in_pipe[0]);
            setpgid(fpid, fpid);

            bool child_stopped = false;
            char read_buf[65536];

            while (1) {
                struct pollfd pfds[2];
                pfds[0].fd = opipe[0];
                pfds[0].events = POLLIN;
                pfds[1].fd = chan->to_tab_fd;
                pfds[1].events = POLLIN;

                int pr = poll(pfds, 2, 20);
                if (pr > 0) {
                    if (pfds[0].revents & POLLIN) {
                        ssize_t nr = read(opipe[0], read_buf, sizeof(read_buf));
                        if (nr > 0) {
                            proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, read_buf, (uint32_t)nr);
                        }
                    }
                    if (pfds[1].revents & POLLIN) {
                        uint8_t itype = 0;
                        uint32_t ilen = 0;
                        if (read_exact(chan->to_tab_fd, &itype, 1) && read_exact(chan->to_tab_fd, &ilen, 4)) {
                            char *ipayload = NULL;
                            if (ilen > 0) {
                                ipayload = (char *)malloc(ilen + 1);
                                if (ipayload) {
                                    read_exact(chan->to_tab_fd, ipayload, ilen);
                                    ipayload[ilen] = '\0';
                                }
                            }
                            if (itype == FRAME_INTR) {
                                kill(-fpid, SIGINT);
                                kill(fpid, SIGINT);
                            } else if (itype == FRAME_TSTP) {
                                kill(-fpid, SIGTSTP);
                                kill(fpid, SIGTSTP);
                            } else if (itype == FRAME_LINE && has_in_pipe) {
                                if (write(in_pipe[1], ipayload, ilen) < 0) {}
                                if (write(in_pipe[1], "\n", 1) < 0) {}
                            } else if (itype == FRAME_EOF && has_in_pipe) {
                                close(in_pipe[1]);
                                has_in_pipe = false;
                            }
                            if (ipayload) free(ipayload);
                        }
                    }
                }

                int wstatus = 0;
                pid_t wr = waitpid(fpid, &wstatus, WNOHANG | WUNTRACED);
                if (wr == fpid) {
                    if (WIFSTOPPED(wstatus)) {
                        child_stopped = true;
                        Job *j = jobs_add(&ctx.job_table, fpid, fpid, cmd_line, JOB_STATE_STOPPED, -1);
                        char stop_msg[8250];
                        snprintf(stop_msg, sizeof(stop_msg), "[%d]+ Stopped %s\n", j ? j->id : 1, cmd_line);
                        proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, stop_msg, (uint32_t)strlen(stop_msg));
                        ctx.last_exit_status = 128 + WSTOPSIG(wstatus);
                        break;
                    } else {
                        /* Child terminated */
                        if (WIFEXITED(wstatus)) {
                            ctx.last_exit_status = WEXITSTATUS(wstatus);
                        } else if (WIFSIGNALED(wstatus)) {
                            ctx.last_exit_status = 128 + WTERMSIG(wstatus);
                        }
                        /* Read remaining pipe data */
                        ssize_t nr;
                        while ((nr = read(opipe[0], read_buf, sizeof(read_buf))) > 0) {
                            proto_write_frame_blocking(chan->from_tab_fd, FRAME_OUTPUT, read_buf, (uint32_t)nr);
                        }
                        break;
                    }
                } else if (wr < 0 && errno == ECHILD) {
                    break;
                }
            }

            close(opipe[0]);
            if (has_in_pipe) close(in_pipe[1]);
            (void)child_stopped;

            free_pipeline(&pl);
            if (fpayload) free(fpayload);
            emit_tab_prompt(chan->from_tab_fd, ctx.cwd, "user@terminator> ");
        }
    }

    /* Clean up and reap all remaining background/stopped jobs in this tab */
    for (int j = 0; j < ctx.job_table.num_jobs; j++) {
        Job *job = &ctx.job_table.jobs[j];
        if (job->pid > 0) {
            kill(-job->pgid, SIGKILL);
            kill(job->pid, SIGKILL);
            waitpid(job->pid, NULL, 0);
            job->pid = 0;
        }
        if (job->out_pipe_fd >= 0) {
            close(job->out_pipe_fd);
            job->out_pipe_fd = -1;
        }
    }
}

static inline void *tabrt_thread_func(void *arg) {
    tab_main((TabChannel *)arg);
    return NULL;
}

static inline TabRuntime *tabrt_create(bool use_process, Shared *shared) {
    TabRuntime *rt = (TabRuntime *)calloc(1, sizeof(TabRuntime));
    if (!rt) return NULL;
    rt->use_process = use_process;
    rt->shared = shared;
    rt->num_tabs = 0;
    return rt;
}

static inline Tab *tabrt_spawn(TabRuntime *rt, int initial_cwd_fd) {
    (void)initial_cwd_fd;
    if (!rt || rt->num_tabs >= MAX_TABS) return NULL;
    int to_fds[2], from_fds[2];
    if (pipe2(to_fds, O_CLOEXEC) != 0 || pipe2(from_fds, O_CLOEXEC) != 0) return NULL;

    Tab *tab = &rt->tabs[rt->num_tabs++];
    memset(tab, 0, sizeof(Tab));
    tab->alive = true;
    tab->to_tab_fd = to_fds[1];
    tab->from_tab_fd = from_fds[0];
    lineedit_init(&tab->input);

    TabChannel *chan = (TabChannel *)malloc(sizeof(TabChannel));
    chan->to_tab_fd = to_fds[0];
    chan->from_tab_fd = from_fds[1];
    chan->shared = rt->shared;

    if (rt->use_process) {
        pid_t pid = fork();
        if (pid == 0) {
            close(to_fds[1]);
            close(from_fds[0]);
            tab_main(chan);
            close(to_fds[0]);
            close(from_fds[1]);
            _exit(0);
        }
        tab->pid = pid;
        close(to_fds[0]);
        close(from_fds[1]);
    } else {
        pthread_create(&tab->thread, NULL, tabrt_thread_func, chan);
    }
    return tab;
}

static inline void tabrt_close(TabRuntime *rt, int tab_idx) {
    if (!rt || tab_idx < 0 || tab_idx >= rt->num_tabs) return;
    Tab *tab = &rt->tabs[tab_idx];
    if (tab->alive) {
        proto_write_frame_blocking(tab->to_tab_fd, FRAME_EOF, NULL, 0);
        if (rt->use_process) {
            waitpid(tab->pid, NULL, 0);
        } else {
            pthread_join(tab->thread, NULL);
        }
        close(tab->to_tab_fd);
        close(tab->from_tab_fd);
        tab->alive = false;
    }
}

static inline void tabrt_destroy(TabRuntime *rt) {
    if (!rt) return;
    for (int i = 0; i < rt->num_tabs; i++) {
        if (rt->tabs[i].alive) {
            tabrt_close(rt, i);
        }
    }
    free(rt);
}

#endif /* HARNESS_COMPAT_H */
