#ifndef COMPLETION_H
#define COMPLETION_H

#include <stddef.h>
#include <stdbool.h>

#define COMPLETE_MAX_WORD 1024

/**
 * @brief Auto-completion candidates result structure for Task 10.
 */
typedef struct {
    size_t count;                       /**< Number of matching candidates */
    char **items;                       /**< Array of candidate filenames */
    char lcp[COMPLETE_MAX_WORD];        /**< Longest Common Prefix of all candidates */
    size_t word_start;                  /**< Start index of the word in line */
    size_t word_len;                    /**< Length of the word in line */
    char dir_prefix[COMPLETE_MAX_WORD]; /**< Directory prefix path if any (e.g. "subdir/") */
} CompletionResult;

/**
 * @brief Frees allocated CompletionResult items.
 */
void completion_result_free(CompletionResult *cr);

/**
 * @brief Extracts the word ending at cursor position, honoring quotes and backslash escapes.
 *
 * @param[in] line Input string.
 * @param[in] cursor Cursor position.
 * @param[out] out_len Length of extracted word.
 * @return Start index of the word in line.
 */
size_t complete_extract_word(const char *line, size_t cursor, size_t *out_len);

/**
 * @brief Strips escaping backslashes from a word.
 */
void complete_unescape_word(const char *in, size_t len, char *out, size_t osize);

/**
 * @brief Escapes shell metacharacters and spaces in a filename candidate.
 */
void complete_escape_filename(const char *in, char *out, size_t osize);

/**
 * @brief Decomposes word into directory prefix and filename prefix.
 */
void complete_decompose_word(const char *word, size_t len,
                             char *dir_pre, size_t dsize,
                             char *file_pre, size_t fsize);

/**
 * @brief Computes the Longest Common Prefix (LCP) across candidate items.
 */
void complete_compute_lcp(char * const *items, size_t count, char *out, size_t osize);

/**
 * @brief Collects candidate filename matches from the directory matching the word before cursor.
 *
 * @param[in] line Full input line.
 * @param[in] cursor Cursor position.
 * @param[in] cwd Tab's current working directory.
 * @param[out] cr Populated completion results.
 * @return 0 on success.
 */
int complete_file_candidates(const char *line, size_t cursor, const char *cwd, CompletionResult *cr);

/**
 * @brief Formats multiple candidates as a numbered list: "1. abc.txt   2. abcd.txt".
 *
 * @param[in] cr CompletionResult structure.
 * @param[in] width Available display width.
 * @return Heap-allocated string that the caller must free().
 */
char *complete_format_numbered_list(const CompletionResult *cr, size_t width);

#endif /* COMPLETION_H */
