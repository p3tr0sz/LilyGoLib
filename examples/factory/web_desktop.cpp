/**
 * @file      web_desktop.cpp
 * @brief     The watch installs its web desktop by itself (docs/web-roadmap.md, "Getting the desktop
 *            onto the card"):
 *
 *            1. /sd/web holds a desktop for API 1 at least as new as this firmware wants: nothing to do.
 *            2. A twatch-sd-<version>.zip is in the card's root: unpack it (CRC-checked), check it
 *               against its manifest, swap it in as /sd/web, create data/. The ZIP stays.
 *            3. No ZIP, and online: download the ZIP this firmware pins (web_desktop_pin.h: name, size,
 *               SHA-256), resuming a cut download, then step 2. Anything else is refused, so a
 *               tampered download never reaches the browser.
 *
 *            Runs on its own task; GET /api/desktop shows it, POST /api/desktop/install starts it.
 */
#include "web_internal.h"

#if FACTORY_WEB

#include "web_desktop_pin.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <vector>
#include <memory>
#include <lvgl.h>

#define ENTRY_MAX           (8 * 1024 * 1024)      /* largest single file we unpack */
#define AUTO_RETRY_MS       (10 * 60 * 1000)       /* after a failed automatic download */

/* ---------------------------------------------------------------- state */

enum dstate_t { D_IDLE, D_CHECKING, D_DOWNLOADING, D_UNPACKING, D_ERROR };
static const char *const STATE_NAMES[] = { "idle", "checking", "downloading", "unpacking", "error" };

static std::recursive_mutex s_lock;
static dstate_t s_state = D_IDLE;
static bool s_busy = false;
static char s_pending[12] = "";             /* a request that came while busy */
static char s_installed[24] = "";
static char s_zip[64] = "";                 /* best twatch-sd-*.zip on the card */
static char s_source[12] = "";
static char s_error[120] = "";
static long long s_done = 0, s_total = 0;
static unsigned s_run = 0;                  /* +1 each time a check starts */
static uint32_t s_last_event_ms = 0;
static uint32_t s_auto_fail_ms = 0;
static bool s_auto_failed = false;
/* While unpacking, an error is held back until /sd/web.new is cleaned up, so nobody sees "error"
 * with a half-written folder still on the card. */
static bool s_defer_error = false;
static char s_deferred[120] = "";

std::string web_desktop_json(void)
{
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    std::string j = "{\"installed\":";
    if (s_installed[0]) web_json_str(j, s_installed); else j += "null";
    j += ",\"wanted\":";
    web_json_str(j, WEB_DESKTOP_VERSION);
    j += ",\"zip_on_card\":";
    if (s_zip[0]) web_json_str(j, s_zip); else j += "null";
    j += ",\"state\":";
    web_json_str(j, STATE_NAMES[s_state]);
    j += ",\"source\":";
    web_json_str(j, s_source);
    char b[80];
    snprintf(b, sizeof(b), ",\"run\":%u,\"progress\":{\"done\":%lld,\"total\":%lld}", s_run, s_done, s_total);
    j += b;
    j += ",\"error\":";
    if (s_state == D_ERROR) web_json_str(j, s_error); else j += "null";
    return j + "}";
}

static void publish(bool force)
{
    uint32_t now = lv_tick_get();
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (!force && lv_tick_diff(now, s_last_event_ms) < 300) return;
        s_last_event_ms = now;
    }
    web_events_publish("desktop", web_desktop_json().c_str());
}

static void set_state(dstate_t st, const char *error = NULL)
{
    if (st == D_ERROR && s_defer_error) {
        snprintf(s_deferred, sizeof(s_deferred), "%s", error ? error : "failed");
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_state = st;
        if (error) snprintf(s_error, sizeof(s_error), "%s", error);
        if (st != D_DOWNLOADING && st != D_UNPACKING) s_done = s_total = 0;
    }
    if (error) web_plat_log("[web] desktop: %s", error);
    publish(true);
}

static void set_progress(long long done, long long total)
{
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_done = done;
        s_total = total;
    }
    publish(false);
}

/* ---------------------------------------------------------------- helpers */

