/**
 * @file      web_json.cpp
 * @brief     Minimal JSON for the web API: write string literals, read fields of a flat object.
 *            The API only exchanges small flat objects, so this avoids a JSON library on both
 *            the watch and the simulator.
 */
#include "web_internal.h"

#if FACTORY_WEB

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>

void web_json_str(std::string &out, const char *s)
{
    out += '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*p < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", *p);
                out += b;
            } else {
                out += (char)*p;
            }
        }
    }
    out += '"';
}

/* ---- reader ---- */

static size_t skip_ws(const std::string &s, size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

/* Parse a string literal at s[i] == '"'. Returns the index after it, or npos. */
static size_t parse_string(const std::string &s, size_t i, std::string *out)
{
    if (i >= s.size() || s[i] != '"') return std::string::npos;
    i++;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return i;
        if (c != '\\') {
            if (out) *out += c;
            continue;
        }
        if (i >= s.size()) return std::string::npos;
        char e = s[i++];
        switch (e) {
        case '"': case '\\': case '/': if (out) *out += e; break;
        case 'n': if (out) *out += '\n'; break;
        case 'r': if (out) *out += '\r'; break;
        case 't': if (out) *out += '\t'; break;
        case 'b': if (out) *out += '\b'; break;
        case 'f': if (out) *out += '\f'; break;
        case 'u': {
            if (i + 4 > s.size()) return std::string::npos;
            unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), NULL, 16);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                unsigned lo = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), NULL, 16);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (out) {
                if (cp < 0x80) *out += (char)cp;
                else if (cp < 0x800) { *out += (char)(0xC0 | (cp >> 6)); *out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { *out += (char)(0xE0 | (cp >> 12)); *out += (char)(0x80 | ((cp >> 6) & 0x3F)); *out += (char)(0x80 | (cp & 0x3F)); }
                else { *out += (char)(0xF0 | (cp >> 18)); *out += (char)(0x80 | ((cp >> 12) & 0x3F)); *out += (char)(0x80 | ((cp >> 6) & 0x3F)); *out += (char)(0x80 | (cp & 0x3F)); }
            }
            break;
        }
        default: return std::string::npos;
        }
    }
    return std::string::npos;
}

/* Skip any JSON value at s[i]. Returns the index after it, or npos. */
static size_t skip_value(const std::string &s, size_t i)
{
    i = skip_ws(s, i);
    if (i >= s.size()) return std::string::npos;
    char c = s[i];
    if (c == '"') return parse_string(s, i, NULL);
    if (c == '{' || c == '[') {
        int depth = 0;
        while (i < s.size()) {
            char d = s[i];
            if (d == '"') {
                i = parse_string(s, i, NULL);
                if (i == std::string::npos) return i;
                continue;
            }
            if (d == '{' || d == '[') depth++;
            else if (d == '}' || d == ']') {
                if (--depth == 0) return i + 1;
            }
            i++;
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
            s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') i++;
    return i;
}

/* Find the raw value of a top-level key. Returns its [start, end) or false. */
static bool find_value(const std::string &s, const char *key, size_t *vs, size_t *ve)
{
    size_t i = skip_ws(s, 0);
    if (i >= s.size() || s[i] != '{') return false;
    i++;
    while (true) {
        i = skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') return false;
        std::string k;
        i = parse_string(s, i, &k);
        if (i == std::string::npos) return false;
        i = skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        i = skip_ws(s, i + 1);
        size_t end = skip_value(s, i);
        if (end == std::string::npos) return false;
        if (k == key) {
            *vs = i;
            *ve = end;
            return true;
        }
        i = skip_ws(s, end);
        if (i < s.size() && s[i] == ',') {
            i++;
            continue;
        }
        return false;
    }
}

bool web_json_has(const std::string &json, const char *key)
{
    size_t a, b;
    return find_value(json, key, &a, &b);
}

bool web_json_get_str(const std::string &json, const char *key, std::string &out)
{
    size_t a, b;
    if (!find_value(json, key, &a, &b) || json[a] != '"') return false;
    out.clear();
    return parse_string(json, a, &out) != std::string::npos;
}

bool web_json_get_num(const std::string &json, const char *key, double *out)
{
    size_t a, b;
    if (!find_value(json, key, &a, &b)) return false;
    char c = json[a];
    if (!(c == '-' || (c >= '0' && c <= '9'))) return false;
    std::string v = json.substr(a, b - a);
    char *end = NULL;
    double d = strtod(v.c_str(), &end);
    if (!end || *end) return false;
    *out = d;
    return true;
}

bool web_json_get_bool(const std::string &json, const char *key, bool *out)
{
    size_t a, b;
    if (!find_value(json, key, &a, &b)) return false;
    std::string v = json.substr(a, b - a);
    if (v == "true") { *out = true; return true; }
    if (v == "false") { *out = false; return true; }
    return false;
}

bool web_json_is_object(const std::string &json)
{
    size_t i = skip_ws(json, 0);
    if (i >= json.size() || json[i] != '{') return false;
    size_t end = skip_value(json, i);
    return end != std::string::npos && skip_ws(json, end) == json.size();
}

/* The raw text of a top-level key's value (a nested object/array kept verbatim for re-parsing),
 * trimmed of surrounding whitespace. false if the key is absent. */
bool web_json_raw(const std::string &json, const char *key, std::string &out)
{
    size_t a, b;
    if (!find_value(json, key, &a, &b)) return false;
    out.assign(json, a, b - a);
    return true;
}

/* The top-level member keys of a JSON object, in document order. */
void web_json_object_keys(const std::string &json, std::vector<std::string> &out)
{
    out.clear();
    size_t i = skip_ws(json, 0);
    if (i >= json.size() || json[i] != '{') return;
    i++;
    while (true) {
        i = skip_ws(json, i);
        if (i >= json.size() || json[i] == '}') return;
        std::string k;
        i = parse_string(json, i, &k);
        if (i == std::string::npos) return;
        i = skip_ws(json, i);
        if (i >= json.size() || json[i] != ':') return;
        i = skip_ws(json, i + 1);
        size_t end = skip_value(json, i);
        if (end == std::string::npos) return;
        out.push_back(k);
        i = skip_ws(json, end);
        if (i < json.size() && json[i] == ',') { i++; continue; }
        return;
    }
}

/* The raw element substrings of a top-level JSON array, in order. */
void web_json_array_items(const std::string &json, std::vector<std::string> &out)
{
    out.clear();
    size_t i = skip_ws(json, 0);
    if (i >= json.size() || json[i] != '[') return;
    i++;
    while (true) {
        i = skip_ws(json, i);
        if (i >= json.size() || json[i] == ']') return;
        size_t end = skip_value(json, i);
        if (end == std::string::npos) return;
        out.push_back(json.substr(i, end - i));
        i = skip_ws(json, end);
        if (i < json.size() && json[i] == ',') { i++; continue; }
        return;
    }
}

#endif /* FACTORY_WEB */
