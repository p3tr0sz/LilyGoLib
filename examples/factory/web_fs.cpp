/**
 * @file      web_fs.cpp
 * @brief     Web API v1 file routes (docs/web-api.md, Files) on /sd and /flash.
 *            POSIX file calls only: they work on the watch's VFS mounts (/sd, /ffat)
 *            and on the simulator's folders.
 */
#include "web_internal.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <memory>

#define FS_CHUNK    16384

/* ---------------------------------------------------------------- paths */

int web_fs_resolve(const char *api_path, std::string &host, std::string *root_out)
{
    if (!api_path || api_path[0] != '/') return 400;
    /* No "..", no empty segments, no backslashes. */
    const char *p = api_path;
    while (*p) {
        const char *seg = p + 1;
        const char *end = strchr(seg, '/');
        size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if ((n == 2 && !strncmp(seg, "..", 2)) || (n == 1 && seg[0] == '.')) return 400;
        if (n == 0 && end) return 400;
        if (!end) break;
        p = end;
    }
    if (strchr(api_path, '\\')) return 400;

    const char *rest;
    std::string root;
    if (!strncmp(api_path, "/sd", 3) && (api_path[3] == 0 || api_path[3] == '/')) {
        root = "sd";
        rest = api_path + 3;
    } else if (!strncmp(api_path, "/flash", 6) && (api_path[6] == 0 || api_path[6] == '/')) {
        root = "flash";
        rest = api_path + 6;
    } else {
        return 400;
    }
    if (root == "sd" && !web_fs_sd_mounted()) return 503;
    const char *dir = web_plat_root_dir(root.c_str());
    if (!dir) return 400;
    host = dir;
    if (*rest && strcmp(rest, "/")) host += rest;
    while (host.size() > 1 && host.back() == '/') host.pop_back();
    if (root_out) *root_out = root;
    return 0;
}

static bool fs_error(web_req_t *r, int code)
{
    if (code == 503) web_resp_error(r, 503, "sd_missing", "SD card not mounted");
    else web_resp_error(r, 400, "bad_request", "bad path");
    return false;
}

static bool is_root(const std::string &host)
{
    const char *sd = web_plat_root_dir("sd");
    const char *fl = web_plat_root_dir("flash");
    return (sd && host == sd) || (fl && host == fl);
}

static bool stat_path(const std::string &p, struct stat *st)
{
    web_plat_io_lock();
    bool ok = stat(p.c_str(), st) == 0;
    web_plat_io_unlock();
    return ok;
}

static bool mkdirs(const std::string &path)
{
    struct stat st;
    if (stat_path(path, &st)) return S_ISDIR(st.st_mode);
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(path.substr(0, slash))) return false;
    web_plat_io_lock();
    bool ok = mkdir(path.c_str(), 0775) == 0 || errno == EEXIST;
    web_plat_io_unlock();
    return ok;
}

static bool remove_tree(const std::string &path)
{
    struct stat st;
    if (!stat_path(path, &st)) return false;
    if (!S_ISDIR(st.st_mode)) {
        web_plat_io_lock();
        bool ok = unlink(path.c_str()) == 0;
        web_plat_io_unlock();
        return ok;
    }
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
    for (auto &n : names) {
        if (!remove_tree(path + "/" + n)) return false;
    }
    web_plat_io_lock();
    bool ok = rmdir(path.c_str()) == 0;
    web_plat_io_unlock();
    return ok;
}

static bool copy_tree(const std::string &from, const std::string &to)
{
    struct stat st;
    if (!stat_path(from, &st)) return false;
    if (S_ISDIR(st.st_mode)) {
        if (!mkdirs(to)) return false;
        std::vector<std::string> names;
        web_plat_io_lock();
        DIR *d = opendir(from.c_str());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            }
            closedir(d);
        }
        web_plat_io_unlock();
        for (auto &n : names) {
            if (!copy_tree(from + "/" + n, to + "/" + n)) return false;
        }
        return true;
    }
    web_plat_io_lock();
    FILE *in = fopen(from.c_str(), "rb");
    FILE *out = in ? fopen(to.c_str(), "wb") : NULL;
    web_plat_io_unlock();
    if (!in || !out) {
        if (in) fclose(in);
        return false;
    }
    std::unique_ptr<char[]> buf(new char[FS_CHUNK]);
    bool ok = true;
    for (;;) {
        web_plat_io_lock();
        size_t n = fread(buf.get(), 1, FS_CHUNK, in);
        size_t w = n ? fwrite(buf.get(), 1, n, out) : 0;
        web_plat_io_unlock();
        if (w != n) { ok = false; break; }
        if (n == 0) break;
    }
    web_plat_io_lock();
    fclose(in);
    if (fclose(out) != 0) ok = false;
    web_plat_io_unlock();
    return ok;
}