static std::string sd_path(const char *rel)
{
    const char *sd = web_plat_root_dir("sd");
    return std::string(sd ? sd : "/sd") + rel;
}

/* "1.2.10" vs "1.10.0": numeric, part by part. */
static int vcmp(const char *a, const char *b)
{
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!isdigit((unsigned char)*a) && !isdigit((unsigned char)*b)) break;
    }
    return 0;
}

static bool read_file(const std::string &path, std::string &out, size_t max)
{
    web_plat_io_lock();
    FILE *f = fopen(path.c_str(), "rb");
    bool ok = false;
    if (f) {
        out.clear();
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() <= max) out.append(buf, n);
        ok = out.size() <= max;
        fclose(f);
    }
    web_plat_io_unlock();
    return ok;
}

/* The desktop's version in <dir>/manifest.json, when it is for API 1. */
static bool desktop_version(const std::string &dir, char *out, size_t n)
{
    std::string m, v;
    double api = 0;
    if (!read_file(dir + "/manifest.json", m, 256 * 1024)) return false;
    if (!web_json_get_num(m, "api", &api) || api != 1 || !web_json_get_str(m, "version", v)) return false;
    snprintf(out, n, "%s", v.c_str());
    return true;
}

/* The newest twatch-sd-<version>.zip in the card's root (not a .part). */
static bool find_card_zip(char *name, size_t name_len, char *ver, size_t ver_len)
{
    std::string root = sd_path("");
    bool found = false;
    web_plat_io_lock();
    DIR *d = opendir(root.c_str());
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            if (strncasecmp(n, "twatch-sd-", 10) || l < 15 || strcasecmp(n + l - 4, ".zip")) continue;
            char v[24];
            size_t vl = l - 14;
            if (vl >= sizeof(v)) continue;
            memcpy(v, n + 10, vl);
            v[vl] = 0;
            if (!found || vcmp(v, ver) > 0) {
                snprintf(name, name_len, "%s", n);
                snprintf(ver, ver_len, "%s", v);
                found = true;
            }
        }
        closedir(d);
    }
    web_plat_io_unlock();
    return found;
}

static bool mkdirs(const std::string &path)
{
    struct stat st;
    web_plat_io_lock();
    bool exists = stat(path.c_str(), &st) == 0;
    web_plat_io_unlock();
    if (exists) return S_ISDIR(st.st_mode);
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(path.substr(0, slash))) return false;
    web_plat_io_lock();
    bool ok = mkdir(path.c_str(), 0775) == 0;
    web_plat_io_unlock();
    return ok;
}

static void remove_tree(const std::string &path)
{
    struct stat st;
    web_plat_io_lock();
    bool exists = stat(path.c_str(), &st) == 0;
    web_plat_io_unlock();
    if (!exists) return;
    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> names;
        web_plat_io_lock();
        DIR *d = opendir(path.c_str());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            }
            closedir(d);
        }
        web_plat_io_unlock();
        for (auto &n : names) remove_tree(path + "/" + n);
        web_plat_io_lock();
        rmdir(path.c_str());
        web_plat_io_unlock();
    } else {
        web_plat_io_lock();
        unlink(path.c_str());
        web_plat_io_unlock();
    }
}

/* ---- CRC-32 (ZIP) and SHA-256 (the pinned download) */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct sha256_t {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t n;
};

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, H0, sizeof(H0));
    s->len = 0;
    s->n = 0;
}

static void sha256_update(sha256_t *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k; p += k; n -= k;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}

