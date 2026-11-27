/**
 * @file      ui_nfc.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include <LilyGoLog.h>
#include "ui_define.h"

#if defined(ARDUINO) && !defined(EXCLUDE_NFC)

#include <SD.h>
#include <time.h>
#include "capture_store.h"

static lv_obj_t *msgbox = NULL;
static lv_obj_t *page_container = NULL;
static lv_obj_t *state_value = NULL;
static lv_obj_t *uid_value = NULL;
static lv_obj_t *tech_value = NULL;
static lv_obj_t *ndef_value = NULL;
static lv_obj_t *record_body = NULL;
static lv_obj_t *raw_value = NULL;
static lv_obj_t *hint_value = NULL;
static bool nfc_page_running = false;

static lv_obj_t *create_value_label(lv_obj_t *card, const char *icon, const char *title, const char *value)
{
    lv_obj_t *label = lv_label_create(lv_obj_create(card));
    lv_label_set_text(label, value ? value : "--");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_width(label, 1);
    ui_create_card_item(card, icon, title, label);
    return label;
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (label) {
        lv_label_set_text(label, text ? text : "--");
    }
}

static bool is_mifare_classic_like(const LilyGoNfcReaderResult &result)
{
    return result.mifareClassic ||
           strcmp(result.tagType, "MIFARE") == 0 ||
           strstr(result.cardType, "MIFARE Classic") != NULL;
}

static bool is_uid_only_result(const LilyGoNfcReaderResult &result)
{
    return strcmp(result.ndefStateText, "UID only") == 0;
}

static const char *card_detected_message(const LilyGoNfcReaderResult &result)
{
    if (is_mifare_classic_like(result)) {
        return "MIFARE Classic card detected.\nID has been read.";
    }
    if (strstr(result.cardType, "Type 4") != NULL) {
        return "NFC Type 4 card/device detected.\nID has been read.";
    }
    if (strstr(result.cardType, "Type 3") != NULL || strstr(result.cardType, "FeliCa") != NULL) {
        return "FeliCa / NFC Type 3 card detected.\nID has been read.";
    }
    if (strstr(result.cardType, "Type 5") != NULL || strstr(result.cardType, "NFC-V") != NULL) {
        return "ISO15693 / NFC Type 5 card detected.\nID has been read.";
    }
    if (strstr(result.cardType, "P2P") != NULL) {
        return "NFC peer-to-peer device detected.\nID has been read.";
    }
    if (strstr(result.cardType, "ST25TB") != NULL) {
        return "ST25TB tag detected.\nID has been read.";
    }
    return "NFC card detected.\nID has been read.";
}

static const char *card_hint_message(const LilyGoNfcReaderResult &result)
{
    if (is_mifare_classic_like(result)) {
        return "MIFARE Classic ID is public. Sector data needs keys/auth.";
    }
    if (strstr(result.cardType, "Type 4") != NULL) {
        return "Type 4 / ISO-DEP card detected. Standard NDEF data was not found.";
    }
    if (strstr(result.cardType, "P2P") != NULL) {
        return "P2P device detected. This reader only shows the detected NFC ID.";
    }
    return "Card type and ID are shown above. Standard NDEF data is optional.";
}

static const char *record_icon(LilyGoNfcRecordKind kind)
{
    switch (kind) {
    case LILYGO_NFC_RECORD_TEXT:
        return LV_SYMBOL_EDIT;
    case LILYGO_NFC_RECORD_URI:
        return LV_SYMBOL_WIFI;
    case LILYGO_NFC_RECORD_WIFI:
        return LV_SYMBOL_WIFI;
    case LILYGO_NFC_RECORD_VCARD:
        return LV_SYMBOL_LIST;
    case LILYGO_NFC_RECORD_DEVICE_INFO:
        return LV_SYMBOL_SETTINGS;
    case LILYGO_NFC_RECORD_AAR:
        return LV_SYMBOL_LIST;
    case LILYGO_NFC_RECORD_EMPTY:
        return LV_SYMBOL_WARNING;
    case LILYGO_NFC_RECORD_MEDIA:
    case LILYGO_NFC_RECORD_UNKNOWN:
    default:
        return LV_SYMBOL_LIST;
    }
}

static void add_record_row(const LilyGoNfcRecord &record)
{
    if (!record_body) return;

    lv_obj_t *row = lv_obj_create(record_body);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_pad_row(row, 3, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char title[48];
    snprintf(title, sizeof(title), "%u  %s  %s", record.index, record_icon(record.kind), record.title);
    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_12, 0);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, record.value[0] ? record.value : "--");
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(value_label, LV_PCT(100));
    lv_obj_set_style_text_color(value_label, UI_COLOR_TEXT_PRIMARY, 0);

    if (record.detail[0]) {
        lv_obj_t *detail_label = lv_label_create(row);
        lv_label_set_text(detail_label, record.detail);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail_label, LV_PCT(100));
        lv_obj_set_style_text_color(detail_label, UI_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_12, 0);
    }
}

static void clear_records(const char *emptyText)
{
    if (!record_body) return;
    lv_obj_clean(record_body);

    lv_obj_t *label = lv_label_create(record_body);
    lv_label_set_text(label, emptyText ? emptyText : "No records");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(label, 6, 0);
    lv_obj_set_style_pad_bottom(label, 6, 0);
}

static void update_record_view(const LilyGoNfcReaderResult &result)
{
    if (!record_body) return;
    lv_obj_clean(record_body);

    if (result.recordCount == 0) {
        clear_records(result.hasNdef ? "NDEF message has no records" : "No NDEF records");
        return;
    }

    for (uint8_t i = 0; i < result.recordCount; ++i) {
        add_record_row(result.records[i]);
    }
}

/* ── Save a read to the SD card ──
 * A read is held in memory and written only when you tap Save, under a name you
 * accept or edit (default "nfc_<n>_<datetime>"). Files land in the /nfc catalog:
 *   /nfc/<name>.txt     human-readable dump
 *   /nfc/<name>.replay  one line the NFC Card app can re-emit (URL/Text/WiFi)
 *   /nfc/_last.replay   pointer to the most recently saved replayable capture
 * The reader and the SD card share the SPI bus, so Save runs from the UI only,
 * after a read has completed — check on hardware it doesn't disturb discovery. */
