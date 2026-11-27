/**
 * @file      hw_config.cpp
 * @brief     Settings files on internal flash, mirrored to the SD card. See hw_config.h.
 */
#include "hw_config.h"
#include "hal_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef ARDUINO
#include <FFat.h>
#endif

#define CONFIG_DIR      "config"
#define CONFIG_MAX_SIZE (32 * 1024)

/* ---------------------------------------------------------------- locations */

static const char *flash_root(void)
{
#ifdef ARDUINO
    /* The board mounts FFat during instance.begin() (USB MSC uses "/fs"); the web
     * layer and FIDO2 use "/ffat". Reuse whichever is live (as fido2_device.cpp does)
     * and only mount it ourselves when neither is. */
    static const char *root = NULL;
    if (root) return root;
    struct stat st;
    if (stat("/ffat", &st) == 0 && S_ISDIR(st.st_mode)) root = "/ffat";
    else if (stat("/fs", &st) == 0 && S_ISDIR(st.st_mode)) root = "/fs";
    else if (FFat.begin(false, "/ffat")) root = "/ffat";
    return root;
#else
    const char *env = getenv("TWATCH_SIM_FLASH");
    return env && *env ? env : "sim_flash";
#endif
}

static const char *sd_root(void)
{
#ifdef ARDUINO
    return hw_is_sd_insert() ? "/sd" : NULL;
#else
    /* The simulator's "card" is a folder; it counts as inserted when it exists. */
    const char *env = getenv("TWATCH_SIM_SD");
    const char *root = env && *env ? env : "sim_sd";
    struct stat st;
    return stat(root, &st) == 0 && S_ISDIR(st.st_mode) ? root : NULL;
#endif
}

static bool config_path(const char *root, const char *name, std::string &dir, std::string &path)
{
    if (!root || !name || !*name || strchr(name, '/')) return false;
    dir = std::string(root) + "/" CONFIG_DIR;
    path = dir + "/" + name;
    return true;
}

/* ---------------------------------------------------------------- files */

static bool read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > CONFIG_MAX_SIZE) { ok = false; break; }
    }
    fclose(f);
    return ok && !out.empty();
}

/* Write a temporary file, then swap it in. FAT can't rename over an existing file,
 * so the old one is removed first; read_copy() falls back to the .tmp if power is
 * lost between the two steps. */
static bool write_file(const std::string &dir, const std::string &path, const std::string &data)
{
    mkdir(dir.c_str(), 0775);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

static bool read_copy(const char *root, const char *name, std::string &out)
{
    std::string dir, path;
    if (!config_path(root, name, dir, path)) return false;
    return read_file(path, out) || read_file(path + ".tmp", out);
}

bool hw_config_read(const char *name, std::string &out)
{
    if (read_copy(flash_root(), name, out)) return true;
    if (!read_copy(sd_root(), name, out)) return false;
    /* Only the card has it (a new or erased watch): put it back on flash. */
    std::string dir, path;
    if (config_path(flash_root(), name, dir, path)) write_file(dir, path, out);
    printf("[config] restored %s from the SD card\n", name);
    return true;
}

bool hw_config_write(const char *name, const std::string &data)
{
    std::string dir, path;
    bool ok = config_path(flash_root(), name, dir, path) && write_file(dir, path, data);
    if (!ok) printf("[config] could not write %s to internal flash\n", name);
    if (config_path(sd_root(), name, dir, path) && !write_file(dir, path, data)) {
        printf("[config] could not mirror %s to the SD card\n", name);
    }
    return ok;
}

/* ---------------------------------------------------------------- JSON */

static size_t skip_ws(const std::string &s, size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

/* Index just past the value starting at i (after whitespace), or npos. */
static size_t skip_value(const std::string &s, size_t i)
{
    i = skip_ws(s, i);
    if (i >= s.size()) return std::string::npos;
    char c = s[i];
    if (c == '"') {
        for (i++; i < s.size(); i++) {
            if (s[i] == '\\') i++;
            else if (s[i] == '"') return i + 1;
        }
        return std::string::npos;
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        for (; i < s.size(); i++) {
            char d = s[i];
            if (d == '"') {
                size_t end = skip_value(s, i);
                if (end == std::string::npos) return end;
                i = end - 1;
            } else if (d == '{' || d == '[') {
                depth++;
            } else if (d == '}' || d == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
            s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
    return i;
}

static bool unquote(const std::string &raw, std::string &out)
{
    if (raw.size() < 2 || raw[0] != '"' || raw.back() != '"') return false;
    out.clear();
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\' || i + 2 >= raw.size()) { out += c; continue; }
        c = raw[++i];
        switch (c) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (i + 4 >= raw.size()) return false;
            unsigned cp = (unsigned)strtoul(raw.substr(i + 1, 4).c_str(), NULL, 16);
            i += 4;
            /* Basic Multilingual Plane only, as UTF-8; enough for SSIDs and passwords. */
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
        }
        default: out += c; break;   /* \" \\ \/ */
        }
    }
    return true;
}

bool cfg_json_raw(const std::string &obj, const char *key, std::string &out)
{
    size_t i = skip_ws(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return false;
    i++;
    while (true) {
        i = skip_ws(obj, i);
        if (i >= obj.size() || obj[i] == '}') return false;
        size_t kend = skip_value(obj, i);
        if (kend == std::string::npos) return false;
        std::string k;
        if (!unquote(obj.substr(i, kend - i), k)) return false;
        i = skip_ws(obj, kend);
        if (i >= obj.size() || obj[i] != ':') return false;
        size_t vstart = skip_ws(obj, i + 1);
        size_t vend = skip_value(obj, vstart);
        if (vend == std::string::npos) return false;
        if (k == key) { out = obj.substr(vstart, vend - vstart); return true; }
        i = skip_ws(obj, vend);
        if (i < obj.size() && obj[i] == ',') i++;
    }
}

bool cfg_json_num(const std::string &obj, const char *key, double *out)
{
    std::string raw;
    if (!cfg_json_raw(obj, key, raw) || raw.empty()) return false;
    if (raw == "true") { *out = 1; return true; }
    if (raw == "false") { *out = 0; return true; }
    char *end = NULL;
    double v = strtod(raw.c_str(), &end);
    if (end == raw.c_str()) return false;
    *out = v;
    return true;
}

bool cfg_json_str(const std::string &obj, const char *key, std::string &out)
{
    std::string raw;
    return cfg_json_raw(obj, key, raw) && unquote(raw, out);
}

void cfg_json_items(const std::string &arr, std::vector<std::string> &out)
{
    out.clear();
    size_t i = skip_ws(arr, 0);
    if (i >= arr.size() || arr[i] != '[') return;
    i++;
    while (true) {
        i = skip_ws(arr, i);
        if (i >= arr.size() || arr[i] == ']') return;
        size_t end = skip_value(arr, i);
        if (end == std::string::npos) return;
        out.push_back(arr.substr(i, end - i));
        i = skip_ws(arr, end);
        if (i < arr.size() && arr[i] == ',') i++;
    }
}

void cfg_json_quote(std::string &out, const char *s)
{
    out += '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                out += esc;
            } else {
                out += (char)*p;
            }
        }
    }
    out += '"';
}
