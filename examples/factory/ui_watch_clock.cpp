/**
 * @file      ui_watch_clock.cpp
 * @license   MIT
 * @date      2026-11-04
 *
 * Stopwatch, Timer and Alarm: three apps on the watch, in place of Clock Tools.
 * Setting the date, time and zone lives in Settings > Date & time
 * (ui_clock_settings_build()).
 *
 * The engine keeps running when the app is closed. A finished timer or a due
 * alarm opens a full-screen ring page over whatever is showing and vibrates
 * until Stop (or Snooze) is tapped. When the screen is off, light sleep wakes
 * at the next due time (watch_clock_next_wake_ms()). Alarms don't wake the
 * watch from deep sleep or power off.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

#if !defined(EXCLUDE_CLOCK_TOOLS)

LV_FONT_DECLARE(font_alibaba_24);
LV_FONT_DECLARE(font_alibaba_100);
LV_IMAGE_DECLARE(img_watch_reset);
LV_IMAGE_DECLARE(img_watch_lap);

#define CLOCK_ENGINE_PERIOD_MS  250
#define RING_BUZZ_PERIOD_MS     1200
#define RING_TIMEOUT_MS         60000   /* Stops by itself after a minute */
#define SNOOZE_MINUTES          9
#define SNOOZE_TEXT             "Snooze 9 min"
#define WAKE_MARGIN_MS          500     /* Wake just after the due time, not before */
#define STOPWATCH_LAP_MAX       30
#define STOPWATCH_REFRESH_MS    50
#define TIMER_REFRESH_MS        200
#define ROUND_BTN               96
#define PRESET_BTN              112
#define PRESET_GAP              28
#define TIMER_ARC_SIZE          220
#define ALARM_ROW_H             96
#define COLOR_DANGER            lv_color_hex(0xE5484D)

/* ── Engine state: kept while the app is closed ── */

static struct {
    bool running;
    uint32_t start_tick;    /* lv_tick when started or resumed */
    uint32_t banked_ms;     /* Time counted before the last start */
    uint8_t lap_count;
    uint32_t laps[STOPWATCH_LAP_MAX];   /* Total time at each lap */
} sw;

typedef enum { TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED } timer_state_t;

static struct {
    timer_state_t state;
    uint32_t duration_ms;   /* Length it was started with */
    uint32_t end_tick;      /* TIMER_RUNNING: lv_tick when it ends */
    uint32_t left_ms;       /* TIMER_PAUSED: time left */
} tmr;

static struct {
    bool active;
    uint32_t due_tick;
    uint8_t hour, minute;   /* The alarm it came from, for the ring page */
} snooze;

typedef enum { RING_TIMER, RING_ALARM } ring_kind_t;

static struct {
    lv_obj_t *overlay;
    lv_timer_t *buzz;
    uint32_t start_tick;
    ring_kind_t kind;
    uint8_t hour, minute;
} ring;

static int32_t last_alarm_minute = -1;  /* Minute of the year last checked */
static lv_timer_t *engine_timer;

/* ── Digits that don't jitter ──
 * The fonts have proportional digits, so a centred "00:01" and "00:11" differ in
 * width and the whole line shifts as it counts. Each digit gets its own cell as
 * wide as the widest digit. */

#define DIGIT_CELLS 8       /* h h : m m : s s */

typedef struct {
    lv_obj_t *box;
    lv_obj_t *cell[DIGIT_CELLS];
} digits_t;

static int32_t widest_digit(const lv_font_t *font)
{
    int32_t w = 0;
    for (uint32_t c = '0'; c <= '9'; ++c) {
        w = LV_MAX(w, (int32_t)lv_font_get_glyph_width(font, c, 0));
    }
    return w;
}

static lv_obj_t *digit_cell(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            int32_t width, const char *text)
{
    lv_obj_t *cell = lv_label_create(parent);
    lv_label_set_text(cell, text);
    lv_obj_set_style_text_font(cell, font, 0);
    lv_obj_set_style_text_color(cell, color, 0);
    if (width) {
        lv_obj_set_width(cell, width);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
    }
    return cell;
}

