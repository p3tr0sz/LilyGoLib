/**
 * @file      word_list.h
 * @license   MIT
 *
 * A small built-in English word list for keyboard word completion on the watch.
 */
#ifndef FACTORY_WORD_LIST_H
#define FACTORY_WORD_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill out[] with up to max completions whose start matches prefix
 * (case-insensitive, ASCII only). A word equal to the prefix is skipped, since
 * there is nothing to complete. Returns the number of pointers written; each
 * points at a static lowercase string that outlives the call.
 */
int word_suggest(const char *prefix, const char **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_WORD_LIST_H */
