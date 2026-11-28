/**
 * @file      word_list.c
 * @license   MIT
 *
 * A compact, frequency-ordered list of common English words used to offer
 * prefix completions on the watch keyboard. Lowercase ASCII only, to keep the
 * flash cost small and the prefix match simple. The order is roughly by
 * frequency so the earliest prefix matches are the most useful.
 */
#include "word_list.h"
#include <ctype.h>
#include <string.h>

static const char *const kWords[] = {
    "the", "be", "and", "are", "about", "all", "also", "after", "again",
    "always", "another", "any", "around", "ask", "away", "back", "because",
    "been", "before", "being", "best", "better", "between", "both", "but",
    "call", "came", "can", "cannot", "come", "coming", "could", "day",
    "days", "did", "different", "does", "doing", "done", "down", "during",
    "each", "early", "enough", "even", "evening", "ever", "every", "everyone",
    "everything", "example", "far", "feel", "few", "find", "first", "follow",
    "following", "for", "found", "friend", "friends", "from", "get", "getting",
    "give", "given", "going", "gone", "good", "great", "group", "had", "happy",
    "has", "have", "having", "hello", "help", "her", "here", "high", "him",
    "his", "home", "hope", "hour", "how", "however", "idea", "important", "info",
    "information", "inside", "instead", "into", "its", "just", "keep", "kind",
    "know", "known", "large", "last", "late", "later", "least", "leave", "left",
    "less", "let", "letter", "life", "like", "line", "little", "live", "long",
    "look", "looking", "lot", "love", "made", "make", "making", "many", "may",
    "maybe", "mean", "meet", "meeting", "might", "mind", "minute", "money",
    "month", "more", "morning", "most", "move", "much", "must", "name", "near",
    "need", "never", "new", "news", "next", "nice", "night", "not", "nothing",
    "now", "number", "off", "offer", "office", "often", "okay", "old", "once",
    "one", "only", "open", "order", "other", "our", "out", "over", "own",
    "part", "people", "person", "phone", "place", "plan", "please", "point",
    "possible", "problem", "program", "put", "question", "quick", "rather",
    "read", "ready", "real", "really", "reason", "remember", "right", "room",
    "run", "said", "same", "saw", "say", "see", "seem", "seen", "send",
    "sent", "service", "several", "she", "short", "should", "show", "side",
    "since", "small", "some", "someone", "something", "sometimes", "soon",
    "sorry", "sound", "start", "state", "still", "stop", "story", "such",
    "sure", "system", "take", "talk", "tell", "than", "thank", "thanks",
    "that", "their", "them", "then", "there", "these", "they", "thing",
    "things", "think", "this", "those", "though", "thought", "three",
    "through", "time", "today", "together", "tomorrow", "tonight", "too",
    "took", "top", "town", "true", "try", "turn", "two", "under", "until",
    "upon", "use", "used", "using", "very", "wait", "walk", "want", "was",
    "watch", "water", "way", "week", "weekend", "welcome", "well", "went",
    "were", "what", "when", "where", "whether", "which", "while", "who",
    "whole", "why", "will", "with", "within", "without", "word", "words",
    "work", "working", "world", "would", "write", "wrong", "year", "years",
    "yes", "yet", "you", "young", "your", "yours",
};

#define WORD_COUNT ((int)(sizeof(kWords) / sizeof(kWords[0])))

int word_suggest(const char *prefix, const char **out, int max)
{
    if (!prefix || !out || max <= 0) {
        return 0;
    }

    size_t plen = strlen(prefix);
    if (plen == 0) {
        return 0;
    }

    char low[32];
    if (plen >= sizeof(low)) {
        return 0;   /* Longer than any real prefix we care to match */
    }
    for (size_t i = 0; i < plen; ++i) {
        low[i] = (char)tolower((unsigned char)prefix[i]);
    }
    low[plen] = '\0';

    int n = 0;
    for (int i = 0; i < WORD_COUNT && n < max; ++i) {
        const char *w = kWords[i];
        /* Match the prefix, and require the word to be longer so there is
         * something left to complete (an exact match offers nothing). */
        if (strncmp(w, low, plen) == 0 && w[plen] != '\0') {
            out[n++] = w;
        }
    }
    return n;
}
