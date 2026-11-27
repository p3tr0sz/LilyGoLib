/**
 * @file      capture_store.h
 * @brief     Shared helpers for saving captured signals to per-type catalogs on
 *            the SD card, so every capture tool behaves the same way:
 *              - captures live in a top-level catalog folder: /nfc, /ir, /subghz, /nrf24
 *              - a new capture gets a default name "<prefix>_<n>_<YYYYMMDD_HHMMSS>"
 *                that the user can accept or rename before saving
 *              - a picker lists a catalog's files so a saved capture can be replayed
 *
 * The functions touch the SD card only on hardware; on the desktop simulator the
 * SD-backed ones are no-ops (there is no capture hardware there anyway).
 */
#pragma once

#include <stddef.h>

/* Format a default capture name "<prefix>_<n>_<YYYYMMDD_HHMMSS>" (or, when the
 * clock isn't set, "<prefix>_<n>") into out. n is the caller's session counter. */
void capture_make_name(const char *prefix, int n, char *out, size_t out_len);

/* Copy in to out keeping only filename-safe characters (A-Z a-z 0-9 _ - .) and
 * dropping any extension/path separators. Returns false when nothing is left. */
bool capture_sanitize_name(const char *in, char *out, size_t out_len);

/* Build "/<catalog>/<name><ext>" into out (ext may be "" or ".replay" etc.). */
void capture_path(const char *catalog, const char *name, const char *ext,
                  char *out, size_t out_len);

/* Make sure /<catalog> exists on the card. Returns false without an SD card. */
bool capture_catalog_ensure(const char *catalog);

/* List base names (without ext) of files in /<catalog> ending with ext, newest
 * entries not ordered. Names beginning with '_' (internal, e.g. _last) are
 * skipped. Returns the count written, up to max. */
int capture_list(const char *catalog, const char *ext, char names[][64], int max);