static void digits_create(digits_t *d, lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    d->box = lv_obj_create(parent);
    lv_obj_remove_style_all(d->box);
    lv_obj_set_size(d->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(d->box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d->box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(d->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d->box, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = widest_digit(font);
    for (int i = 0; i < DIGIT_CELLS; ++i) {
        bool colon = i == 2 || i == 5;
        d->cell[i] = digit_cell(d->box, font, color, colon ? 0 : w, colon ? ":" : "0");
    }
}

/* "MM:SS", or "H:MM:SS" from an hour on. */
static void digits_set(digits_t *d, uint32_t seconds)
{
    if (!d->box) return;
    uint32_t v[3] = {seconds / 3600, (seconds / 60) % 60, seconds % 60};
    for (int g = 0; g < 3; ++g) {
        char tens[2] = {(char)('0' + (v[g] / 10) % 10), 0};
        char ones[2] = {(char)('0' + v[g] % 10), 0};
        lv_label_set_text(d->cell[g * 3], tens);
        lv_label_set_text(d->cell[g * 3 + 1], ones);
    }
    bool hours = v[0] > 0;
    lv_obj_set_flag(d->cell[0], LV_OBJ_FLAG_HIDDEN, v[0] < 10);
    lv_obj_set_flag(d->cell[1], LV_OBJ_FLAG_HIDDEN, !hours);
    lv_obj_set_flag(d->cell[2], LV_OBJ_FLAG_HIDDEN, !hours);
}

/* ── Pages: NULL while not open ── */

static struct {
    digits_t time;          /* "00:00" */
    lv_obj_t *hundredths[2];
    lv_obj_t *left_btn;     /* Lap or Reset */
    lv_obj_t *left_icon;    /* Lap flag or reset arrow */
    lv_obj_t *main_icon;    /* Play or pause */
    lv_obj_t *laps;         /* Card of lap rows */
    uint8_t laps_shown;
    lv_timer_t *refresh;
} sw_ui;

static struct {
    lv_obj_t *presets;      /* Idle view */
    lv_obj_t *running;      /* Countdown view */
    lv_obj_t *arc;
    digits_t left;
    lv_obj_t *main_icon;
    lv_obj_t *recents;      /* Last custom lengths, in the idle view */
    bool idle_shown;
    lv_timer_t *refresh;
} tmr_ui;

static struct {
    lv_obj_t *list;         /* Alarm list page: box the rows are rebuilt in */
    lv_obj_t *add_row;
    lv_obj_t *edit_time;    /* Edit page: the time value label */
    lv_obj_t *edit_repeat;  /* Edit page: the repeat value label */
    int edit_index;
    lv_obj_t *wheel[2];     /* Hour and minute on a time page */
    int wheel_target;       /* Alarm index, or -1 for a new alarm */
} alarm_ui = {NULL, NULL, NULL, NULL, -1, {NULL, NULL}, -1};

static lv_obj_t *custom_wheel[3];   /* Hours, minutes, seconds */


static char hour_options[24 * 3];
static char minute_options[60 * 3];

static void build_wheel_options(void)
{
    if (hour_options[0]) return;
    ui_wheel_number_options(hour_options, sizeof(hour_options), 24, 0);
    ui_wheel_number_options(minute_options, sizeof(minute_options), 60, 0);
}

/* ── Formatting ── */

static void format_hms(char *out, size_t size, uint32_t seconds)
{
    uint32_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    if (h) lv_snprintf(out, size, "%" LV_PRIu32 ":%02" LV_PRIu32 ":%02" LV_PRIu32, h, m, s);
    else lv_snprintf(out, size, "%02" LV_PRIu32 ":%02" LV_PRIu32, m, s);
}

static void format_stopwatch(char *out, size_t size, uint32_t ms)
{
    char hms[16];
    format_hms(hms, sizeof(hms), ms / 1000);
    lv_snprintf(out, size, "%s.%02" LV_PRIu32, hms, (ms / 10) % 100);
}

/* Repeat presets; any other set of days is shown by name. Bit 0 = Sunday. */
#define REPEAT_PRESETS  4
static const char *repeat_preset_names[REPEAT_PRESETS] = {"Once", "Every day", "Weekdays", "Weekends"};
static const uint8_t repeat_masks[REPEAT_PRESETS] = {0x00, 0x7F, 0x3E, 0x41};
/* Weeks start on Monday on the watch. */
static const uint8_t week_order[7] = {1, 2, 3, 4, 5, 6, 0};
static const char *day_long[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                  "Friday", "Saturday"};
static const char *day_short[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *day_tiny[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};

/* "Weekdays", or "Mon Wed Fri", or "Mo Tu We Th Sa" when there are more. */
static void repeat_name(char *out, size_t size, uint8_t days)
{
    for (uint8_t i = 0; i < REPEAT_PRESETS; ++i) {
        if (repeat_masks[i] == days) {
            lv_snprintf(out, size, "%s", repeat_preset_names[i]);
            return;
        }
    }
    int count = 0;
    for (int d = 0; d < 7; ++d) count += (days >> d) & 1;
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < 7; ++i) {
        int d = week_order[i];
        if (!(days & (1 << d)) || len + 5 >= size) continue;
        len += (size_t)lv_snprintf(out + len, size - len, "%s%s", len ? " " : "",
                                   count > 3 ? day_tiny[d] : day_short[d]);
    }
}

/* ── Stopwatch engine ── */

static uint32_t stopwatch_ms(void)
{
    return sw.banked_ms + (sw.running ? lv_tick_elaps(sw.start_tick) : 0);
}

static void stopwatch_toggle(void)
{
    if (sw.running) {
        sw.banked_ms = stopwatch_ms();
        sw.running = false;
    } else {
        sw.start_tick = lv_tick_get();
        sw.running = true;
    }
}

static void stopwatch_reset(void)
{
    sw.running = false;
    sw.banked_ms = 0;
    sw.lap_count = 0;
}

/* ── Timer engine ── */

static uint32_t timer_left_ms(void)
{
    if (tmr.state == TIMER_PAUSED) return tmr.left_ms;
    if (tmr.state != TIMER_RUNNING) return 0;
    int32_t left = (int32_t)(tmr.end_tick - lv_tick_get());
    return left > 0 ? (uint32_t)left : 0;
}

static void timer_start(uint32_t duration_ms)
{
    tmr.duration_ms = duration_ms;
    tmr.end_tick = lv_tick_get() + duration_ms;
    tmr.state = TIMER_RUNNING;
}

static void timer_toggle_pause(void)
{
    if (tmr.state == TIMER_RUNNING) {
        tmr.left_ms = timer_left_ms();
        tmr.state = TIMER_PAUSED;
    } else if (tmr.state == TIMER_PAUSED) {
        tmr.end_tick = lv_tick_get() + tmr.left_ms;
        tmr.state = TIMER_RUNNING;
    }
}

/* ── Alarm schedule ── */

/* Seconds from `now` until the alarm next rings, or -1 if it is off. The
 * current minute counts as passed: the engine only rings on a minute change. */
static int32_t alarm_seconds_until(const watch_alarm_t &a, const struct tm &now)
{
    if (!a.used || !a.enabled) return -1;
    int32_t now_sec = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;
    int32_t at_sec = a.hour * 3600 + a.minute * 60;
    for (int d = 0; d <= 7; ++d) {
        if (d == 0 && at_sec <= now.tm_hour * 3600 + now.tm_min * 60) continue;
        int wday = (now.tm_wday + d) % 7;
        if (a.days == 0 || (a.days & (1 << wday))) {
            return d * 86400 + at_sec - now_sec;
        }
    }
    return -1;
}

static int32_t next_alarm_seconds(int *index)
{
    struct tm now;
    hw_get_date_time(now);
    int32_t best = -1;
    for (uint8_t i = 0; i < WATCH_ALARM_COUNT; ++i) {
        int32_t s = alarm_seconds_until(hw_get_watch_alarm(i), now);
        if (s >= 0 && (best < 0 || s < best)) {
            best = s;
            if (index) *index = i;
        }
    }
    return best;
}

static void toast_rings_in(int32_t seconds)
{
    if (seconds < 0) return;
    int32_t minutes = (seconds + 59) / 60;
    char buf[40];
    if (minutes >= 60) {
        lv_snprintf(buf, sizeof(buf), "Rings in %" LV_PRId32 " h %" LV_PRId32 " min",
                    minutes / 60, minutes % 60);
    } else {
        lv_snprintf(buf, sizeof(buf), "Rings in %" LV_PRId32 " min", minutes);
    }
    ui_toast(buf);
}

uint32_t watch_clock_next_wake_ms(void)
{
    uint32_t best = 0;
    if (tmr.state == TIMER_RUNNING) {
        best = timer_left_ms() + 1;
    }
    if (snooze.active) {
        int32_t left = (int32_t)(snooze.due_tick - lv_tick_get());
        uint32_t ms = left > 0 ? (uint32_t)left + 1 : 1;
        if (!best || ms < best) best = ms;
    }
    int32_t alarm_s = next_alarm_seconds(NULL);
    if (alarm_s >= 0) {
        uint32_t ms = (uint32_t)alarm_s * 1000U + WAKE_MARGIN_MS;
        if (!best || ms < best) best = ms;
    }
    return best;
}

/* ── Ring page ── */

static lv_obj_t *plain_box(lv_obj_t *parent, lv_flex_flow_t flow);

static void ring_close(void)
{
    if (ring.buzz) {
        lv_timer_delete(ring.buzz);
        ring.buzz = NULL;
    }
    if (ring.overlay) {
        lv_obj_delete(ring.overlay);
        ring.overlay = NULL;
    }
}

static void ring_stop_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ring_close();
}

static void ring_snooze_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    snooze.active = true;
    snooze.due_tick = lv_tick_get() + SNOOZE_MINUTES * 60000U;
    snooze.hour = ring.hour;
    snooze.minute = ring.minute;
    ring_close();
    ui_toast("Snoozed for 9 min");
}

static void ring_buzz_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (lv_tick_elaps(ring.start_tick) > RING_TIMEOUT_MS) {
        ring_close();
        return;
    }
    hw_haptic(HW_HAPTIC_ALERT);
    lv_display_trigger_activity(NULL);  /* Keep the screen on while ringing */
}