static void sha256_hex(sha256_t *s, char out[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0;
    sha256_update(s, &pad, 1);
    while (s->n != 56) sha256_update(s, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", (unsigned)s->h[i]);
}

static bool file_sha256(const std::string &path, char out[65], long long *size)
{
    web_plat_io_lock();
    FILE *f = fopen(path.c_str(), "rb");
    web_plat_io_unlock();
    if (!f) return false;
    sha256_t s;
    sha256_init(&s);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[8192]);
    size_t n;
    *size = 0;
    for (;;) {
        web_plat_io_lock();
        n = fread(buf.get(), 1, 8192, f);
        web_plat_io_unlock();
        if (!n) break;
        sha256_update(&s, buf.get(), n);
        *size += (long long)n;
    }
    fclose(f);
    sha256_hex(&s, out);
    return true;
}

/* ---------------------------------------------------------------- unpack */

struct zentry_t {
    std::string name;
    uint16_t method;
    uint32_t crc, csize, usize, local;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool read_at(FILE *f, long off, void *buf, size_t n)
{
    web_plat_io_lock();
    bool ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
    web_plat_io_unlock();
    return ok;
}

static bool zip_directory(FILE *f, long size, std::vector<zentry_t> &out, std::string &err)
{
    long tail = size < 65557 ? size : 65557;
    std::vector<uint8_t> t((size_t)tail);
    if (tail < 22 || !read_at(f, size - tail, t.data(), (size_t)tail)) { err = "not a ZIP file"; return false; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (rd32(&t[(size_t)i]) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { err = "not a ZIP file"; return false; }
    uint16_t count = rd16(&t[(size_t)eocd + 10]);
    uint32_t cd_size = rd32(&t[(size_t)eocd + 12]), cd_off = rd32(&t[(size_t)eocd + 16]);
    if ((long)cd_off + (long)cd_size > size || cd_size > 4 * 1024 * 1024) { err = "damaged ZIP (directory)"; return false; }
    std::vector<uint8_t> cd(cd_size);
    if (!read_at(f, (long)cd_off, cd.data(), cd_size)) { err = "damaged ZIP (directory)"; return false; }
    size_t p = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) { err = "damaged ZIP (entry)"; return false; }
        zentry_t e;
        uint16_t flags = rd16(&cd[p + 8]);
        e.method = rd16(&cd[p + 10]);
        e.crc = rd32(&cd[p + 16]);
        e.csize = rd32(&cd[p + 20]);
        e.usize = rd32(&cd[p + 24]);
        uint16_t nl = rd16(&cd[p + 28]), xl = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
        e.local = rd32(&cd[p + 42]);
        if (p + 46 + nl > cd.size()) { err = "damaged ZIP (name)"; return false; }
        e.name.assign((const char *)&cd[p + 46], nl);
        p += 46 + nl + xl + cl;
        if (flags & 1) { err = "encrypted ZIP"; return false; }
        out.push_back(e);
    }
    return true;
}

static bool safe_rel(const std::string &rel)
{
    if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos) return false;
    size_t s = 0;
    while (s <= rel.size()) {
        size_t e = rel.find('/', s);
        std::string seg = rel.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (seg == ".." || seg == ".") return false;
        if (e == std::string::npos) break;
        s = e + 1;
    }
    return true;
}

static bool install_zip_steps(const std::string &zip_path);

/* Unpack a ZIP from the card into /sd/web.new, check it, swap it in. Sets the error on failure,
 * and never leaves a half-written /sd/web.new behind; the old desktop stays as it was. */
static bool install_zip(const std::string &zip_path)
{
    s_defer_error = true;
    s_deferred[0] = 0;
    bool ok = install_zip_steps(zip_path);
    s_defer_error = false;
    if (!ok) {
        remove_tree(sd_path("/web.new"));
        set_state(D_ERROR, s_deferred);
    } else {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_auto_failed = false;
    }
    return ok;
}

static bool install_zip_steps(const std::string &zip_path)
{
    set_state(D_UNPACKING);
    web_plat_io_lock();
    FILE *f = fopen(zip_path.c_str(), "rb");
    long size = 0;
    if (f && fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    web_plat_io_unlock();
    if (!f) { set_state(D_ERROR, "could not open the ZIP"); return false; }

    std::vector<zentry_t> entries;
    std::string err;
    if (!zip_directory(f, size, entries, err)) { fclose(f); set_state(D_ERROR, err.c_str()); return false; }

    /* Where the desktop is inside: web/ at the top, or under a folder (Finder wraps ZIPs in one). */
    std::string prefix;
    bool found = false;
    for (auto &e : entries) {
        if (e.name.compare(0, 9, "__MACOSX/") == 0) continue;
        size_t l = e.name.size();
        if (l >= 17 && e.name.compare(l - 17, 17, "web/manifest.json") == 0 && (l == 17 || e.name[l - 18] == '/')) {
            prefix = e.name.substr(0, l - 13);      /* ".../web/" */
            found = true;
            break;
        }
    }
    if (!found) { fclose(f); set_state(D_ERROR, "no web/manifest.json in the ZIP"); return false; }
    std::string card_root = prefix.substr(0, prefix.size() - 4);

    long long total = 0, done = 0;
    for (auto &e : entries) {
        if (e.name.compare(0, prefix.size(), prefix) == 0 && e.name.back() != '/') total += e.usize;
    }
    std::string stage = sd_path("/web.new");
    remove_tree(stage);
    if (!mkdirs(stage)) { fclose(f); set_state(D_ERROR, "could not write to the card"); return false; }

    for (auto &e : entries) {
        bool in_web = e.name.compare(0, prefix.size(), prefix) == 0;
        bool in_data = e.name.compare(0, card_root.size() + 5, card_root + "data/") == 0;
        if (in_data && e.name.back() == '/') {
            std::string rel = e.name.substr(card_root.size(), e.name.size() - card_root.size() - 1);
            if (safe_rel(rel)) mkdirs(sd_path(("/" + rel).c_str()));
            continue;
        }
        if (!in_web || e.name.back() == '/') continue;
        std::string rel = e.name.substr(prefix.size());
        if (!safe_rel(rel)) { fclose(f); set_state(D_ERROR, "a ZIP entry points outside the desktop"); return false; }
        if (e.usize > ENTRY_MAX || (e.method != 0 && e.method != 8)) { fclose(f); set_state(D_ERROR, "a ZIP entry is too big or uses an unknown compression"); return false; }
        uint8_t lh[30];
        if (!read_at(f, (long)e.local, lh, 30) || rd32(lh) != 0x04034b50) { fclose(f); set_state(D_ERROR, "damaged ZIP (local header)"); return false; }
        long data_off = (long)e.local + 30 + rd16(lh + 26) + rd16(lh + 28);
        std::vector<uint8_t> comp(e.csize), plain;
        if (e.csize && !read_at(f, data_off, comp.data(), e.csize)) { fclose(f); set_state(D_ERROR, "damaged ZIP (data)"); return false; }
        if (e.method == 0) {
            plain.swap(comp);
            if (plain.size() != e.usize) { fclose(f); set_state(D_ERROR, "damaged ZIP (size)"); return false; }
        } else {
            plain.resize(e.usize);
            if (!web_plat_inflate_raw(comp.data(), comp.size(), plain.data(), plain.size())) {
                fclose(f);
                set_state(D_ERROR, "damaged ZIP (deflate)");
                return false;
            }
        }
        if (crc32_update(0, plain.data(), plain.size()) != e.crc) {
            fclose(f);
            std::string m = "damaged ZIP: " + rel + " fails its CRC";
            set_state(D_ERROR, m.c_str());
            return false;
        }
        std::string out = stage + "/" + rel;
        size_t slash = out.rfind('/');
        mkdirs(out.substr(0, slash));
        web_plat_io_lock();
        FILE *o = fopen(out.c_str(), "wb");
        bool ok = o && fwrite(plain.data(), 1, plain.size(), o) == plain.size();
        if (o && fclose(o) != 0) ok = false;
        web_plat_io_unlock();
        if (!ok) { fclose(f); set_state(D_ERROR, "could not write to the card"); return false; }
        done += e.usize;
        set_progress(done, total);
    }
    fclose(f);

    /* Every file the manifest lists, with its size. */
    std::string m;
    char ver[24];
    if (!read_file(stage + "/manifest.json", m, 256 * 1024) || !desktop_version(stage, ver, sizeof(ver))) {
        set_state(D_ERROR, "the desktop in the ZIP is not for this watch (API 1)");
        return false;
    }
    size_t at = m.find("\"files\"");
    while (at != std::string::npos && (at = m.find("\"size\"", at)) != std::string::npos) {
        /* walk back to this file's name: the key before the {...} that holds "size" */
        size_t brace = m.rfind('{', at), q2 = m.rfind('"', brace), q1 = q2 == std::string::npos ? q2 : m.rfind('"', q2 - 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string name = m.substr(q1 + 1, q2 - q1 - 1);
        long long want = atoll(m.c_str() + m.find(':', at) + 1);
        struct stat st;
        web_plat_io_lock();
        bool ok = stat((stage + "/" + name).c_str(), &st) == 0 && (long long)st.st_size == want;
        web_plat_io_unlock();
        if (!ok) {
            std::string msg = "the ZIP is missing " + name;
            set_state(D_ERROR, msg.c_str());
            return false;
        }
        at++;
    }

    /* Swap: web -> web.old, web.new -> web, drop web.old. */
    std::string cur = sd_path("/web"), old = sd_path("/web.old");
    remove_tree(old);
    web_plat_io_lock();
    struct stat st;
    bool had = stat(cur.c_str(), &st) == 0;
    bool ok = (!had || rename(cur.c_str(), old.c_str()) == 0) && rename(stage.c_str(), cur.c_str()) == 0;
    if (!ok && had) rename(old.c_str(), cur.c_str());
    web_plat_io_unlock();
    if (!ok) { set_state(D_ERROR, "could not swap the new desktop in"); return false; }
    remove_tree(old);
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        snprintf(s_installed, sizeof(s_installed), "%s", ver);
    }
    web_plat_log("[web] desktop %s installed from %s", ver, zip_path.c_str());
    web_events_publish("files.changed", "{\"op\":\"write\",\"path\":\"/sd/web\"}");
    return true;
}

/* ---------------------------------------------------------------- download */

static bool download_pinned(void)
{
    std::string part = sd_path("/" WEB_DESKTOP_ZIP_NAME ".part"), final_path = sd_path("/" WEB_DESKTOP_ZIP_NAME);
    std::string url = web_plat_desktop_url(WEB_DESKTOP_VERSION, WEB_DESKTOP_ZIP_NAME);
    set_state(D_DOWNLOADING);
    web_plat_log("[web] desktop: downloading %s", url.c_str());
    int rc = web_plat_download(url.c_str(), part.c_str(), [](long long d, long long t) { set_progress(d, t); });
    if (rc != 0) {
        char m[96];
        if (rc == 404) snprintf(m, sizeof(m), "the release has no %s (HTTP 404)", WEB_DESKTOP_ZIP_NAME);
        else if (rc > 0) snprintf(m, sizeof(m), "download failed (HTTP %d); it resumes on the next try", rc);
        else snprintf(m, sizeof(m), "download cut off; it resumes on the next try");
        set_state(D_ERROR, m);
        return false;
    }
    char hex[65];
    long long size = 0;
    if (!file_sha256(part, hex, &size) || size != WEB_DESKTOP_ZIP_SIZE || strcmp(hex, WEB_DESKTOP_ZIP_SHA256)) {
        remove_tree(part);
        set_state(D_ERROR, "the downloaded ZIP is not the one this firmware expects (size or SHA-256); deleted");
        return false;
    }
    remove_tree(final_path);
    web_plat_io_lock();
    bool ok = rename(part.c_str(), final_path.c_str()) == 0;
    web_plat_io_unlock();
    if (!ok) { set_state(D_ERROR, "could not save the ZIP on the card"); return false; }
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        snprintf(s_zip, sizeof(s_zip), "%s", WEB_DESKTOP_ZIP_NAME);
    }
    return true;
}

/* ---------------------------------------------------------------- the job */

static void job(void *arg)
{
    char source[12];
    snprintf(source, sizeof(source), "%s", (const char *)arg);
    free(arg);
    for (;;) {
        bool is_auto = !strcmp(source, "auto");
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            snprintf(s_source, sizeof(s_source), "%s", source);
            s_run++;
            s_state = D_CHECKING;
        }
        publish(true);
        char installed[24] = "", zip[64] = "", zver[24] = "";
        bool have = desktop_version(sd_path("/web"), installed, sizeof(installed));
        bool have_zip = find_card_zip(zip, sizeof(zip), zver, sizeof(zver));
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            snprintf(s_installed, sizeof(s_installed), "%s", have ? installed : "");
            snprintf(s_zip, sizeof(s_zip), "%s", have_zip ? zip : "");
        }
        bool current = have && vcmp(installed, WEB_DESKTOP_VERSION) >= 0;
        bool ok = true;

        if (!strcmp(source, "card")) {
            if (!have_zip) { set_state(D_ERROR, "no twatch-sd-<version>.zip in the card's root"); ok = false; }
            else ok = install_zip(sd_path(("/" + std::string(zip)).c_str()));
        } else if (!strcmp(source, "download")) {
            ok = download_pinned() && install_zip(sd_path("/" WEB_DESKTOP_ZIP_NAME));
        } else if (!current) {
            /* auto: a card ZIP newer than what is installed first; then the pinned download. */
            if (have_zip && (!have || vcmp(zver, installed) > 0)) {
                ok = install_zip(sd_path(("/" + std::string(zip)).c_str()));
                if (ok) {
                    have = desktop_version(sd_path("/web"), installed, sizeof(installed));
                    current = have && vcmp(installed, WEB_DESKTOP_VERSION) >= 0;
                }
            }
            bool retry_ok;
            {
                std::lock_guard<std::recursive_mutex> lk(s_lock);
                retry_ok = !s_auto_failed || lv_tick_diff(lv_tick_get(), s_auto_fail_ms) > AUTO_RETRY_MS;
            }
            if (ok && !current && web_plat_online() && retry_ok) {
                ok = download_pinned() && install_zip(sd_path("/" WEB_DESKTOP_ZIP_NAME));
                std::lock_guard<std::recursive_mutex> lk(s_lock);
                s_auto_failed = !ok;
                if (!ok) s_auto_fail_ms = lv_tick_get();
            }
        }
        if (ok) set_state(D_IDLE);
        (void)is_auto;

        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (!s_pending[0]) {
            s_busy = false;
            return;
        }
        snprintf(source, sizeof(source), "%s", s_pending);
        s_pending[0] = 0;
    }
}

