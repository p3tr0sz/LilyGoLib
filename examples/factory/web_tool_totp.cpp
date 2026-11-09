/**
 * @file      web_tool_totp.cpp
 * @brief     TOTP authenticator (RFC 6238 / RFC 4226) shared by the web TOTP app and the native
 *            on-device TOTP app. Secrets live on the watch (/sd/totp/accounts.tsv) and the codes
 *            are computed on the watch from its own clock — the browser only displays them, so a
 *            shared secret never has to leave the device to be useful.
 *
 * Self-contained crypto: a small SHA-1 + HMAC-SHA1 (RFC 2104) and a Base32 (RFC 4648) decoder are
 * implemented here so the exact same bytes are produced on the watch and in the host simulator with
 * no platform crypto dependency. v1 is HMAC-SHA1 only (what Google Authenticator, Authy, 1Password
 * and the like emit); SHA-256/512 variants are a later addition.
 *
 * Routes (docs/web-api.md, TOTP; cap "totp"):
 *   GET  /api/totp[?at=<epoch>]     list accounts with their current code + seconds remaining
 *   POST /api/totp/add              {issuer?, account?, secret, digits?, period?} -> {ok, id}
 *   POST /api/totp/delete           {id} -> {ok}
 *   POST /api/totp/preview          {secret, at?, digits?, period?} -> {code, remaining_s}  (no storage)
 *
 * otpauth:// URIs are parsed in the browser into these fields, so the firmware stays field-only.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "web_internal.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <string>
#include <vector>

#define TOTP_DIR        "/sd/totp"
#define TOTP_FILE       "/sd/totp/accounts.tsv"
#define TOTP_MAX        50              /* most accounts stored */
#define TOTP_FIELD_MAX  64              /* longest issuer / account */
#define TOTP_SECRET_MAX 128             /* longest decoded secret, bytes */

/* ---------------------------------------------------------------- SHA-1 (RFC 3174) */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static inline uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if (i < 20)      { t = (b & d) | (~b & e);           k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                    k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);  k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                    k = 0xCA62C1D6; }
        uint32_t tmp = rol(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; c->n = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *p, size_t len)
{
    c->len += len;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->n != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

static void hmac_sha1(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[20])
{
    uint8_t k[64] = {0};
    if (keylen > 64) { sha1_ctx c; sha1_init(&c); sha1_update(&c, key, keylen); sha1_final(&c, k); }
    else memcpy(k, key, keylen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    uint8_t inner[20];
    sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, msglen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* ---------------------------------------------------------------- Base32 (RFC 4648) */

/* Decode a Base32 secret (case-insensitive, spaces and '=' ignored). false on an illegal char. */
static bool base32_decode(const std::string &in, std::string &out)
{
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '-') continue;
        int d;
        if (ch >= 'A' && ch <= 'Z') d = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a';
        else if (ch >= '2' && ch <= '7') d = ch - '2' + 26;
        else return false;
        v = (v << 5) | (uint32_t)d;
        bits += 5;
        if (bits >= 8) { bits -= 8; out += (char)((v >> bits) & 0xFF); if (out.size() > TOTP_SECRET_MAX) return false; }
    }
    return !out.empty();
}

/* ---------------------------------------------------------------- HOTP / TOTP */

/* Compute the RFC 6238 code for a Base32 secret at a given epoch. out holds `digits` chars + NUL. */
static bool totp_code(const std::string &secret_b32, long long epoch, int digits, int period, char *out)
{
    if (digits < 6 || digits > 8 || period < 1) return false;
    std::string key;
    if (!base32_decode(secret_b32, key)) return false;
    long long counter = epoch / period;
    if (counter < 0) counter = 0;
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)(counter & 0xFF); counter >>= 8; }
    uint8_t mac[20];
    hmac_sha1((const uint8_t *)key.data(), key.size(), msg, 8, mac);
    int off = mac[19] & 0x0F;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7F) << 24) | ((uint32_t)mac[off + 1] << 16) |
                   ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
    uint32_t mod = 1;
    for (int i = 0; i < digits; i++) mod *= 10;
    snprintf(out, digits + 1, "%0*u", digits, bin % mod);
    return true;
}

/* ---------------------------------------------------------------- storage (TSV) */

struct totp_acct { std::string issuer, account, secret; int digits, period; };

