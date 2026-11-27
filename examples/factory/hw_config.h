/**
 * @file      hw_config.h
 * @brief     Persistent settings files. Each config lives as a small JSON file in
 *            <internal flash>/config/<name> (the main copy, there without a card) and
 *            is mirrored to /sd/config/<name> on every write. A read falls back to the
 *            SD copy when the flash one is missing (and restores it to flash), so a
 *            fresh or fully erased watch picks its settings back up from the card.
 *
 *            Internal flash is the FFat partition: reflashing the firmware, even the
 *            merged image at 0x0, leaves it alone, unlike the NVS partition.
 */
#pragma once

#include <string>
#include <vector>

/* Reads <name>; false when neither copy exists. */
bool hw_config_read(const char *name, std::string &out);
/* Writes <name> to flash (atomically) and mirrors it to the SD card if one is in. */
bool hw_config_write(const char *name, const std::string &data);

/* Minimal JSON helpers for these files: flat objects of numbers, booleans,
 * strings and arrays. Lookups only see the object's own (top-level) keys. */
bool cfg_json_num(const std::string &obj, const char *key, double *out);  /* true/false read as 1/0 */
bool cfg_json_str(const std::string &obj, const char *key, std::string &out);
bool cfg_json_raw(const std::string &obj, const char *key, std::string &out);
void cfg_json_items(const std::string &arr, std::vector<std::string> &out);
void cfg_json_quote(std::string &out, const char *s);