/* Rename that replaces an existing file (FAT's rename does not). */
static bool replace_file(const std::string &from, const std::string &to)
{
    web_plat_io_lock();
    bool ok = rename(from.c_str(), to.c_str()) == 0;
    if (!ok) {
        unlink(to.c_str());
        ok = rename(from.c_str(), to.c_str()) == 0;
    }
    web_plat_io_unlock();
    return ok;
}

static void publish_change(const char *op, const char *path, const char *to = NULL)
{
    std::string d = "{\"op\":";
    web_json_str(d, op);
    d += ",\"path\":";
    web_json_str(d, path);
    if (to) {
        d += ",\"to\":";
        web_json_str(d, to);
    }
    d += "}";
    web_events_publish("files.changed", d.c_str());
}

/* ---------------------------------------------------------------- routes */

static void handle_list(web_req_t *r, const char *api_path, const std::string &host)
{
    struct stat st;
    if (!stat_path(host, &st) || !S_ISDIR(st.st_mode)) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    struct entry_t { std::string name; bool dir; long long size; long long mtime; };
    std::vector<entry_t> list;
    std::vector<std::string> names;
    web_plat_io_lock();
    DIR *d = opendir(host.c_str());
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".tmp")) continue;
            names.push_back(e->d_name);
        }
        closedir(d);
    }
    web_plat_io_unlock();
    for (auto &n : names) {
        struct stat es;
        if (!stat_path(host + "/" + n, &es)) continue;
        bool dir = S_ISDIR(es.st_mode);
        list.push_back({ n, dir, dir ? 0 : (long long)es.st_size, (long long)es.st_mtime });
    }
    std::sort(list.begin(), list.end(), [](const entry_t &a, const entry_t &b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::string j = "{\"path\":";
    web_json_str(j, api_path);
    j += ",\"entries\":[";
    char b[96];
    for (size_t i = 0; i < list.size(); i++) {
        if (i) j += ",";
        j += "{\"name\":";
        web_json_str(j, list[i].name.c_str());
        snprintf(b, sizeof(b), ",\"dir\":%s,\"size\":%lld,\"mtime\":%lld}",
                 list[i].dir ? "true" : "false", list[i].size, list[i].mtime);
        j += b;
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void handle_read(web_req_t *r, const std::string &host)
{
    struct stat st;
    if (!stat_path(host, &st) || !S_ISREG(st.st_mode)) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    long long size = (long long)st.st_size, a = 0, b = size - 1;
    int status = 200;
    std::string h = "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n";
    if (r->range[0]) {
        long long ra = -1, rb = -1;
        const char *p = strstr(r->range, "bytes=");
        if (p) {
            p += 6;
            if (*p != '-') ra = strtoll(p, (char **)&p, 10);
            if (*p == '-') {
                p++;
                if (*p) rb = strtoll(p, NULL, 10);
            }
        }
        if (ra < 0 && rb >= 0) {            /* suffix: last rb bytes */
            ra = size - rb;
            rb = size - 1;
            if (ra < 0) ra = 0;
        }
        if (ra < 0) ra = 0;
        if (rb < 0 || rb >= size) rb = size - 1;
        if (ra > rb) {
            char cr[64];
            snprintf(cr, sizeof(cr), "Content-Range: bytes */%lld\r\n", size);
            r->begin(r, 416, "text/plain", cr, 0);
            r->end(r);
            return;
        }
        a = ra;
        b = rb;
        status = 206;
        char cr[96];
        snprintf(cr, sizeof(cr), "Content-Range: bytes %lld-%lld/%lld\r\n", a, b, size);
        h += cr;
    }
    web_plat_io_lock();
    FILE *f = fopen(host.c_str(), "rb");
    if (f && a) fseek(f, (long)a, SEEK_SET);
    web_plat_io_unlock();
    if (!f) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    long long left = size ? b - a + 1 : 0;
    if (r->begin(r, status, web_mime_type(host.c_str()), h.c_str(), left)) {
        std::unique_ptr<char[]> buf(new char[FS_CHUNK]);
        while (left > 0) {
            size_t want = left > FS_CHUNK ? FS_CHUNK : (size_t)left;
            web_plat_io_lock();
            size_t n = fread(buf.get(), 1, want, f);
            web_plat_io_unlock();
            if (n == 0 || !r->write(r, buf.get(), n)) break;
            left -= (long long)n;
        }
    }
    web_plat_io_lock();
    fclose(f);
    web_plat_io_unlock();
    r->end(r);
}

/* Drain the body into an open file. false on a read or write error. */
static bool body_to_file(web_req_t *r, FILE *f, long long *written)
{
    std::unique_ptr<char[]> buf(new char[FS_CHUNK]);
    *written = 0;
    for (;;) {
        int n = r->read(r, buf.get(), FS_CHUNK);
        if (n < 0) return false;
        if (n == 0) return true;
        web_plat_io_lock();
        size_t w = fwrite(buf.get(), 1, (size_t)n, f);
        web_plat_io_unlock();
        if (w != (size_t)n) return false;
        *written += n;
    }
}

static void handle_write(web_req_t *r, const char *api_path, const std::string &host)
{
    struct stat st;
    bool exists = stat_path(host, &st);
    if (exists && S_ISDIR(st.st_mode)) {
        web_resp_error(r, 409, "conflict", "a folder has that name");
        return;
    }
    size_t slash = host.rfind('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(host.substr(0, slash))) {
        web_resp_error(r, 500, "io_error", "could not create the folder");
        return;
    }
    char off[32];
    if (web_query_get(r, "offset", off, sizeof(off))) {
        char *end = NULL;
        long long at = strtoll(off, &end, 10);
        long long have = exists ? (long long)st.st_size : 0;
        if (!*off || (end && *end) || at != have) {
            /* Drain what the client sent before answering. */
            char sink[512];
            while (r->read(r, sink, sizeof(sink)) > 0) {}
            char extra[48];
            snprintf(extra, sizeof(extra), "\"size\":%lld", have);
            web_resp_error(r, 409, "conflict", "offset does not match", extra);
            return;
        }
        web_plat_io_lock();
        FILE *f = fopen(host.c_str(), "ab");
        web_plat_io_unlock();
        if (!f) {
            web_resp_error(r, 500, "io_error", "open");
            return;
        }
        long long written = 0;
        bool ok = body_to_file(r, f, &written);
        web_plat_io_lock();
        if (fclose(f) != 0) ok = false;
        web_plat_io_unlock();
        if (!ok) {
            web_resp_error(r, 500, "io_error", "write");
            return;
        }
        publish_change("write", api_path);
        char extra[48];
        snprintf(extra, sizeof(extra), "\"size\":%lld", have + written);
        web_resp_ok(r, extra);
        return;
    }

    /* Whole file: write <path>.tmp, then rename it into place. */
    std::string tmp = host + ".tmp";
    web_plat_io_lock();
    FILE *f = fopen(tmp.c_str(), "wb");
    web_plat_io_unlock();
    if (!f) {
        web_resp_error(r, 500, "io_error", "open");
        return;
    }
    long long written = 0;
    bool ok = body_to_file(r, f, &written);
    web_plat_io_lock();
    if (fclose(f) != 0) ok = false;
    web_plat_io_unlock();
    if (!ok || !replace_file(tmp, host)) {
        web_plat_io_lock();
        unlink(tmp.c_str());
        web_plat_io_unlock();
        web_resp_error(r, 500, "io_error", "write");
        return;
    }
    publish_change("write", api_path);
    char extra[48];
    snprintf(extra, sizeof(extra), "\"size\":%lld", written);
    web_resp_ok(r, extra);
}

static void handle_delete(web_req_t *r, const char *api_path, const std::string &host)
{
    struct stat st;
    if (!stat_path(host, &st)) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    if (is_root(host)) {
        web_resp_error(r, 400, "bad_request", "cannot delete a root");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char rec[8] = "";
        web_query_get(r, "recursive", rec, sizeof(rec));
        bool empty = true;
        web_plat_io_lock();
        DIR *d = opendir(host.c_str());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                    empty = false;
                    break;
                }
            }
            closedir(d);
        }
        web_plat_io_unlock();
        if (!empty && strcmp(rec, "1")) {
            web_resp_error(r, 409, "conflict", "folder not empty");
            return;
        }
    }
    if (!remove_tree(host)) {
        web_resp_error(r, 500, "io_error", "delete");
        return;
    }
    publish_change("delete", api_path);
    web_resp_ok(r);
}