static bool field_ok(const std::string &s)
{
    if (s.size() > TOTP_FIELD_MAX) return false;
    for (char c : s) if (c == '\t' || c == '\n' || c == '\r' || (unsigned char)c < 0x20) return false;
    return true;
}

static void load_accounts(std::vector<totp_acct> &out)
{
    out.clear();
    std::string host;
    if (web_fs_resolve(TOTP_FILE, host)) return;
    FILE *f = fopen(host.c_str(), "rb");
    if (!f) return;
    std::string all;
    char chunk[512];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) all.append(chunk, rd);
    fclose(f);
    size_t pos = 0;
    while (pos < all.size() && out.size() < TOTP_MAX) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        if (line.empty()) continue;
        std::string f_[5];
        int fi = 0;
        size_t fp = 0;
        for (; fi < 5; fi++) {
            size_t tab = line.find('\t', fp);
            f_[fi] = line.substr(fp, tab == std::string::npos ? std::string::npos : tab - fp);
            if (tab == std::string::npos) { fi++; break; }
            fp = tab + 1;
        }
        if (fi < 5) continue;
        totp_acct a;
        a.issuer = f_[0]; a.account = f_[1]; a.secret = f_[2];
        a.digits = atoi(f_[3].c_str()); a.period = atoi(f_[4].c_str());
        if (a.digits < 6 || a.digits > 8) a.digits = 6;
        if (a.period < 1) a.period = 30;
        out.push_back(a);
    }
}