#define NFC_CATALOG "nfc"

static LilyGoNfcReaderResult captured_result;   /* the read waiting to be saved */
static bool have_capture = false;
static int nfc_counter = 1;
static lv_obj_t *filename_ta = NULL;
static lv_obj_t *save_btn = NULL;
static lv_obj_t *save_status = NULL;

/* Build a one-line, machine-parseable replay record from the first NDEF record
 * that the card emulator can re-emit (URL / Text / Wi-Fi). Returns false when the
 * card has nothing replayable (e.g. a UID-only access card — its UID can't be
 * emulated anyway). Control characters are flattened so the line stays single. */
static bool nfc_build_replay_line(const LilyGoNfcReaderResult &result, char *out, size_t out_len)
{
    out[0] = '\0';
    for (uint8_t i = 0; i < result.recordCount; ++i) {
        const LilyGoNfcRecord &r = result.records[i];
        if (r.kind == LILYGO_NFC_RECORD_URI && r.value[0]) {
            snprintf(out, out_len, "URL|%s", r.value);
        } else if (r.kind == LILYGO_NFC_RECORD_TEXT && r.value[0]) {
            snprintf(out, out_len, "TEXT|%s", r.value);
        } else if (r.kind == LILYGO_NFC_RECORD_WIFI && r.wifiSsid[0]) {
            snprintf(out, out_len, "WIFI|%s|%s", r.wifiSsid, r.wifiPassword);
        } else {
            continue;
        }
        for (char *p = out; *p; ++p) {
            if (*p == '\n' || *p == '\r') *p = ' ';
        }
        return true;
    }
    return false;
}

