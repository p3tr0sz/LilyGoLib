/**
 * @file      web_tool_audio.cpp
 * @brief     Recorder and player (docs/web-api.md, "Recorder and player").
 *
 *            The recorder writes the microphone straight to <path>.tmp on its own task, then
 *            renames it into place, so a cut never leaves half a recording. The player hands a
 *            file to the watch's music player. The watch's Recorder, Player, Net Radio and Walkie
 *            apps win: 503 busy while open, and a recording stops when one opens.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <thread>

#define RATE            16000
#define SAMPLES_PER_MS  (RATE / 1000)
#define DEFAULT_S       60
#define MAX_S           600
#define WAV_HEADER      44
#define EVENT_MS        250

static const char *const APPS[] = { "Recorder", "Player", "Net Radio", "Walkie", NULL };

/* ---------------------------------------------------------------- recorder state */

struct web_recorder_t {
    std::mutex mx;                      /* guards the fields below, signals cv when active drops */
    std::condition_variable cv;
    bool active = false;
    bool failed = false;
    std::string path, host;
    uint32_t max_s = DEFAULT_S;
    uint32_t size = 0;
    std::atomic<bool> stop { false };
    std::atomic<uint32_t> samples { 0 };
    std::atomic<int> level { 0 };
};
static web_recorder_t s_rec;

static std::string rec_json(void)
{
    std::lock_guard<std::mutex> lk(s_rec.mx);
    std::string j = std::string("{\"state\":") + (s_rec.active ? "\"recording\"" : "\"idle\"") + ",\"path\":";
    if (s_rec.path.empty()) j += "null";
    else web_json_str(j, s_rec.path.c_str());
    return j + ",\"elapsed_ms\":" + tools_int(s_rec.samples / SAMPLES_PER_MS) + ",\"max_s\":" + tools_int(s_rec.max_s) +
           ",\"level\":" + tools_int(s_rec.active ? s_rec.level.load() : 0) + "}";
}

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    auto u32 = [&](int at, uint32_t v) { h[at] = v; h[at + 1] = v >> 8; h[at + 2] = v >> 16; h[at + 3] = v >> 24; };
    auto u16 = [&](int at, uint16_t v) { h[at] = v; h[at + 1] = v >> 8; };
    memcpy(h, "RIFF", 4);
    u32(4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);                         /* PCM */
    u16(22, 1);                         /* mono */
    u32(24, RATE);
    u32(28, RATE * 2);
    u16(32, 2);
    u16(34, 16);
    memcpy(h + 36, "data", 4);
    u32(40, data_bytes);
}

static int peak_level(const int16_t *s, int n)
{
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int a = abs((int)s[i]);
        if (a > peak) peak = a;
    }
    return peak * 100 / 32768;
}

/* Record into an open file until stopped, full or the time is up. false on a read or write error. */
static bool record_into(FILE *f, uint32_t max_samples)
{
    int16_t buf[320];
    uint32_t last_event = tools_now_ms();
    while (!s_rec.stop && s_rec.samples < max_samples) {
        size_t want = sizeof(buf) / sizeof(buf[0]);
        if (max_samples - s_rec.samples < want) want = max_samples - s_rec.samples;
        int n = web_hw_mic_read(buf, want);
        if (n < 0) return false;
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        s_rec.level = peak_level(buf, n);
        web_plat_io_lock();
        size_t written = fwrite(buf, 2, (size_t)n, f);
        web_plat_io_unlock();
        if (written != (size_t)n) return false;     /* card full or removed */
        s_rec.samples += (uint32_t)n;
        if (tools_now_ms() - last_event >= EVENT_MS) {
            last_event = tools_now_ms();
            web_events_publish("recorder", rec_json().c_str());
        }
    }
    return true;
}

/* Patch the header and move <host>.tmp into place (keep a partial take if it has sound). */
static bool finish_file(FILE *f, const std::string &host, uint32_t bytes, bool keep)
{
    uint8_t hdr[WAV_HEADER];
    wav_header(hdr, bytes);
    std::string tmp = host + ".tmp";
    web_plat_io_lock();
    fseek(f, 0, SEEK_SET);
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    ok = fclose(f) == 0 && ok;
    if (ok && keep) {
        remove(host.c_str());                       /* FAT's rename does not replace */
        ok = rename(tmp.c_str(), host.c_str()) == 0;
    } else {
        remove(tmp.c_str());
        ok = false;
    }
    web_plat_io_unlock();
    return ok;
}