static void handle_mkdir(web_req_t *r)
{
    std::string body, path, host;
    if (!web_read_body(r, body, 2048) || !web_json_get_str(body, "path", path)) {
        web_resp_error(r, 400, "bad_request", "path");
        return;
    }
    int e = web_fs_resolve(path.c_str(), host);
    if (e) {
        fs_error(r, e);
        return;
    }
    struct stat st;
    bool existed = stat_path(host, &st);
    if (existed && !S_ISDIR(st.st_mode)) {
        web_resp_error(r, 409, "conflict", "a file has that name");
        return;
    }
    if (!mkdirs(host)) {
        web_resp_error(r, 500, "io_error", "mkdir");
        return;
    }
    if (!existed) publish_change("mkdir", path.c_str());
    web_resp_ok(r);
}

static void handle_move(web_req_t *r)
{
    std::string body, from, to, hf, ht, rf, rt;
    if (!web_read_body(r, body, 4096) || !web_json_get_str(body, "from", from) || !web_json_get_str(body, "to", to)) {
        web_resp_error(r, 400, "bad_request", "from, to");
        return;
    }
    bool overwrite = false;
    web_json_get_bool(body, "overwrite", &overwrite);
    int e = web_fs_resolve(from.c_str(), hf, &rf);
    if (!e) e = web_fs_resolve(to.c_str(), ht, &rt);
    if (e) {
        fs_error(r, e);
        return;
    }
    if (is_root(hf) || is_root(ht)) {
        web_resp_error(r, 400, "bad_request", "cannot move a root");
        return;
    }
    struct stat st;
    if (!stat_path(hf, &st)) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    if (stat_path(ht, &st)) {
        if (!overwrite) {
            web_resp_error(r, 409, "conflict", "destination exists");
            return;
        }
        if (!remove_tree(ht)) {
            web_resp_error(r, 500, "io_error", "could not replace the destination");
            return;
        }
    }
    size_t slash = ht.rfind('/');
    if (slash != std::string::npos && slash > 0) mkdirs(ht.substr(0, slash));
    bool ok;
    if (rf == rt) {
        web_plat_io_lock();
        ok = rename(hf.c_str(), ht.c_str()) == 0;
        web_plat_io_unlock();
    } else {
        ok = copy_tree(hf, ht) && remove_tree(hf);
    }
    if (!ok) {
        web_resp_error(r, 500, "io_error", "move");
        return;
    }
    publish_change("move", from.c_str(), to.c_str());
    web_resp_ok(r);
}