static lv_obj_t *ring_pill(lv_obj_t *box, const char *text, lv_event_cb_t cb, bool primary)
{
    lv_obj_t *row = ui_create_card_action(box, NULL, text, cb);
    if (!primary) {
        lv_obj_t *btn = lv_obj_get_child(row, -1);
        lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_BG, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_TEXT_PRIMARY, LV_STATE_PRESSED);
    }
    return row;
}

static void ring_open(ring_kind_t kind, uint8_t hour, uint8_t minute)
{
    ring_close();
    ui_styles_init();
    watch_shell_wake();
    ring.kind = kind;
    ring.hour = hour;
    ring.minute = minute;
    ring.start_tick = lv_tick_get();

    lv_obj_t *o = lv_obj_create(lv_layer_top());
    ring.overlay = o;
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(o, UI_SCREEN_MARGIN, 0);
    lv_obj_set_style_pad_top(o, 28, 0);
    lv_obj_set_style_pad_bottom(o, 24, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);     /* Taps don't reach the page below */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = plain_box(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_t *icon = lv_label_create(head);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_add_style(icon, &ui_styles.accent_text, 0);
    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, kind == RING_ALARM ? "Alarm" : "Timer done");
    lv_obj_set_style_text_font(title, &font_alibaba_24, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_SECONDARY, 0);

    lv_obj_t *big = lv_label_create(o);
    char buf[16];
    if (kind == RING_ALARM) {
        lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    } else {
        format_hms(buf, sizeof(buf), tmr.duration_ms / 1000);
    }
    lv_label_set_text(big, buf);
    lv_obj_set_style_text_font(big, &font_alibaba_100, 0);
    lv_obj_set_style_text_color(big, UI_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *box = lv_obj_create(o);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (kind == RING_ALARM) {
        ring_pill(box, SNOOZE_TEXT, ring_snooze_cb, false);
    }
    ring_pill(box, "Stop", ring_stop_cb, true);

    ring.buzz = lv_timer_create(ring_buzz_cb, RING_BUZZ_PERIOD_MS, NULL);
    ring_buzz_cb(ring.buzz);
}

/* ── Engine tick ── */

static void tmr_ui_show(void);

static void engine_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (tmr.state == TIMER_RUNNING && timer_left_ms() == 0) {
        tmr.state = TIMER_IDLE;
        tmr_ui_show();
        ring_open(RING_TIMER, 0, 0);
    }
    if (snooze.active && (int32_t)(snooze.due_tick - lv_tick_get()) <= 0) {
        snooze.active = false;
        ring_open(RING_ALARM, snooze.hour, snooze.minute);
    }

    struct tm now;
    hw_get_date_time(now);
    int32_t minute = now.tm_yday * 1440 + now.tm_hour * 60 + now.tm_min;
    if (minute == last_alarm_minute) return;
    /* Setting the clock back must not ring an alarm twice; forward still rings. */
    bool first = last_alarm_minute < 0;
    last_alarm_minute = minute;
    if (first) return;
    for (uint8_t i = 0; i < WATCH_ALARM_COUNT; ++i) {
        watch_alarm_t a = hw_get_watch_alarm(i);
        if (!a.used || !a.enabled || a.hour != now.tm_hour || a.minute != now.tm_min) continue;
        if (a.days && !(a.days & (1 << now.tm_wday))) continue;
        if (!a.days) {
            a.enabled = false;  /* "Once" turns itself off */
            hw_set_watch_alarm(i, a);
        }
        ring_open(RING_ALARM, a.hour, a.minute);
        break;
    }
}