/* Put a fresh default name in the filename box for the capture just read. */
static void nfc_set_default_name(void)
{
    if (!filename_ta) return;
    char name[48];
    capture_make_name(NFC_CATALOG, nfc_counter, name, sizeof(name));
    lv_textarea_set_text(filename_ta, name);
}

/* Remember the latest read so Save can write it, and prompt the user to save. */
static void nfc_capture_latest(const LilyGoNfcReaderResult &result)
{
    if (!result.uidText[0] || strcmp(result.uidText, "--") == 0) return;
    captured_result = result;
    have_capture = true;
    nfc_set_default_name();
    if (save_btn) lv_obj_clear_state(save_btn, LV_STATE_DISABLED);
    if (save_status) set_label_text(save_status, "Card read — tap Save to keep it.");
}

/* Write the held capture to /nfc/<name>.txt (+ .replay sidecar) when Save is tapped. */
static void nfc_write_capture(const char *name)
{
    if (!have_capture) {
        if (save_status) set_label_text(save_status, "Read a card first.");
        return;
    }
    if (!hw_is_sd_insert() || !capture_catalog_ensure(NFC_CATALOG)) {
        if (save_status) set_label_text(save_status, "Insert an SD card to save.");
        return;
    }
    char safe[48];
    if (!capture_sanitize_name(name, safe, sizeof(safe))) {
        if (save_status) set_label_text(save_status, "Enter a filename.");
        return;
    }

    const LilyGoNfcReaderResult &result = captured_result;
    char path[96];
    capture_path(NFC_CATALOG, safe, ".txt", path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        if (save_status) set_label_text(save_status, "SD write failed.");
        return;
    }

    char line[256];
    time_t now = time(NULL);
    struct tm tmv;
    char ts[32] = "";
    if (now > 1000000000L && localtime_r(&now, &tmv)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    }

    f.println("NFC capture");
    if (ts[0]) { snprintf(line, sizeof(line), "time: %s", ts); f.println(line); }
    snprintf(line, sizeof(line), "uid: %s", result.uidText); f.println(line);
    snprintf(line, sizeof(line), "card: %s",
             result.cardType[0] ? result.cardType : "--"); f.println(line);
    snprintf(line, sizeof(line), "tech: %s / %s / %s",
             result.technology, result.interfaceName, result.tagType); f.println(line);
    if (result.cardInfo[0] && strcmp(result.cardInfo, "--") != 0) {
        snprintf(line, sizeof(line), "info: %s", result.cardInfo); f.println(line);
    }
    if (result.hasNdef) {
        snprintf(line, sizeof(line), "ndef: %s  v%u.%u  %luB",
                 result.ndefStateText, result.ndefMajorVersion, result.ndefMinorVersion,
                 (unsigned long)result.ndefMessageLen);
    } else {
        snprintf(line, sizeof(line), "ndef: %s",
                 result.ndefStateText[0] ? result.ndefStateText : "none");
    }
    f.println(line);
    for (uint8_t i = 0; i < result.recordCount; ++i) {
        const LilyGoNfcRecord &r = result.records[i];
        snprintf(line, sizeof(line), "record %u: %s", r.index, r.title); f.println(line);
        if (r.value[0]) { snprintf(line, sizeof(line), "  value: %s", r.value); f.println(line); }
        if (r.detail[0]) { snprintf(line, sizeof(line), "  detail: %s", r.detail); f.println(line); }
    }
    if (result.rawHexPreview[0] && strcmp(result.rawHexPreview, "--") != 0) {
        snprintf(line, sizeof(line), "raw: %s", result.rawHexPreview); f.println(line);
    }
    f.close();

    /* Replay sidecar the NFC Card (emulation) app can re-emit, plus a _last pointer. */
    char replay[300];
    if (nfc_build_replay_line(result, replay, sizeof(replay))) {
        char rpath[96];
        capture_path(NFC_CATALOG, safe, ".replay", rpath, sizeof(rpath));
        File rf = SD.open(rpath, FILE_WRITE);
        if (rf) { rf.println(replay); rf.close(); }
        File lf = SD.open("/" NFC_CATALOG "/_last.replay", FILE_WRITE);
        if (lf) { lf.println(replay); lf.close(); }
    }

    nfc_counter++;
    snprintf(line, sizeof(line), "Saved %s.txt", safe);
    if (save_status) set_label_text(save_status, line);
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    nfc_write_capture(filename_ta ? lv_textarea_get_text(filename_ta) : "");
}