static void recorder_task(void *arg)
{
    (void)arg;
    std::string host, path;
    uint32_t max_samples;
    {
        std::lock_guard<std::mutex> lk(s_rec.mx);
        host = s_rec.host;
        path = s_rec.path;
        max_samples = s_rec.max_s * RATE;
    }
    uint8_t hdr[WAV_HEADER];
    wav_header(hdr, 0);
    web_plat_io_lock();
    FILE *f = fopen((host + ".tmp").c_str(), "wb");
    bool ok = f && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    web_plat_io_unlock();

    if (ok) ok = record_into(f, max_samples);
    web_hw_mic_close();
    uint32_t bytes = s_rec.samples * 2;
    bool saved = f && finish_file(f, host, bytes, bytes > 0);

    web_plat_log("[web] recording %s: %u ms%s", path.c_str(), (unsigned)(bytes / 2 / SAMPLES_PER_MS),
                 ok ? "" : " (stopped by an error)");
    {
        std::lock_guard<std::mutex> lk(s_rec.mx);
        s_rec.size = WAV_HEADER + bytes;
        s_rec.failed = !saved;
        s_rec.active = false;
    }
    s_rec.cv.notify_all();
    if (saved) {
        std::string ev = "{\"op\":\"write\",\"path\":";
        web_json_str(ev, path.c_str());
        web_events_publish("files.changed", (ev + "}").c_str());
    }
    web_events_publish("recorder", rec_json().c_str());
}

static std::string default_path(void)
{
    struct tm t;
    hw_get_date_time(t);
    char b[80];
    snprintf(b, sizeof(b), "/sd/data/recordings/rec-%04d%02d%02d-%02d%02d%02d.wav",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return b;
}

static void make_parents(const std::string &host)
{
    web_plat_io_lock();
    for (size_t i = 1; i < host.size(); i++) {
        if (host[i] == '/') mkdir(host.substr(0, i).c_str(), 0775);
    }
    web_plat_io_unlock();
}

/* API path -> host path; answers 400/503 and returns false when it cannot be used. */
static bool resolve(web_req_t *r, const std::string &path, std::string &host, std::string *root = NULL)
{
    int st = web_fs_resolve(path.c_str(), host, root);
    if (st == 503) web_resp_error(r, 503, "sd_missing", "SD card not mounted");
    else if (st) web_resp_error(r, 400, "bad_request", "bad path");
    return st == 0;
}

static void start_route(web_req_t *r)
{
    std::string body, path, host;
    if (!tools_read_json(r, body)) return;
    double max_s = DEFAULT_S;
    if (web_json_has(body, "max_s") && (!web_json_get_num(body, "max_s", &max_s) || max_s < 1 || max_s > MAX_S)) {
        web_resp_error(r, 400, "bad_request", "max_s: 1-600");
        return;
    }
    if (!web_json_has(body, "path")) path = default_path();
    else if (!web_json_get_str(body, "path", path) || path.size() < 5 || strcasecmp(path.c_str() + path.size() - 4, ".wav")) {
        web_resp_error(r, 400, "bad_request", "path must end in .wav");
        return;
    }
    if (tools_busy(r, APPS) || !resolve(r, path, host)) return;
    {
        std::lock_guard<std::mutex> lk(s_rec.mx);
        if (s_rec.active) {
            web_resp_error(r, 409, "conflict", "already recording");
            return;
        }
    }
    make_parents(host);
    if (!web_hw_mic_open()) {
        web_resp_error(r, 500, "internal", "the microphone did not open");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s_rec.mx);
        s_rec.active = true;
        s_rec.failed = false;
        s_rec.path = path;
        s_rec.host = host;
        s_rec.max_s = (uint32_t)max_s;
        s_rec.size = 0;
        s_rec.stop = false;
        s_rec.samples = 0;
        s_rec.level = 0;
    }
    web_plat_spawn(recorder_task, NULL);
    web_plat_log("[web] recording to %s", path.c_str());
    web_events_publish("recorder", rec_json().c_str());
    std::string extra = "\"path\":";
    web_json_str(extra, path.c_str());
    web_resp_ok(r, extra.c_str());
}

static void stop_route(web_req_t *r)
{
    std::unique_lock<std::mutex> lk(s_rec.mx);
    if (!s_rec.active) {
        web_resp_error(r, 409, "conflict", "not recording");
        return;
    }
    s_rec.stop = true;
    if (!s_rec.cv.wait_for(lk, std::chrono::seconds(4), [] { return !s_rec.active; })) {
        web_resp_error(r, 500, "internal", "the recording did not stop");
        return;
    }
    if (s_rec.failed) {
        web_resp_error(r, 500, "internal", "the recording could not be written");
        return;
    }
    std::string extra = "\"path\":";
    web_json_str(extra, s_rec.path.c_str());
    extra += ",\"duration_ms\":" + tools_int(s_rec.samples / SAMPLES_PER_MS) + ",\"size\":" + tools_int(s_rec.size);
    lk.unlock();
    web_resp_ok(r, extra.c_str());
}

static void recorder_second(uint32_t now)
{
    (void)now;
    bool active;
    {
        std::lock_guard<std::mutex> lk(s_rec.mx);
        active = s_rec.active;
    }
    if (active && tools_holder(APPS)) s_rec.stop = true;    /* the task finishes the file and says so */
}