void watch_clock_init(void)
{
    if (engine_timer) return;
    engine_timer = lv_timer_create(engine_cb, CLOCK_ENGINE_PERIOD_MS, NULL);
}

/* ── Shared page parts ── */

static lv_obj_t *plain_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

/* A round button: accent for the main action, card grey for the other. */
static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, const char *text,
                              const lv_font_t *font, bool primary, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_add_style(btn, primary ? &ui_styles.accent_btn : &ui_styles.card, 0);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_ACCENT_DIM : UI_COLOR_CARD_FOCUS,
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_group_remove_obj(btn);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, primary ? lv_color_black() : UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_center(label);
    return btn;
}

/* ── Stopwatch page ── */

/* A tinted A8 icon from img_watch_icons.c, centred in its parent. */
static lv_obj_t *icon_image(lv_obj_t *parent, const lv_image_dsc_t *src, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(img, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(img);
    return img;
}

static void sw_ui_update(void)
{
    if (!sw_ui.time.box) return;
    uint32_t ms = stopwatch_ms();
    digits_set(&sw_ui.time, ms / 1000);
    uint32_t cs = (ms / 10) % 100;
    char tens[2] = {(char)('0' + cs / 10), 0};
    char ones[2] = {(char)('0' + cs % 10), 0};
    lv_label_set_text(sw_ui.hundredths[0], tens);
    lv_label_set_text(sw_ui.hundredths[1], ones);
    lv_label_set_text(sw_ui.main_icon, sw.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_image_set_src(sw_ui.left_icon, sw.running || ms == 0 ? &img_watch_lap : &img_watch_reset);
    /* Nothing to lap or reset at zero; the icon dims with its button. */
    lv_obj_set_state(sw_ui.left_btn, LV_STATE_DISABLED, !(sw.running || ms));
    lv_obj_set_state(sw_ui.left_icon, LV_STATE_DISABLED, !(sw.running || ms));

    if (sw_ui.laps_shown != sw.lap_count) {
        sw_ui.laps_shown = sw.lap_count;
        lv_obj_clean(sw_ui.laps);
        if (sw.lap_count) lv_obj_remove_flag(sw_ui.laps, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sw_ui.laps, LV_OBJ_FLAG_HIDDEN);
        for (int i = sw.lap_count - 1; i >= 0; --i) {   /* Newest first */
            uint32_t split = sw.laps[i] - (i ? sw.laps[i - 1] : 0);
            char title[12], value[20];
            lv_snprintf(title, sizeof(title), "Lap %d", i + 1);
            format_stopwatch(value, sizeof(value), split);
            ui_create_card_info(sw_ui.laps, NULL, title, value);
        }
    }
}

static void sw_refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    sw_ui_update();
}

static void sw_main_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    stopwatch_toggle();
    hw_haptic(sw.running ? HW_HAPTIC_START : HW_HAPTIC_STOP);
    sw_ui_update();
}

static void sw_left_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (sw.running) {
        if (sw.lap_count < STOPWATCH_LAP_MAX) {
            sw.laps[sw.lap_count++] = stopwatch_ms();
        } else {
            ui_toast("Lap list is full");
        }
    } else {
        stopwatch_reset();
    }
    hw_feedback();
    sw_ui_update();
}

static void sw_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (sw_ui.refresh) lv_timer_delete(sw_ui.refresh);
    memset(&sw_ui, 0, sizeof(sw_ui));
}

