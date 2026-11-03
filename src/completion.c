#include "completion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>

void completion_result_free(CompletionResult *cr) {
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
    cr->dir_prefix[0] = '\0';
}

size_t complete_extract_word(const char *line, size_t cursor, size_t *out_len) {
    if (!line || cursor == 0) {
        if (out_len) *out_len = 0;
        return 0;
    }

    size_t start = cursor;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < cursor; i++) {
        if (line[i] == '\\' && !in_single) {
            i++;
            continue;
        }
        if (line[i] == '\'' && !in_double) {
            in_single = !in_single;
        } else if (line[i] == '"' && !in_single) {
            in_double = !in_double;
        }
    }

    if (in_single) {
        ssize_t s = (ssize_t)cursor - 1;
        while (s >= 0 && line[s] != '\'') s--;
        start = (s >= 0) ? (size_t)(s + 1) : 0;
        if (out_len) *out_len = cursor - start;
        return start;
    }

    if (in_double) {
        ssize_t s = (ssize_t)cursor - 1;
        while (s >= 0 && (line[s] != '"' || (s > 0 && line[s - 1] == '\\'))) s--;
        start = (s >= 0) ? (size_t)(s + 1) : 0;
        if (out_len) *out_len = cursor - start;
        return start;
    }

    ssize_t idx = (ssize_t)cursor - 1;
    while (idx >= 0) {
        char c = line[idx];
        if (c == ' ' || c == '\t' || c == '|' || c == '<' || c == '>' || c == '&' || c == ';') {
            size_t slashes = 0;
            ssize_t b = idx - 1;
            while (b >= 0 && line[b] == '\\') {
                slashes++;
                b--;
            }
            if ((slashes % 2) == 0) {
                break;
            }
        }
        idx--;
    }

    start = (size_t)(idx + 1);
    if (out_len) *out_len = cursor - start;
    return start;
}

void complete_unescape_word(const char *in, size_t len, char *out, size_t osize) {
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

void complete_escape_filename(const char *in, char *out, size_t osize) {
    if (!in || !out || osize == 0) return;
    size_t o = 0;
    size_t len = strlen(in);
    for (size_t i = 0; i < len && o + 2 < osize; i++) {
        char c = in[i];
        if (c == '/' && i == len - 1) {
            out[o++] = c;
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

void complete_decompose_word(const char *word, size_t len,
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

void complete_compute_lcp(char * const *items, size_t count, char *out, size_t osize) {
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

int complete_file_candidates(const char *line, size_t cursor, const char *cwd, CompletionResult *cr) {
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
    if (!cr->items) {
        closedir(d);
        return 0;
    }

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
                char **new_items = (char **)realloc(cr->items, sizeof(char *) * cap);
                if (!new_items) break;
                cr->items = new_items;
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

char *complete_format_numbered_list(const CompletionResult *cr, size_t width) {
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