void web_fs_handle(web_req_t *r)
{
    if (!strcmp(r->path, "/api/files/mkdir")) {
        if (strcmp(r->method, "POST")) return (void)web_resp_error(r, 405, "bad_request", "POST");
        return handle_mkdir(r);
    }
    if (!strcmp(r->path, "/api/files/move")) {
        if (strcmp(r->method, "POST")) return (void)web_resp_error(r, 405, "bad_request", "POST");
        return handle_move(r);
    }
    bool content = !strcmp(r->path, "/api/files/content");
    if (!content && strcmp(r->path, "/api/files")) return (void)web_resp_error(r, 404, "not_found");

    char path[512];
    if (!web_query_get(r, "path", path, sizeof(path))) return (void)web_resp_error(r, 400, "bad_request", "path");
    std::string host;
    int e = web_fs_resolve(path, host);
    if (e) {
        fs_error(r, e);
        return;
    }
    if (!content && !strcmp(r->method, "GET")) return handle_list(r, path, host);
    if (!content && !strcmp(r->method, "DELETE")) return handle_delete(r, path, host);
    if (content && !strcmp(r->method, "GET")) return handle_read(r, host);
    if (content && !strcmp(r->method, "PUT")) return handle_write(r, path, host);
    web_resp_error(r, 405, "bad_request", "method");
}

#endif /* FACTORY_WEB */