static void stopwatch_build(lv_obj_t *content)
{
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 20, 0);
    lv_obj_add_event_cb(content, sw_page_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *digits = plain_box(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(digits, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_top(digits, 12, 0);
    digits_create(&sw_ui.time, digits, &font_alibaba_100, UI_COLOR_TEXT_PRIMARY);
    lv_obj_t *small = lv_obj_create(digits);
    lv_obj_remove_style_all(small);
    lv_obj_set_size(small, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(small, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_bottom(small, 6, 0);
    lv_obj_remove_flag(small, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *small_font = &lv_font_montserrat_32;
    int32_t small_w = widest_digit(small_font);
    digit_cell(small, small_font, UI_COLOR_TEXT_SECONDARY, 0, ".");
    sw_ui.hundredths[0] = digit_cell(small, small_font, UI_COLOR_TEXT_SECONDARY, small_w, "0");
    sw_ui.hundredths[1] = digit_cell(small, small_font, UI_COLOR_TEXT_SECONDARY, small_w, "0");

    lv_obj_t *buttons = plain_box(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 48, 0);
    sw_ui.left_btn = round_button(buttons, ROUND_BTN, "", &lv_font_montserrat_24, false, sw_left_cb);
    sw_ui.left_icon = icon_image(sw_ui.left_btn, &img_watch_lap, UI_COLOR_TEXT_PRIMARY);
    lv_obj_t *main_btn = round_button(buttons, ROUND_BTN, LV_SYMBOL_PLAY, &lv_font_montserrat_32,
                                      true, sw_main_cb);
    sw_ui.main_icon = lv_obj_get_child(main_btn, 0);

    sw_ui.laps = ui_create_card(content, NULL);
    sw_ui.laps_shown = 0xFF;    /* Build on the first update */

    sw_ui_update();
    sw_ui.refresh = lv_timer_create(sw_refresh_cb, STOPWATCH_REFRESH_MS, NULL);
}

/* ── Timer page ── */

static const uint16_t timer_presets_min[] = {1, 3, 5, 10, 15, 30};

static bool is_timer_preset(uint32_t seconds)
{
    for (uint16_t minutes : timer_presets_min) {
        if (seconds == minutes * 60U) return true;
    }
    return false;
}

static void tmr_custom_start(uint32_t seconds)
{
    if (!is_timer_preset(seconds)) hw_add_watch_timer_recent(seconds);
    timer_start(seconds * 1000U);
    hw_haptic(HW_HAPTIC_START);
}

static void tmr_recent_cb(lv_event_t *e)
{
    tmr_custom_start((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    tmr_ui_show();
}

/* Grey pills for the last custom lengths, above the Custom pill. */
/* Text for a round button, like the presets' "5\nmin": "5 min\n2 s", "1 h\n30 min",
 * or "1:05:10" when all three units are there. */
static void format_duration_round(char *out, size_t size, uint32_t seconds)
{
    uint32_t v[3] = {seconds / 3600, (seconds / 60) % 60, seconds % 60};
    static const char *units[3] = {"h", "min", "s"};
    int parts = (v[0] != 0) + (v[1] != 0) + (v[2] != 0);
    if (parts == 3) {
        lv_snprintf(out, size, "%" LV_PRIu32 ":%02" LV_PRIu32 ":%02" LV_PRIu32, v[0], v[1], v[2]);
        return;
    }
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < 3; ++i) {
        if (!v[i]) continue;
        /* One unit: number over unit, as on the presets. Two: one unit per line. */
        len += (size_t)lv_snprintf(out + len, size - len, parts == 1 ? "%s%" LV_PRIu32 "\n%s" : "%s%" LV_PRIu32 " %s",
                                   len ? "\n" : "", v[i], units[i]);
    }
}

/* Round buttons in three columns, the same for presets and recent lengths. */
static lv_obj_t *round_grid(lv_obj_t *parent)
{
    lv_obj_t *grid = plain_box(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(grid, 3 * PRESET_BTN + 2 * PRESET_GAP);
    lv_obj_set_style_pad_column(grid, PRESET_GAP, 0);
    lv_obj_set_style_pad_row(grid, 16, 0);
    return grid;
}

/* The last custom lengths, under the presets and above the Custom pill. */
static void tmr_recents_build(void)
{
    lv_obj_clean(tmr_ui.recents);
    bool any = false;
    for (uint8_t i = 0; i < WATCH_TIMER_RECENT_COUNT; ++i) {
        uint32_t seconds = hw_get_watch_timer_recent(i);
        if (!seconds) continue;
        if (!any) {
            lv_obj_t *caption = lv_label_create(tmr_ui.recents);
            lv_label_set_text(caption, "Recent");
            lv_obj_set_width(caption, LV_PCT(100));
            lv_obj_set_style_text_font(caption, UI_FONT_CAPTION, 0);
            lv_obj_set_style_text_color(caption, UI_COLOR_TEXT_SECONDARY, 0);
            any = true;
        }
        char text[32];
        format_duration_round(text, sizeof(text), seconds);
        lv_obj_t *btn = round_button(tmr_ui.recents, PRESET_BTN, text, &lv_font_montserrat_24, false, NULL);
        lv_obj_add_event_cb(btn, tmr_recent_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)seconds);
    }
    lv_obj_set_flag(tmr_ui.recents, LV_OBJ_FLAG_HIDDEN, !any);
}

static void tmr_ui_show(void)
{
    if (!tmr_ui.presets) return;
    bool idle = tmr.state == TIMER_IDLE;
    if (idle) {
        if (!tmr_ui.idle_shown) {
            tmr_ui.idle_shown = true;
            tmr_recents_build();    /* A custom start may have changed the list */
            lv_obj_remove_flag(tmr_ui.presets, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tmr_ui.running, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_y(lv_obj_get_parent(tmr_ui.presets), 0, LV_ANIM_OFF);
        }
        return;
    }
    tmr_ui.idle_shown = false;
    lv_obj_add_flag(tmr_ui.presets, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(tmr_ui.running, LV_OBJ_FLAG_HIDDEN);

    uint32_t left = timer_left_ms();
    digits_set(&tmr_ui.left, (left + 999) / 1000);
    int32_t permille = tmr.duration_ms ? (int32_t)((uint64_t)left * 1000 / tmr.duration_ms) : 0;
    lv_arc_set_value(tmr_ui.arc, permille);
    lv_label_set_text(tmr_ui.main_icon, tmr.state == TIMER_RUNNING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void tmr_refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    tmr_ui_show();
}

static void tmr_preset_cb(lv_event_t *e)
{
    uint32_t minutes = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    timer_start(minutes * 60000U);
    hw_haptic(HW_HAPTIC_START);
    tmr_ui_show();
}

static void tmr_pause_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    timer_toggle_pause();
    hw_haptic(tmr.state == TIMER_RUNNING ? HW_HAPTIC_START : HW_HAPTIC_STOP);
    tmr_ui_show();
}

static void tmr_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    tmr.state = TIMER_IDLE;
    hw_haptic(HW_HAPTIC_STOP);
    tmr_ui_show();
}

static void custom_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    custom_wheel[0] = custom_wheel[1] = custom_wheel[2] = NULL;
}

static void custom_start_cb(lv_event_t *e)
{
    if (!custom_wheel[0]) return;
    uint32_t seconds = lv_roller_get_selected(custom_wheel[0]) * 3600U +
                       lv_roller_get_selected(custom_wheel[1]) * 60U +
                       lv_roller_get_selected(custom_wheel[2]);
    if (seconds == 0) {
        ui_toast("Pick a time first");
        return;
    }
    tmr_custom_start(seconds);
    tmr_ui_show();
    ui_pop_page(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target_obj(e))));
}

static lv_obj_t *wheel_colon(lv_obj_t *row)
{
    lv_obj_t *colon = lv_label_create(row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(colon, UI_COLOR_TEXT_PRIMARY, 0);
    return colon;
}

static void tmr_custom_cb(lv_event_t *e)
{
    build_wheel_options();
    lv_obj_t *row;
    lv_obj_t *content = ui_push_wheel_page(lv_event_get_current_target_obj(e), "Custom timer", &row);
    lv_obj_add_event_cb(content, custom_delete_cb, LV_EVENT_DELETE, NULL);
    uint32_t last = hw_get_watch_timer_recent(0);
    if (!last) last = 5 * 60;
    custom_wheel[0] = ui_create_wheel(row, hour_options, (last / 3600) % 24, 110, true);
    wheel_colon(row);
    custom_wheel[1] = ui_create_wheel(row, minute_options, (last / 60) % 60, 110, true);
    wheel_colon(row);
    custom_wheel[2] = ui_create_wheel(row, minute_options, last % 60, 110, true);
    ui_create_card_icon_action(content, LV_SYMBOL_PLAY, custom_start_cb);
}

static void tmr_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (tmr_ui.refresh) lv_timer_delete(tmr_ui.refresh);
    memset(&tmr_ui, 0, sizeof(tmr_ui));
}

static void timer_build(lv_obj_t *content)
{
    lv_obj_add_event_cb(content, tmr_page_delete_cb, LV_EVENT_DELETE, NULL);

    /* Idle: round presets, three to a row, and a Custom pill. */
    tmr_ui.presets = plain_box(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *grid = round_grid(tmr_ui.presets);
    lv_obj_set_style_pad_top(grid, 8, 0);
    for (uint16_t minutes : timer_presets_min) {
        char text[16];
        lv_snprintf(text, sizeof(text), "%u\nmin", (unsigned)minutes);
        lv_obj_t *btn = round_button(grid, PRESET_BTN, text, &lv_font_montserrat_24, false, NULL);
        lv_obj_add_event_cb(btn, tmr_preset_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)minutes);
    }
    tmr_ui.recents = round_grid(tmr_ui.presets);
    lv_obj_set_style_pad_top(tmr_ui.recents, 16, 0);
    lv_obj_set_style_pad_row(tmr_ui.recents, 8, 0);
    lv_obj_t *custom_pill = lv_obj_create(tmr_ui.presets);
    lv_obj_remove_style_all(custom_pill);
    lv_obj_set_size(custom_pill, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(custom_pill, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(custom_pill, 16, 0);
    lv_obj_remove_flag(custom_pill, LV_OBJ_FLAG_SCROLLABLE);
    ui_create_card_action(custom_pill, LV_SYMBOL_EDIT, "Custom", tmr_custom_cb);

    /* Running or paused: a ring that empties, the time left, pause and cancel. */
    tmr_ui.running = plain_box(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tmr_ui.running, 12, 0);
    tmr_ui.arc = lv_arc_create(tmr_ui.running);
    lv_obj_set_size(tmr_ui.arc, TIMER_ARC_SIZE, TIMER_ARC_SIZE);
    lv_arc_set_rotation(tmr_ui.arc, 270);
    lv_arc_set_bg_angles(tmr_ui.arc, 0, 360);
    lv_arc_set_range(tmr_ui.arc, 0, 1000);
    lv_arc_set_mode(tmr_ui.arc, LV_ARC_MODE_REVERSE);
    lv_obj_remove_style(tmr_ui.arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(tmr_ui.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(tmr_ui.arc, 12, 0);
    lv_obj_set_style_arc_width(tmr_ui.arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(tmr_ui.arc, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_arc_color(tmr_ui.arc, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    digits_create(&tmr_ui.left, tmr_ui.arc, &lv_font_montserrat_48, UI_COLOR_TEXT_PRIMARY);
    lv_obj_center(tmr_ui.left.box);

    lv_obj_t *buttons = plain_box(tmr_ui.running, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 48, 0);
    round_button(buttons, ROUND_BTN - 8, LV_SYMBOL_CLOSE, &lv_font_montserrat_32, false, tmr_cancel_cb);
    lv_obj_t *main_btn = round_button(buttons, ROUND_BTN - 8, LV_SYMBOL_PAUSE, &lv_font_montserrat_32,
                                      true, tmr_pause_cb);
    tmr_ui.main_icon = lv_obj_get_child(main_btn, 0);

    tmr_ui_show();
    tmr_ui.refresh = lv_timer_create(tmr_refresh_cb, TIMER_REFRESH_MS, NULL);
}

/* ── Alarm pages ── */

static void alarm_list_refresh(void);

static void alarm_time_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alarm_ui.wheel[0] = alarm_ui.wheel[1] = NULL;
}

static int free_alarm_slot(void)
{
    for (uint8_t i = 0; i < WATCH_ALARM_COUNT; ++i) {
        if (!hw_get_watch_alarm(i).used) return i;
    }
    return -1;
}

static void alarm_time_set_cb(lv_event_t *e)
{
    if (!alarm_ui.wheel[0]) return;
    int index = alarm_ui.wheel_target;
    watch_alarm_t a;
    if (index < 0) {
        index = free_alarm_slot();
        if (index < 0) return;
        a = {true, true, 7, 0, 0};
    } else {
        a = hw_get_watch_alarm(index);
    }
    a.hour = (uint8_t)lv_roller_get_selected(alarm_ui.wheel[0]);
    a.minute = (uint8_t)lv_roller_get_selected(alarm_ui.wheel[1]);
    a.enabled = true;   /* Picking a time means it should ring */
    hw_set_watch_alarm(index, a);

    if (alarm_ui.edit_time) {
        lv_label_set_text_fmt(alarm_ui.edit_time, "%02d:%02d  " LV_SYMBOL_RIGHT, a.hour, a.minute);
    }
    alarm_list_refresh();
    struct tm now;
    hw_get_date_time(now);
    toast_rings_in(alarm_seconds_until(a, now));
    ui_pop_page(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target_obj(e))));
}

static void open_alarm_time_page(lv_obj_t *from, int index)
{
    build_wheel_options();
    watch_alarm_t a;
    if (index >= 0) {
        a = hw_get_watch_alarm(index);
    } else {
        struct tm now;
        hw_get_date_time(now);
        a = {false, true, (uint8_t)now.tm_hour, (uint8_t)now.tm_min, 0};
    }
    alarm_ui.wheel_target = index;
    lv_obj_t *row;
    lv_obj_t *content = ui_push_wheel_page(from, index < 0 ? "New alarm" : "Alarm time", &row);
    lv_obj_add_event_cb(content, alarm_time_delete_cb, LV_EVENT_DELETE, NULL);
    alarm_ui.wheel[0] = ui_create_wheel(row, hour_options, a.hour, 130, true);
    wheel_colon(row);
    alarm_ui.wheel[1] = ui_create_wheel(row, minute_options, a.minute, 130, true);
    ui_create_card_icon_action(content, LV_SYMBOL_OK, alarm_time_set_cb);
}

static void alarm_add_cb(lv_event_t *e)
{
    if (free_alarm_slot() < 0) return;
    open_alarm_time_page(lv_event_get_current_target_obj(e), -1);
}

static void edit_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw_obj = lv_event_get_target_obj(e);
    watch_alarm_t a = hw_get_watch_alarm(alarm_ui.edit_index);
    a.enabled = lv_obj_has_state(sw_obj, LV_STATE_CHECKED);
    hw_set_watch_alarm(alarm_ui.edit_index, a);
    alarm_list_refresh();
    if (a.enabled) {
        struct tm now;
        hw_get_date_time(now);
        toast_rings_in(alarm_seconds_until(a, now));
    }
}

/* Repeat page: the presets as a radio list, then one switch per day. */
static struct {
    lv_obj_t *check[REPEAT_PRESETS];
    lv_obj_t *day_switch[7];    /* By tm wday */
} repeat_ui;

static void edit_repeat_label_update(uint8_t days)
{
    if (!alarm_ui.edit_repeat) return;
    char buf[40];
    repeat_name(buf, sizeof(buf) - 6, days);
    lv_strcat(buf, "  " LV_SYMBOL_RIGHT);
    lv_label_set_text(alarm_ui.edit_repeat, buf);
}

static void repeat_sync(uint8_t days)
{
    for (uint8_t i = 0; i < REPEAT_PRESETS; ++i) {
        if (repeat_ui.check[i]) lv_label_set_text(repeat_ui.check[i], repeat_masks[i] == days ? LV_SYMBOL_OK : "");
    }
    for (int d = 0; d < 7; ++d) {
        if (repeat_ui.day_switch[d]) lv_obj_set_state(repeat_ui.day_switch[d], LV_STATE_CHECKED, days & (1 << d));
    }
}

static void repeat_save(uint8_t days)
{
    watch_alarm_t a = hw_get_watch_alarm(alarm_ui.edit_index);
    a.days = days & 0x7F;
    hw_set_watch_alarm(alarm_ui.edit_index, a);
    repeat_sync(a.days);
    edit_repeat_label_update(a.days);
    alarm_list_refresh();
}

static void repeat_preset_cb(lv_event_t *e)
{
    uint32_t i = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (i < REPEAT_PRESETS) repeat_save(repeat_masks[i]);
}

static void repeat_day_cb(lv_event_t *e)
{
    lv_obj_t *sw_obj = lv_event_get_target_obj(e);
    int d = (int)(intptr_t)lv_obj_get_user_data(sw_obj);
    uint8_t days = hw_get_watch_alarm(alarm_ui.edit_index).days;
    if (lv_obj_has_state(sw_obj, LV_STATE_CHECKED)) days |= (uint8_t)(1 << d);
    else days &= (uint8_t)~(1 << d);
    repeat_save(days);
}

static void repeat_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    memset(&repeat_ui, 0, sizeof(repeat_ui));
}

static lv_obj_t *tappable_info_row(lv_obj_t *card, const char *icon, const char *title,
                                   const char *value, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = ui_create_card_info(card, icon, title, value);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

static void edit_repeat_cb(lv_event_t *e)
{
    uint8_t days = hw_get_watch_alarm(alarm_ui.edit_index).days;
    lv_obj_t *content = ui_push_page(lv_event_get_current_target_obj(e), "Repeat");
    lv_obj_add_event_cb(content, repeat_page_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = ui_create_card(content, NULL);
    for (uint8_t i = 0; i < REPEAT_PRESETS; ++i) {
        lv_obj_t *row = tappable_info_row(card, NULL, repeat_preset_names[i], "",
                                          repeat_preset_cb, (void *)(uintptr_t)i);
        repeat_ui.check[i] = lv_obj_get_child(row, -1);
        lv_obj_add_style(repeat_ui.check[i], &ui_styles.accent_text, 0);
    }

    card = ui_create_card(content, "Days");
    for (int i = 0; i < 7; ++i) {
        int d = week_order[i];
        lv_obj_t *row = ui_create_card_switch(card, NULL, day_long[d], days & (1 << d), repeat_day_cb);
        repeat_ui.day_switch[d] = lv_obj_get_child(row, -1);
        lv_obj_set_user_data(repeat_ui.day_switch[d], (void *)(intptr_t)d);
    }
    repeat_sync(days);
}

static void edit_time_cb(lv_event_t *e)
{
    open_alarm_time_page(lv_event_get_current_target_obj(e), alarm_ui.edit_index);
}

static void edit_delete_cb(lv_event_t *e)
{
    watch_alarm_t empty = {false, false, 0, 0, 0};
    hw_set_watch_alarm(alarm_ui.edit_index, empty);
    alarm_list_refresh();
    ui_toast("Alarm deleted");
    ui_pop_page(lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target_obj(e))));
}

static void edit_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alarm_ui.edit_time = NULL;
    alarm_ui.edit_repeat = NULL;
}

static void open_alarm_edit_cb(lv_event_t *e)
{
    alarm_ui.edit_index = (int)(intptr_t)lv_event_get_user_data(e);
    watch_alarm_t a = hw_get_watch_alarm(alarm_ui.edit_index);
    char title[12];
    lv_snprintf(title, sizeof(title), "%02d:%02d", a.hour, a.minute);
    lv_obj_t *content = ui_push_page(lv_event_get_current_target_obj(e), "Alarm");
    lv_obj_add_event_cb(content, edit_page_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = ui_create_card(content, NULL);
    ui_create_card_switch(card, LV_SYMBOL_BELL, "Alarm", a.enabled, edit_switch_cb);
    lv_obj_t *row = ui_create_card_info(card, LV_SYMBOL_EDIT, "Time", title);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_event_cb(row, edit_time_cb, LV_EVENT_CLICKED, NULL);
    alarm_ui.edit_time = lv_obj_get_child(row, -1);
    lv_obj_set_style_text_color(alarm_ui.edit_time, UI_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text_fmt(alarm_ui.edit_time, "%02d:%02d  " LV_SYMBOL_RIGHT, a.hour, a.minute);
    lv_obj_t *repeat_row = tappable_info_row(card, LV_SYMBOL_LOOP, "Repeat", "", edit_repeat_cb, NULL);
    alarm_ui.edit_repeat = lv_obj_get_child(repeat_row, -1);
    lv_obj_set_style_text_color(alarm_ui.edit_repeat, UI_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_long_mode(alarm_ui.edit_repeat, LV_LABEL_LONG_DOT);
    edit_repeat_label_update(a.days);

    /* Directly in content: edit_delete_cb finds the page from the pill's row. */
    lv_obj_t *del_row = ui_create_card_icon_action(content, LV_SYMBOL_TRASH, edit_delete_cb);
    lv_obj_t *del = lv_obj_get_child(del_row, -1);
    lv_obj_set_style_bg_color(del, COLOR_DANGER, 0);
    lv_obj_set_style_bg_color(del, lv_color_darken(COLOR_DANGER, LV_OPA_30), LV_STATE_PRESSED);
    ui_require_hold(del);
}

/* "07:30   Weekdays    On" */
static void alarm_row_create(lv_obj_t *parent, uint8_t index, const watch_alarm_t &a)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_add_style(row, &ui_styles.card, 0);
    lv_obj_set_size(row, LV_PCT(100), ALARM_ROW_H);
    lv_obj_set_style_radius(row, ALARM_ROW_H / 2, 0);
    lv_obj_set_style_pad_hor(row, 32, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, open_alarm_edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *time = lv_label_create(row);
    lv_label_set_text_fmt(time, "%02d:%02d", a.hour, a.minute);
    lv_obj_set_style_text_font(time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time, a.enabled ? UI_COLOR_TEXT_PRIMARY : UI_COLOR_TEXT_SECONDARY, 0);

    lv_obj_t *days = lv_label_create(row);
    char repeat[32];
    repeat_name(repeat, sizeof(repeat), a.days);
    lv_label_set_text(days, repeat);
    lv_obj_set_style_text_font(days, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(days, UI_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_long_mode(days, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(days, 1);

    lv_obj_t *state = lv_label_create(row);
    lv_label_set_text(state, a.enabled ? "On" : "Off");
    lv_obj_set_style_text_font(state, &lv_font_montserrat_20, 0);
    if (a.enabled) lv_obj_add_style(state, &ui_styles.accent_text, 0);
    else lv_obj_set_style_text_color(state, UI_COLOR_TEXT_SECONDARY, 0);
}

static void alarm_list_refresh(void)
{
    if (!alarm_ui.list) return;
    lv_obj_clean(alarm_ui.list);
    int count = 0;
    for (uint8_t i = 0; i < WATCH_ALARM_COUNT; ++i) {
        watch_alarm_t a = hw_get_watch_alarm(i);
        if (!a.used) continue;
        alarm_row_create(alarm_ui.list, i, a);
        ++count;
    }
    if (count == 0) {
        lv_obj_t *empty = lv_label_create(alarm_ui.list);
        lv_label_set_text(empty, "No alarms");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_pad_ver(empty, 48, 0);
    }
    if (count >= WATCH_ALARM_COUNT) lv_obj_add_flag(alarm_ui.add_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(alarm_ui.add_row, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alarm_ui.list = NULL;
    alarm_ui.add_row = NULL;
}

static void alarms_build(lv_obj_t *content)
{
    lv_obj_add_event_cb(content, alarm_page_delete_cb, LV_EVENT_DELETE, NULL);
    alarm_ui.list = plain_box(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(alarm_ui.list, 12, 0);
    alarm_ui.add_row = plain_box(content, LV_FLEX_FLOW_COLUMN);
    ui_create_card_icon_action(alarm_ui.add_row, LV_SYMBOL_PLUS, alarm_add_cb);
    alarm_list_refresh();
}

/* ── Apps: Stopwatch, Timer and Alarm, each its own entry in the app list ── */

static lv_obj_t *clock_app_page = NULL;

static void clock_app_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (clock_app_page) {
        lv_obj_t *page = clock_app_page;
        clock_app_page = NULL;
        ui_destroy_app_page(page);
    }
    menu_show();
}

static void clock_app_open(lv_obj_t *parent, const char *title, void (*build)(lv_obj_t *))
{
    watch_clock_init();
    clock_app_page = ui_create_app_page(parent, title, clock_app_back_cb);
    build(clock_app_page);
}

static void stopwatch_enter(lv_obj_t *parent) { clock_app_open(parent, "Stopwatch", stopwatch_build); }
static void timer_enter(lv_obj_t *parent) { clock_app_open(parent, "Timer", timer_build); }
static void alarm_enter(lv_obj_t *parent) { clock_app_open(parent, "Alarm", alarms_build); }
static void clock_app_exit(lv_obj_t *parent) { LV_UNUSED(parent); }

app_t ui_stopwatch_main = {.setup_func_cb = stopwatch_enter, .exit_func_cb = clock_app_exit, .user_data = nullptr};
app_t ui_timer_main = {.setup_func_cb = timer_enter, .exit_func_cb = clock_app_exit, .user_data = nullptr};
app_t ui_alarm_main = {.setup_func_cb = alarm_enter, .exit_func_cb = clock_app_exit, .user_data = nullptr};

#else /* EXCLUDE_CLOCK_TOOLS */

void watch_clock_init(void) {}
uint32_t watch_clock_next_wake_ms(void) { return 0; }

#endif /* EXCLUDE_CLOCK_TOOLS */

#endif /* FACTORY_WATCH_UI */