static void update_result_view(LilyGoNfcEvent event, const LilyGoNfcReaderResult &result)
{
    if (!page_container) return;

    char buf[192];
    snprintf(buf, sizeof(buf), "%s", lilygoNfcEventName(event));
    set_label_text(state_value, buf);

    set_label_text(uid_value, result.uidText);

    if (result.cardInfo[0] && strcmp(result.cardInfo, "--") != 0) {
        snprintf(buf, sizeof(buf), "%s\n%s / %s / %s\n%s",
                 result.cardType[0] ? result.cardType : result.technology,
                 result.technology, result.interfaceName, result.tagType,
                 result.cardInfo);
    } else {
        snprintf(buf, sizeof(buf), "%s / %s / %s",
                 result.technology, result.interfaceName, result.tagType);
    }
    set_label_text(tech_value, buf);

    if (result.lastError != ST_ERR_NONE &&
            event == LILYGO_NFC_EVENT_START_FAILED) {
        snprintf(buf, sizeof(buf), "Error %u  %s", result.lastError,
                 result.errorText[0] ? result.errorText : "NFC error");
        set_label_text(ndef_value, buf);
        clear_records(buf);
    } else if (event == LILYGO_NFC_EVENT_NDEF_UNSUPPORTED) {
        set_label_text(ndef_value, "No NDEF");
        clear_records(card_detected_message(result));
        set_label_text(hint_value, card_hint_message(result));
    } else if (result.lastError != ST_ERR_NONE &&
               event == LILYGO_NFC_EVENT_NDEF_ERROR) {
        snprintf(buf, sizeof(buf), "Read error %u  %s", result.lastError,
                 result.errorText[0] ? result.errorText : "NFC error");
        set_label_text(ndef_value, buf);
        clear_records(buf);
        set_label_text(hint_value, NFC_TIPS_STRING);
    } else if (event == LILYGO_NFC_EVENT_CARD_RELEASED) {
        set_label_text(state_value, "Polling");
        set_label_text(ndef_value, "Ready");
        set_label_text(hint_value, NFC_TIPS_STRING);
    } else if (event == LILYGO_NFC_EVENT_CARD_DETECTED && is_uid_only_result(result)) {
        set_label_text(ndef_value, "ID only");
        clear_records(card_detected_message(result));
        set_label_text(hint_value, card_hint_message(result));
    } else if (result.hasNdef) {
        snprintf(buf, sizeof(buf), "%s  v%u.%u  %luB",
                 result.ndefStateText, result.ndefMajorVersion,
                 result.ndefMinorVersion,
                 static_cast<unsigned long>(result.ndefMessageLen));
        set_label_text(ndef_value, buf);
        set_label_text(hint_value, NFC_TIPS_STRING);
    } else {
        set_label_text(ndef_value, result.ndefStateText[0] ? result.ndefStateText : "--");
    }

    if (event == LILYGO_NFC_EVENT_NDEF_READ) {
        update_record_view(result);
        set_label_text(raw_value, result.rawHexPreview[0] ? result.rawHexPreview : "--");
    } else if (event == LILYGO_NFC_EVENT_NDEF_UNSUPPORTED ||
               event == LILYGO_NFC_EVENT_NDEF_ERROR) {
        set_label_text(raw_value, "--");
    } else if (event == LILYGO_NFC_EVENT_CARD_DETECTED) {
        if (!is_uid_only_result(result)) {
            clear_records("Reading NDEF...");
        }
        set_label_text(raw_value, "--");
    } else if (event == LILYGO_NFC_EVENT_STARTED || event == LILYGO_NFC_EVENT_CARD_RELEASED) {
        if (event == LILYGO_NFC_EVENT_STARTED) {
            clear_records("Waiting for card");
            set_label_text(hint_value, NFC_TIPS_STRING);
        }
        set_label_text(raw_value, "--");
    }

    /* Hold the latest completed read in memory; the user saves it with the Save card. */
    switch (event) {
    case LILYGO_NFC_EVENT_NDEF_READ:
    case LILYGO_NFC_EVENT_NDEF_UNSUPPORTED:
    case LILYGO_NFC_EVENT_NDEF_ERROR:
        nfc_capture_latest(result);
        break;
    case LILYGO_NFC_EVENT_CARD_DETECTED:
        if (is_uid_only_result(result)) nfc_capture_latest(result);
        break;
    default:
        break;
    }
}