/* A card was just mounted: a new card is a new situation, so the pause after a failed automatic
 * download does not apply. */
void web_desktop_card_mounted(void)
{
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_auto_failed = false;
    }
    web_desktop_check("auto");
}

void web_desktop_check(const char *source)
{
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (s_busy) {
            /* An explicit request wins over a queued automatic one. */
            if (!s_pending[0] || strcmp(source, "auto")) snprintf(s_pending, sizeof(s_pending), "%s", source);
            return;
        }
        s_busy = true;
    }
    web_plat_spawn(job, strdup(source));
}

/* One line for the watch's Web page. */
void web_desktop_status_line(char *out, size_t n)
{
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    switch (s_state) {
    case D_DOWNLOADING:
        snprintf(out, n, "Downloading %d%%", s_total ? (int)(s_done * 100 / s_total) : 0);
        break;
    case D_UNPACKING:
        snprintf(out, n, "Installing %d%%", s_total ? (int)(s_done * 100 / s_total) : 0);
        break;
    case D_CHECKING:
        snprintf(out, n, "Checking");
        break;
    case D_ERROR:
        snprintf(out, n, "Not installed");
        break;
    default:
        snprintf(out, n, "%s", s_installed[0] ? s_installed : "Not installed");
    }
}

/* ---------------------------------------------------------------- routes */

bool web_desktop_handle(web_req_t *r)
{
    if (!strcmp(r->path, "/api/desktop") && !strcmp(r->method, "GET")) {
        web_resp_json(r, 200, web_desktop_json());
        return true;
    }
    if (!strcmp(r->path, "/api/desktop/install") && !strcmp(r->method, "POST")) {
        std::string body, source = "auto";
        web_read_body(r, body, 512);
        web_json_get_str(body, "source", source);
        if (source != "auto" && source != "card" && source != "download") {
            web_resp_error(r, 400, "bad_request", "source: auto, card or download");
            return true;
        }
        if (!web_fs_sd_mounted()) {
            web_resp_error(r, 503, "sd_missing", "SD card not mounted");
            return true;
        }
        if (source == "auto") {
            /* By hand, "auto" ignores the pause after a failed automatic download. */
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            s_auto_failed = false;
        }
        web_desktop_check(source.c_str());
        web_resp_json(r, 202, "{\"ok\":true}");
        return true;
    }
    return false;
}

#endif /* FACTORY_WEB */
