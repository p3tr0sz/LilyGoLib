/**
 * @file      capture_store.cpp
 * @brief     SD catalog helpers for captured signals. See capture_store.h.
 */
#include "capture_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ARDUINO
#include <SD.h>
#endif

void capture_make_name(const char *prefix, int n, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    time_t now = time(NULL);
    struct tm tmv;
    /* The RTC starts near the epoch before it's set; only trust a real date. */
    if (now > 1000000000L && localtime_r(&now, &tmv)) {
        snprintf(out, out_len, "%s_%d_%04d%02d%02d_%02d%02d%02d",
                 prefix, n,
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        snprintf(out, out_len, "%s_%d", prefix, n);
    }
}

bool capture_sanitize_name(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) return false;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        char c = in[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        /* No leading dot (hidden files) and never a path separator. */
        if (c == '/' || c == '\\') continue;
        if (ok && !(j == 0 && c == '.')) out[j++] = c;
    }
    out[j] = '\0';
    return j > 0;
}

void capture_path(const char *catalog, const char *name, const char *ext,
                  char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "/%s/%s%s", catalog, name, ext ? ext : "");
}

#ifdef ARDUINO

bool capture_catalog_ensure(const char *catalog)
{
    if (!catalog || !*catalog) return false;
    char dir[48];
    snprintf(dir, sizeof(dir), "/%s", catalog);
    if (SD.exists(dir)) return true;
    return SD.mkdir(dir);
}

int capture_list(const char *catalog, const char *ext, char names[][64], int max)
{
    if (!catalog || !ext || !names || max <= 0) return 0;
    char dir[48];
    snprintf(dir, sizeof(dir), "/%s", catalog);
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return 0;
    }
    const size_t extlen = strlen(ext);
    int count = 0;
    for (File f = root.openNextFile(); f && count < max; f = root.openNextFile()) {
        if (!f.isDirectory()) {
            /* f.name() may be a full path or a bare name depending on the core. */
            const char *nm = f.name();
            const char *slash = strrchr(nm, '/');
            if (slash) nm = slash + 1;
            size_t nlen = strlen(nm);
            if (nm[0] != '_' && nlen > extlen &&
                    strcmp(nm + nlen - extlen, ext) == 0) {
                size_t baselen = nlen - extlen;
                if (baselen > 63) baselen = 63;
                memcpy(names[count], nm, baselen);
                names[count][baselen] = '\0';
                count++;
            }
        }
        f.close();
    }
    root.close();
    return count;
}

#else   /* desktop simulator: no SD, no capture hardware */

bool capture_catalog_ensure(const char *catalog)
{
    (void)catalog;
    return false;
}

int capture_list(const char *catalog, const char *ext, char names[][64], int max)
{
    (void)catalog; (void)ext; (void)names; (void)max;
    return 0;
}

#endif