static void goto_connect_wifi_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);
    LILYGO_LOG_PRINTF("Button %s clicked\n", text);

    if (strcmp(text, "Close") == 0) {
        if (msgbox) { destroy_msgbox(msgbox); msgbox = NULL; }
    } else {
        wifi_conn_params_t *params = (wifi_conn_params_t *)lv_event_get_user_data(e);
        if (params) hw_set_wifi_connect(*params);
        if (msgbox) { destroy_msgbox(msgbox); msgbox = NULL; }
        ui_show_wifi_process_bar();
    }
}

void ui_nfc_pop_up(wifi_conn_params_t &params)
{
    set_low_power_mode_flag(false);
    if (msgbox) return;

    static const char *btns[] = {"Connect", "Close", ""};
    char msg_txt[128];
    snprintf(msg_txt, sizeof(msg_txt), "Connect to \"%s\"?", params.ssid);
    msgbox = create_msgbox(lv_scr_act(), "NFC WiFi", msg_txt, btns,
                           goto_connect_wifi_event_cb, &params);
}

#if defined(ARDUINO) && defined(USING_ST25R3916)
void ui_nfc_on_reader_event(LilyGoNfcEvent event, const LilyGoNfcReaderResult &result)
{
    update_result_view(event, result);
}
#endif

static void stop_nfc_page()
{
    if (nfc_page_running) {
        hw_stop_nfc_discovery();
        nfc_page_running = false;
    }
}

static void back_event_handler(lv_event_t *e)
{
    stop_nfc_page();
    if (page_container) {
        ui_destroy_app_page(page_container);
        page_container = NULL;
    }
    state_value = NULL;
    uid_value = NULL;
    tech_value = NULL;
    ndef_value = NULL;
    record_body = NULL;
    raw_value = NULL;
    hint_value = NULL;
    filename_ta = NULL;
    save_btn = NULL;
    save_status = NULL;
    have_capture = false;
    menu_show();
}