static bool save_accounts(const std::vector<totp_acct> &list)
{
    std::string dirhost;
    if (web_fs_resolve(TOTP_DIR, dirhost)) return false;
    mkdir(dirhost.c_str(), 0777);
    std::string host;
    if (web_fs_resolve(TOTP_FILE, host)) return false;
    FILE *f = fopen(host.c_str(), "wb");
    if (!f) return false;
    for (const totp_acct &a : list) {
        std::string line = a.issuer + "\t" + a.account + "\t" + a.secret + "\t" +
                           tools_int(a.digits) + "\t" + tools_int(a.period) + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
    return true;
}

/* ---------------------------------------------------------------- routes */

static void append_account_json(std::string &j, int id, const totp_acct &a, long long epoch)
{
    char code[9] = {0};
    bool ok = totp_code(a.secret, epoch, a.digits, a.period, code);
    long long rem = a.period - (epoch % a.period);
    j += "{\"id\":" + tools_int(id) + ",\"issuer\":";
    web_json_str(j, a.issuer.c_str());
    j += ",\"account\":";
    web_json_str(j, a.account.c_str());
    j += ",\"digits\":" + tools_int(a.digits) + ",\"period\":" + tools_int(a.period) + ",\"code\":";
    if (ok) web_json_str(j, code); else j += "null";
    j += ",\"remaining_s\":" + tools_int(rem) + "}";
}

static long long totp_at(web_req_t *r, long long fallback)
{
    char buf[24] = {0};
    if (web_query_get(r, "at", buf, sizeof(buf)) && buf[0]) {
        long long v = atoll(buf);
        if (v > 0) return v;
    }
    return fallback;
}

static bool totp_handle(web_req_t *r)
{
    if (tools_is(r, "GET", "/api/totp")) {
        long long epoch = totp_at(r, tools_epoch());
        std::vector<totp_acct> list;
        load_accounts(list);
        std::string j = "{\"epoch\":" + tools_int(epoch) + ",\"accounts\":[";
        for (size_t i = 0; i < list.size(); i++) {
            if (i) j += ',';
            append_account_json(j, (int)i, list[i], epoch);
        }
        j += "]}";
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "POST", "/api/totp/add")) {
        std::string body;
        if (!tools_read_json(r, body, 2048)) return true;
        totp_acct a;
        web_json_get_str(body, "issuer", a.issuer);
        web_json_get_str(body, "account", a.account);
        std::string secret;
        web_json_get_str(body, "secret", secret);
        double d = 6, p = 30;
        web_json_get_num(body, "digits", &d);
        web_json_get_num(body, "period", &p);
        a.digits = (int)d; a.period = (int)p;
        if (a.digits < 6 || a.digits > 8) { web_resp_error(r, 400, "bad_request", "digits must be 6-8"); return true; }
        if (a.period < 10 || a.period > 120) { web_resp_error(r, 400, "bad_request", "period must be 10-120"); return true; }
        if (a.issuer.empty() && a.account.empty()) { web_resp_error(r, 400, "bad_request", "issuer or account required"); return true; }
        if (!field_ok(a.issuer) || !field_ok(a.account)) { web_resp_error(r, 400, "bad_request", "issuer/account too long or has control chars"); return true; }
        /* Normalise + validate the secret: Base32 must decode, and we store the stripped upper form. */
        std::string decoded, norm;
        for (char c : secret) { if (c == ' ' || c == '-' || c == '=' ) continue; norm += (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
        if (!base32_decode(norm, decoded)) { web_resp_error(r, 400, "bad_secret", "not a valid Base32 secret"); return true; }
        a.secret = norm;
        std::vector<totp_acct> list;
        load_accounts(list);
        if (list.size() >= TOTP_MAX) { web_resp_error(r, 409, "full", "too many accounts"); return true; }
        list.push_back(a);
        if (!save_accounts(list)) { web_resp_error(r, 503, "sd_missing", "could not write to SD"); return true; }
        std::string j = "{\"ok\":true,\"id\":" + tools_int((int)list.size() - 1) + "}";
        web_resp_json(r, 200, j);
        return true;
    }

    if (tools_is(r, "POST", "/api/totp/delete")) {
        std::string body;
        if (!tools_read_json(r, body, 512)) return true;
        double idn = -1;
        if (!web_json_get_num(body, "id", &idn)) { web_resp_error(r, 400, "bad_request", "id"); return true; }
        int id = (int)idn;
        std::vector<totp_acct> list;
        load_accounts(list);
        if (id < 0 || id >= (int)list.size()) { web_resp_error(r, 404, "not_found", "no such account"); return true; }
        list.erase(list.begin() + id);
        if (!save_accounts(list)) { web_resp_error(r, 503, "sd_missing", "could not write to SD"); return true; }
        web_resp_ok(r);
        return true;
    }

    if (tools_is(r, "POST", "/api/totp/preview")) {
        std::string body;
        if (!tools_read_json(r, body, 1024)) return true;
        std::string secret;
        web_json_get_str(body, "secret", secret);
        double d = 6, p = 30, at = 0;
        web_json_get_num(body, "digits", &d);
        web_json_get_num(body, "period", &p);
        web_json_get_num(body, "at", &at);
        int digits = (int)d, period = (int)p;
        if (digits < 6 || digits > 8) { web_resp_error(r, 400, "bad_request", "digits must be 6-8"); return true; }
        if (period < 1) { web_resp_error(r, 400, "bad_request", "period"); return true; }
        long long epoch = at > 0 ? (long long)at : tools_epoch();
        std::string norm;
        for (char c : secret) { if (c == ' ' || c == '-' || c == '=') continue; norm += (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
        char code[9] = {0};
        if (!totp_code(norm, epoch, digits, period, code)) { web_resp_error(r, 400, "bad_secret", "not a valid Base32 secret"); return true; }
        long long rem = period - (epoch % period);
        std::string j = "{\"code\":";
        web_json_str(j, code);
        j += ",\"remaining_s\":" + tools_int(rem) + "}";
        web_resp_json(r, 200, j);
        return true;
    }

    return false;
}

/* ---------------------------------------------------------------- native app helpers (ui_totp.cpp) */

/* List accounts with their current code and seconds remaining, for the on-device app. */
void totp_list(std::vector<std::string> &labels, std::vector<std::string> &codes, std::vector<int> &remaining)
{
    labels.clear(); codes.clear(); remaining.clear();
    std::vector<totp_acct> list;
    load_accounts(list);
    long long epoch = tools_epoch();
    for (totp_acct &a : list) {
        std::string label = a.issuer;
        if (!a.account.empty()) label += (label.empty() ? "" : " - ") + a.account;
        if (label.empty()) label = "(account)";
        labels.push_back(label);
        char code[9] = {0};
        codes.push_back(totp_code(a.secret, epoch, a.digits, a.period, code) ? std::string(code) : std::string("------"));
        remaining.push_back((int)(a.period - (epoch % a.period)));
    }
}

/* Delete the account at a list index (same ordering as totp_list / GET /api/totp). */
bool totp_delete_index(int id)
{
    std::vector<totp_acct> list;
    load_accounts(list);
    if (id < 0 || id >= (int)list.size()) return false;
    list.erase(list.begin() + id);
    return save_accounts(list);
}

static const char *const TOTP_CAPS[] = { "totp", NULL };
const web_tool_t web_tool_totp = { TOTP_CAPS, totp_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