static void recorder_stop(void)
{
    s_rec.stop = true;
}

static bool recorder_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/recorder") && strncmp(r->path, "/api/recorder/", 14)) return false;
    if (tools_is(r, "GET", "/api/recorder")) web_resp_json(r, 200, rec_json());
    else if (tools_is(r, "POST", "/api/recorder/start")) start_route(r);
    else if (tools_is(r, "POST", "/api/recorder/stop")) stop_route(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const RECORDER_CAPS[] = { "recorder", NULL };
const web_tool_t web_tool_recorder = { RECORDER_CAPS, recorder_handle, NULL, recorder_second, recorder_stop };

/* ---------------------------------------------------------------- player */

static std::mutex s_play_mx;
static std::string s_play_path;
static bool s_playing = false;
static int s_volume = -1;               /* last known, read on the UI thread */

static std::string audio_json(void)
{
    std::lock_guard<std::mutex> lk(s_play_mx);
    std::string j = std::string("{\"playing\":") + (s_playing ? "true" : "false") + ",\"path\":";
    if (s_play_path.empty()) j += "null";
    else web_json_str(j, s_play_path.c_str());
    return j + ",\"volume\":" + tools_int(s_volume < 0 ? 0 : s_volume) + "}";
}

static void set_playing(bool on, const std::string *path = NULL)
{
    {
        std::lock_guard<std::mutex> lk(s_play_mx);
        s_playing = on;
        if (path) s_play_path = *path;
    }
    web_events_publish("audio", audio_json().c_str());
}

static void play_route(web_req_t *r)
{
    std::string body, path, host, root;
    if (!tools_read_json(r, body)) return;
    if (!web_json_get_str(body, "path", path)) {
        web_resp_error(r, 400, "bad_request", "path");
        return;
    }
    if (!resolve(r, path, host, &root)) return;
    struct stat st;
    web_plat_io_lock();
    bool exists = stat(host.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    web_plat_io_unlock();
    if (!exists) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    const char *dot = strrchr(path.c_str(), '.');
    if (!dot || (strcasecmp(dot, ".wav") && strcasecmp(dot, ".mp3"))) {
        web_resp_error(r, 400, "bad_request", "WAV or MP3 only");
        return;
    }
    if (tools_busy(r, APPS)) return;
    std::string fs_path = path.substr(1 + root.size());     /* "/sd/data/x.wav" -> "/data/x.wav" */
    bool ok = false;
    web_ui_run([&] { ok = web_hw_audio_play(root.c_str(), fs_path.c_str(), host.c_str()); });
    if (!ok) {
        web_resp_error(r, 500, "internal", "the player did not start");
        return;
    }
    set_playing(true, &path);
    web_resp_ok(r);
}

static void patch_route(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;
    double v;
    if (web_json_has(body, "volume")) {
        if (!web_json_get_num(body, "volume", &v) || v < 0 || v > 100) {
            web_resp_error(r, 400, "bad_request", "volume 0-100");
            return;
        }
        web_ui_run([&] { web_hw_audio_set_volume((int)v); s_volume = web_hw_audio_volume(); });
        web_events_publish("audio", audio_json().c_str());
    }
    web_resp_json(r, 200, audio_json());
}

static void audio_second(uint32_t now)
{
    (void)now;
    s_volume = web_hw_audio_volume();
    bool was;
    {
        std::lock_guard<std::mutex> lk(s_play_mx);
        was = s_playing;
    }
    if (!was) return;
    if (tools_holder(APPS)) set_playing(false);         /* a watch app took the speaker */
    else if (!web_hw_audio_playing()) set_playing(false);
}

static void audio_stop(void)
{
    bool was;
    {
        std::lock_guard<std::mutex> lk(s_play_mx);
        was = s_playing;
        s_playing = false;
    }
    if (was) web_hw_audio_stop();
}

static bool audio_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/audio") && strncmp(r->path, "/api/audio/", 11)) return false;
    if (tools_is(r, "GET", "/api/audio")) {
        web_ui_run([] { s_volume = web_hw_audio_volume(); });
        web_resp_json(r, 200, audio_json());
    } else if (tools_is(r, "PATCH", "/api/audio")) patch_route(r);
    else if (tools_is(r, "POST", "/api/audio/play")) play_route(r);
    else if (tools_is(r, "POST", "/api/audio/stop")) {
        web_ui_run([] { web_hw_audio_stop(); });
        set_playing(false);
        web_resp_ok(r);
    } else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const AUDIO_CAPS[] = { "audio.play", NULL };
const web_tool_t web_tool_audio = { AUDIO_CAPS, audio_handle, NULL, audio_second, audio_stop };

#endif /* FACTORY_WEB */