void ui_nfc_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "NFC Reader", back_event_handler);

    lv_obj_t *status_card = ui_create_card(page_container, "Status");
    state_value = create_value_label(status_card, LV_SYMBOL_REFRESH, "State", "Starting");
    uid_value = create_value_label(status_card, LV_SYMBOL_LIST, "ID", "--");
    tech_value = create_value_label(status_card, LV_SYMBOL_SETTINGS, "Card", "--");
    ndef_value = create_value_label(status_card, LV_SYMBOL_OK, "NDEF", "--");

    lv_obj_t *record_card = ui_create_card(page_container, "Records");
    record_body = lv_obj_create(record_card);
    lv_obj_set_width(record_body, LV_PCT(100));
    lv_obj_set_height(record_body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(record_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(record_body, 0, 0);
    lv_obj_set_style_shadow_width(record_body, 0, 0);
    lv_obj_set_style_pad_all(record_body, 0, 0);
    lv_obj_set_style_pad_row(record_body, 4, 0);
    lv_obj_set_flex_flow(record_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(record_body, LV_OBJ_FLAG_SCROLLABLE);
    clear_records("Waiting for card");

    lv_obj_t *raw_card = ui_create_card(page_container, "Raw NDEF");
    raw_value = create_value_label(raw_card, LV_SYMBOL_LIST, "Hex", "--");

    /* ── Save card: read a card, accept or edit the name, tap Save ── */
    have_capture = false;
    lv_obj_t *save_card = ui_create_card(page_container, "Save");

    filename_ta = lv_textarea_create(save_card);
    lv_textarea_set_one_line(filename_ta, true);
    lv_textarea_set_max_length(filename_ta, 40);
    char default_name[48];
    capture_make_name(NFC_CATALOG, nfc_counter, default_name, sizeof(default_name));
    lv_textarea_set_text(filename_ta, default_name);
    lv_obj_set_style_bg_color(filename_ta, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(filename_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(filename_ta, lv_color_white(), 0);
    lv_obj_set_style_border_color(filename_ta, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_border_width(filename_ta, 1, 0);
    lv_obj_set_style_radius(filename_ta, 8, 0);
    ui_create_card_item(save_card, LV_SYMBOL_FILE, "Name", filename_ta);

    save_btn = lv_btn_create(save_card);
    lv_obj_set_width(save_btn, LV_PCT(100));
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    ui_add_accent_focus_style(save_btn);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(save_btn, LV_STATE_DISABLED);     /* enabled once a card is read */
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE " Save to SD");
    lv_obj_center(save_lbl);

    save_status = lv_label_create(save_card);
    lv_label_set_text(save_status, "Read a card, then Save.");
    lv_obj_set_width(save_status, LV_PCT(100));
    lv_label_set_long_mode(save_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(save_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(save_status, &lv_font_montserrat_12, 0);

    lv_obj_t *hint_card = ui_create_card(page_container, "Hint");
    hint_value = create_value_label(hint_card, LV_SYMBOL_WIFI, "Place", NFC_TIPS_STRING);

    bool nfc_started = hw_start_nfc_discovery();
    nfc_page_running = nfc_started;
    if (!nfc_started) {
        set_label_text(state_value, "Start failed");
        set_label_text(ndef_value, "NFC init failed");
        set_label_text(hint_value, "NFC init failed. Check SD/SPI sharing, then re-enter.");
        clear_records("NFC init failed");
    }
}

void ui_nfc_exit(lv_obj_t *parent)
{
    (void)parent;
    stop_nfc_page();
}

app_t ui_nfc_main = {
    .setup_func_cb = ui_nfc_enter,
    .exit_func_cb  = ui_nfc_exit,
    .user_data     = nullptr,
};

#else

static lv_obj_t *page_container = NULL;

static void back_event_handler(lv_event_t *e)
{
    (void)e;
    if (page_container) {
        ui_destroy_app_page(page_container);
        page_container = NULL;
    }
    menu_show();
}

void ui_nfc_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "NFC", back_event_handler);
    lv_obj_t *card = ui_create_card(page_container, NULL);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    LV_IMG_DECLARE(img_nfc_bg);
    lv_obj_t *img_obj = lv_image_create(card);
    lv_image_set_src(img_obj, &img_nfc_bg);
    lv_obj_set_style_image_recolor(img_obj, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_image_recolor_opa(img_obj, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(card);
    lv_obj_set_width(label, lv_pct(90));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
#if FACTORY_WATCH_UI
    /* A headline under the icon, not a paragraph (watch-ux-guidelines.md 1.3, 4.4). */
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, 0);
#else
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_SECONDARY, 0);
#endif
    lv_label_set_text(label, NFC_TIPS_STRING);
}

void ui_nfc_exit(lv_obj_t *parent)
{
    (void)parent;
}

app_t ui_nfc_main = {
    .setup_func_cb = ui_nfc_enter,
    .exit_func_cb  = ui_nfc_exit,
    .user_data     = nullptr,
};

#endif
