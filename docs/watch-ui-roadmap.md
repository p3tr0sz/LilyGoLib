# T-Watch Ultra: smartwatch UI roadmap

This fork changes the factory app on the **T-Watch Ultra** (502×410, about 315 ppi) so it behaves like a Xiaomi or Apple watch, not a small tablet. Other boards keep the original launcher.

Design rules, with their sources and pixel values for this screen, are in `watch-ux-guidelines.md`; new screens are checked against its review checklist.

All watch-only code is behind `FACTORY_WATCH_UI` (in `examples/factory/hal_interface.h`). The flag is on only for `ARDUINO_T_WATCH_S3_ULTRA`. The `emulator_watch_ultra` simulator env defines that board, so the simulator shows the new UI.

## Agreed design

- **Home is the watch face:** time, date and 4 corner slots. Each slot is an app shortcut or a value such as battery, Wi-Fi, steps or calories. Tapping a slot opens its app.
- **App list:** swipe up from the bottom of the face (Xiaomi style). It's a vertical list with 4–5 rows on screen.
- **Inside apps:** also vertical lists with 4–5 rows on screen.
- **Back:** a left-to-right swipe started anywhere, except on sliders, text fields, keyboards and horizontally scrolling content.
- **Lock screen:** configurable. It can be fully black (screen off) to save energy.
- **Buttons keep their hardware roles; no button navigation.**
  - PWR: wake, long press opens the power menu, 6 s hard power-off.
  - BOOT: download mode.

### Interaction rules (Mi Band style)

Every control is used with one gesture: a tap, a vertical scroll, a swipe right or a long press.

- **Tap:** a whole row is the tap target. A row opens a page, toggles a switch, or runs one action. There is never a small button inside a row.
- **Choosing an option:** a radio list with a check mark (`ui_create_card_picker()`), never a dropdown.
- **Changing a level:** the row shows the value ("Brightness 60% ›"). Tapping it opens a full-screen page with −/+ and a big bar. Sliders never sit in a scrolling list.
- **Actions:** a full-width pill with one verb ("Scan", "Set clock"). On a page with a question, one pill and a swipe right to go back.
- **Actions that can't be undone** (power off, restart, delete, forget): press and hold until the fill completes. A short tap only shows "Hold to confirm".
- **Messages:** full screen. A tap anywhere or a swipe right closes a message with one button.
- **No typing on the watch where it can be avoided:** pick from presets. A keyboard, if one is still needed, is full screen.

## Status

| Phase | What | State |
|---|---|---|
| 1 | Navigation shell: face, app list, swipe-right back, screen off | Done (`78c918b`) |
| 2 | Readable app pages and full-screen power menu | Done (`36a851d`) |
| 3 | Settings as nested lists | Done (`2b433e3`) |
| 4 | Configurable face and lock screen | Done; steps need checking on hardware |
| 5 | One-gesture shared controls: level pages, hold to confirm, whole-row switches, full-screen messages | Done (`525da4f`) |
| 6 | One-gesture app controls: pickers, action pills, Clock Tools time page, Display Test | Done; NFC Card and Arduino-only apps need checking on hardware |
| 7 | No typing: presets instead of keyboards, a full-screen keyboard where text is unavoidable | Done; LoRa Hub and Walkie need checking on hardware |
| 6b | Clock app (stopwatch, timer, alarms); date and time move to Settings; one screen margin | Done; alarm wake from light sleep needs checking on hardware |
| 7b | Steady keyboard field; WiFi and BLE scan rows like other card rows | Done; BLE rows need checking on hardware |
| 8 | Fixes from the audit against `watch-ux-guidelines.md` | Done; haptic patterns and File Manager need checking on hardware |
| 8b | App list order (drag to reorder, saved) and crash recovery | Done in the simulator; needs checking on hardware |
| 8c | Arrange apps, names, message pills, screen off in apps, pages follow the finger, fixes for apps outside the simulator | Done; the apps outside the simulator need checking on hardware |

## Next steps

In order. Details are in the follow-up sections further down.

1. **Get the firmware building, then check everything on the wrist.** `pio run -e twatch_ultra` in `LilyGoLib-PlatformIO` gets past SensorLib (`^0.5.1`) and Button2 (2.7.0), and the factory app and LVGL compile. It stops at `demo_ce.h: No such file or directory` in `src/nfc/LilyGoNfcService.cpp`. That header is in `lewisxhe/ST25R3916-NFC-RFAL` v1.0.1, the library `docs/third_party.md` lists; it combines the `ST25R3916-fork` and `NFC-RFAL-fork` the project pulls in now. Swap those two `lib_deps` for it. Then flash and go through the hardware checks: the phase 4–8c follow-ups and the crash-recovery test.
2. **Bigger features, in order of value:**
   - **Wi-Fi from the phone:** set up Wi-Fi over BLE, so there's no password typing.
   - **Quick settings:** swipe down on the face for brightness, Wi-Fi and BLE.
   - **Always-on clock:** a dim clock that stays on.
   - **Typing:** word completion on the keyboard.
   - **Accents in titles:** in the Alibaba title font.
   - **Steps:** keep today's count across a reboot.
   - **Corner picker:** group the apps instead of one 35-row list.

## Next phases

An audit on 2026-11-03 compared every app in the simulator with the Mi Band (see "Interaction rules" above). Navigation already matched; app pages did not.

### Phase 5: one-gesture shared controls

Changes in the shared helpers, so every app gets them at once:
- **Level rows** (`ui_create_card_slider()` on the watch): the row shows the value and a chevron and opens a full-screen page with −/+ (hold to repeat) and a thick bar. The row keeps a hidden `lv_slider` as its last child, so callbacks that call `lv_slider_get_value()` keep working.
  - **Bug this fixes:** a vertical scroll that started on a slider set the slider to the touch point instead of scrolling (Vibration jumped from Effect 1 to 59). The slider guard turned off the parent's scrolling on every press.
  - Sliders that apps build themselves no longer lock the parent's scrolling on a touch press, so a vertical drag scrolls the page. LVGL then leaves the slider alone.
- **Hold to confirm** (`ui_require_hold()`): on the watch a short tap shows "Hold to confirm", and holding fills the control for about 0.8 s and then runs it. Used for:
  - Power menu: Power Off and Restart.
  - Power app: Power Off and Restart.
  - SD Manager: Delete and Clean Logs.
  - WiFi Forget, BLE HID Forget, LoRaWAN session reset.
- **Whole-row switches** (`ui_create_card_switch()`): a tap anywhere on the row toggles it.
- **Full-screen messages** (`create_msgbox()` on the watch): full screen, larger text, buttons as stacked full-width pills. A swipe right presses the last button (the same one Esc presses). A message with one button also closes on a tap anywhere.

### What phase 5 changed

**`ui_watch_pages.cpp`** (declared in `ui_define.h`):
- **`ui_create_card_level()`:** what `ui_create_card_slider()` returns on the watch.
  - The row shows "24% ›". 0–100 and 0–255 ranges show a share of the maximum; other ranges (e.g. Waveform 1–117) show the number.
  - The row's children end with value, chevron and a hidden `lv_slider` (the model). `row_widget()`-style `lv_obj_get_child(row, -1)` still returns the slider.
  - The page has −/+ (96 px, hold to repeat, 20 steps from end to end) and a 28 px bar. It doesn't scroll. A swipe right that starts on the bar moves the bar instead of going back.
  - The page sets the model and sends `LV_EVENT_VALUE_CHANGED` to it, then shows the value the app kept.
- **`ui_require_hold(obj)`:** a preprocess `LV_EVENT_CLICKED` handler blocks the control's own click handlers. Holding for 800 ms sweeps a white fill across it, then sends the click. A short tap shows the "Hold to confirm" toast; a scroll cancels the hold. It does nothing on other boards.
- **`ui_toast(text)`:** a pill at the bottom of the top layer that goes away after 1.4 s.

**`ui_tools.cpp`, on the watch:**
- `ui_create_card_switch()`: the switch isn't clickable, so the whole row takes the tap, toggles it and sends `LV_EVENT_VALUE_CHANGED` to the switch.
- `create_msgbox()`: fills the screen. The title is 24 px, the text `UI_FONT_BODY`, both centred. Buttons are stacked full-width pills; with several buttons the last one (always Cancel/Close in this codebase) is plain text. A swipe right presses the last button; a one-button message also closes on a tap anywhere.
- Slider guard: a touch press no longer turns off the parent's scrolling, and sliders keep `LV_OBJ_FLAG_SCROLL_CHAIN_VER`. A vertical drag from a slider an app built itself now scrolls the page.

**Hold to confirm is on:** power menu Power Off and Restart (the hint says "Hold to confirm, swipe right to cancel"), Power app Power Off and Restart, SD Manager Delete and Clean Logs, WiFi Forget, BLE HID Forget, LoRaWAN session Reset. Sleep stays a tap, because PWR wakes the watch again.

### Phase 5 follow-ups

- **Messages with several buttons** (LoRa Hub save/delete, NFC Wi-Fi, LoRa CW) aren't in the simulator build. Check their layout on hardware.
- **Level rows don't notice `lv_slider_set_value()` on the model** without an event. No caller does this today; an app that does must also send `LV_EVENT_VALUE_CHANGED` or the row's label stays stale.
- **Brightness starts above 0** (the safe minimum), so its bar is empty at about 24%. Showing the share of the range instead would read 0% at the dimmest setting; decide after trying it on the watch.
- **Hold time:** 800 ms. Check on hardware whether it feels too slow or too quick.

### Phase 6: one-gesture app controls

App by app, checked in the simulator:
- **Pickers instead of dropdowns:** switch `ui_create_card_dropdown()` to `ui_create_card_picker()` (QRCode, WiFi Tools, BLE Scan, WiFi Analyzer, LoRa Hub, Bad USB, I2S Test, INA219, Track/Sensor Log). Then the apps that create `lv_dropdown` themselves: NFC Card 2, Sub-G 7, NRF24 6, Radio 8, IR Universal, Walkie.
- **Action pills instead of "label + button" rows:** a new `ui_create_card_action(card, icon, label, cb)` draws one full-width pill. Each `ui_create_card_button()` call (about 30) gets one clear verb, e.g. "Scan [Scan Now]" → "Scan", "Generate [Update]" → "Update code", "Export [CSV]" → "Export CSV".
- **Clock Tools:** replace the 6 date and time dropdowns, "Load Now" and "Apply" with a "Set time" page of scroll wheels (`lv_roller`) and one "Set" pill. Network Time stays one pill.
- **Charge Current:** a picker of currents in mA (Phase 3 follow-up).
- **Display Test:** tap anywhere for the next pattern, swipe right to leave; drop the 4 small buttons.
- **App sliders:** volume in Player, Net Radio and Recorder open the level page from phase 5. (Mi Light Bar is Pager-only.)

### What phase 6 changed

**Shared helpers (every app gets these at once):**
- **No dropdowns on the watch.** `ui_create_card_dropdown()` returns `ui_create_card_picker()`, the way `ui_create_card_slider()` returns a level row.
  - `ui_create_card_item()` also turns any `lv_dropdown` an app builds itself into a picker row (`ui_picker_adopt()` in `ui_watch_pages.cpp`). It resets the app's sizes and colours, so Walkie, Track Log, Sensor Log, BadUSB, INA219, I2S Test and the LoRa Hub editor get pickers with no change of their own.
  - The row's last child is still the `lv_dropdown`, so `lv_dropdown_get_selected()`, `lv_dropdown_set_options()` and widths that apps set keep working. The picker reads the options when it opens.
- **Action pills.** `ui_create_card_button()` returns `ui_create_card_action()`: one full-width 64 px pill with the verb, dark text on the accent (it reads on every theme, Mono's white included). The title only labelled the button and is dropped.
  - The row's last child is the button and the button's first child its label, so `row_widget()`, `ui_require_hold()` and `set_button_text()` keep working. A disabled pill turns grey.
  - Pills aren't in the focus group, so a tap leaves no focus ring.
  - Buttons whose text wasn't a verb on its own got one under the flag, e.g. "Now" → "Scan now", "Update" → "Update code", "CSV" → "Export CSV", "Reset" → "Reset counters" / "Reset session", "Send" → "Send uplink".

**Clock Tools:**
- One "Set" card: Zone, Date, Time and "Sync network time".
- Picking a zone applies it; there's no "Save" step.
- Date and Time open a page of wheels (`lv_roller`: day, month, year; or hour and minute) with one pill, "Set date" or "Set time". Setting the time zeroes the seconds. The page closes when the clock is set and shows a toast if it fails.
- Other boards keep the six dropdowns, "Load Now" and "Apply".

**Settings → Power → Charge Current:** a picker of currents in mA from the new `hw_get_charge_level_current()` (Arduino: `getChargeLevelToCurrent()`). Levels of 0 mA are left out; the Charger switch turns charging off.

**Display Test:** no buttons. Tap for the next pattern, long press for the previous one, swipe right to leave. Each change shows a caption ("03/13 Red" and the gestures) that hides after 2 s.

**Volume in Player, Net Radio and Recorder:** a level row that opens the phase 5 level page.

**Simulator:** "setting" the time keeps an offset from the host clock, so Clock Tools works on desktop.

### Phase 6 follow-ups

- **Check on hardware** (these apps aren't in the simulator build, or have no radio there):
  - Pickers in LoRa Hub (antenna, mode, payload and the profile editor), Walkie channel, BadUSB script, Track/Sensor Log interval, INA219 shunt and I2S Test signal.
  - Volume rows in Player, Net Radio and Recorder.
  - Charge Current lists the PMU's real levels (the simulator shows only two).
- **NFC Card** shows a black screen in the simulator, also before phase 6. Its layout is built by hand (a side panel with a dropdown and three small template buttons), so it needs its own watch layout, checked on hardware.
- **Sub-G, NRF24, Radio and IR Universal** build their own controls. They are external modules or other boards, so they were left as they are.
- **Mi Light Bar** is registered on the Pager only, so its sliders stay.
- **Pickers with many options** (time zones: 26 rows) scroll to the current choice when they open. Check on the watch whether that is quick enough.

### What phase 6b changed

Asked for on 2026-11-04: setting the date and time is a setting, and Clock Tools should be a stopwatch, timer and alarm.

**Settings → Date & time** (`ui_clock_settings_build()` in `ui_clock_tools.cpp`): Zone, Date, Time and "Sync network time", as the phase 6 Clock Tools page had them. Results show as toasts.

**Stopwatch, Timer and Alarm** (new `ui_watch_clock.cpp`): three apps of their own on the watch, in place of Clock Tools (first they were three rows inside it). Stopwatch and Timer have new icons (A8 images from `tools/gen_watch_icons.py`), Alarm keeps the clock icon. Face slots saved as "Clock Tools" open Alarm; the default bottom-left slot and the Date slot open Alarm too.
- **Stopwatch:** big digits, Lap / Reset and Start / Pause round buttons, lap splits below (newest first, up to 30). Each digit sits in a cell as wide as the widest digit, so the line doesn't jitter as it counts (the timer countdown too).
- **Timer:** round presets (1, 3, 5, 10, 15, 30 min), a "Recent" row of round buttons, like the presets, for the 3 most recent custom lengths ("5 min / 2 s", "1 h / 30 min", "1:05:10"; saved, `hw_get_watch_timer_recent()`), and a "Custom" pill that opens h:m:s wheels starting at the last custom length. A custom length equal to a preset isn't added. Running: a ring that empties, the time left, Cancel and Pause / Resume.
- **Alarm:** up to 4 alarms (`hw_get/set_watch_alarm()`, 16 of the spare settings bytes). Rows show time, repeat and On / Off; tapping one opens On, Time, Repeat and "Delete alarm" (hold). Repeat opens a page with the presets (Once, Every day, Weekdays, Weekends) as a radio list and a switch per day, Monday first; other sets of days show as "Mon Wed Fri" or "Mo Tu We Th Sa". "Add alarm" opens the time wheels; picking a time turns the alarm on and toasts "Rings in …". A "Once" alarm turns itself off after ringing.
- **Engine:** keeps running when the app is closed. A finished timer or due alarm opens a full-screen page over anything, vibrates every 1.2 s for up to a minute and keeps the screen on. Stop, or Snooze 9 min for alarms. An alarm rings when the minute changes, so setting the clock forward onto an alarm rings it.
- **Screen off:** `screen_off()` passes `watch_clock_next_wake_ms()` to `hw_low_power_loop()`, and the Ultra's `lightSleep()` takes a new optional timer wake-up (`src/LilyGoWatchUltra.cpp`). Alarms don't wake the watch from deep sleep or power off.
- Other boards keep the old Clock Tools page.

**Wheels** moved to `ui_watch_pages.cpp` as `ui_create_wheel()`, `ui_push_wheel_page()` and `ui_wheel_number_options()`.

**One screen margin:** `UI_SCREEN_MARGIN` (24 px) is the side margin of app pages, the app list and the ring page; it was 12 px on pages and 0 on the IMU page. Inside cards, `UI_CARD_PAD_HOR` (16 px) lines up caption, icons, values and full-width dividers (they were 95 % wide, so they stopped short of the values); rows have no side padding of their own, and a pressed row's highlight is drawn wider.

**Chevrons only where they're needed:** pill rows (Settings sections, Clock Tools, alarms) and Power's button rows have no ">"; they plainly are buttons. Value rows inside cards (pickers, level rows, Date and Time) keep it, because it is what tells them apart from read-only info rows.

**Icons instead of words where the icon says it:** `ui_create_card_icon_action()` makes an icon-only pill. Confirming wheels (Set date, Set time, alarm time) shows ✓, Start on the custom timer ▶, Add alarm +, and Delete alarm a bin (still hold to confirm). The stopwatch's left button is a lap flag while running and a reset arrow when paused; both are A8 images in `src/img_watch_icons.c`, drawn by `tools/gen_watch_icons.py` (LVGL's symbol font has neither). Words stay where an icon would be a guess: Stop and Snooze on the ring page, Custom, Sync network time.

**Direct corner edits:** a long press on a face corner opens its choices; picking one, or swiping back, returns straight to the face instead of the "Watch face" list. Settings > Watch face still shows the list.

### Phase 6b follow-ups

- **Check on hardware:** an alarm and a timer ring with the screen off (light-sleep timer wake), the vibration repeats, and alarms survive a restart.
- No sound yet: `hw_audio_beep()` could add one.
- Apps that set their own row padding (Music, Net Radio, BLE Scan, Wireless) may sit a few pixels off the card's 16 px line.

### Phase 7: no typing

- **WiFi Tools host:** a list of presets (gateway, 1.1.1.1, 8.8.8.8, example.com, last used).
- **QRCode:** content from templates only (device info, Wi-Fi, URL); the free-text field goes on other boards.
- **WiFi password:** keep the keyboard, but full screen with large keys. Check whether provisioning from a phone (BLE or SoftAP) is worth it.
- **LoRa Hub, Walkie, IR:** review each text field; replace with presets or numeric pickers where possible.

### What phase 7 changed

**Shared** (`ui_watch_pages.cpp`):
- **`ui_open_text_page(from, title, ta)`:** a full-screen keyboard page. A big pill field at the top holds a copy of the text, and a two-tap keyboard (one `lv_buttonmatrix`) fills the rest. LVGL's QWERTY keyboard was tried first: with ten keys a row, letters were about 40 px (3 mm) wide.
  - **Groups,** where a phone keypad has them (ITU E.161): .,?! abc def / ghi jkl mno / pqrs tuv wxyz / 123 space ⌫; keys about 155 × 62 px. ✓ is a round button beside the field, so every key grid is three columns wide.
  - **A group:** its letters in lower case, then upper case, on keys about 155 × 85 px, a wide back key and the group's keypad digit (abc → 2). So there's no shift key. The .,?! group, the keypad's "1", holds . , ? ! ' - @ :. Picking a character goes back to the groups.
  - **123 / #+=:** digits and 20 symbols, switched with "#+=" and "123" as on a phone; they stay open for runs until "abc".
  - **Cursor:** a thin steady accent bar (no blinking, no block that changes width).
  - **Number-only fields:** a 3 × 4 number pad, the last row . 0 ⌫.
  - Keys act on release (`LV_BUTTONMATRIX_CTRL_CLICK_TRIG`) and the matrix has `UI_FLAG_SWIPE_BACK_OK`, so swiping right across the keys cancels instead of typing. Only ⌫ repeats when held. The ✓ button writes the text back to `ta` and sends `LV_EVENT_VALUE_CHANGED` and `LV_EVENT_READY`, so apps' existing handlers run; the keyboard key or swiping right leaves `ta` as it was. Fields that accept only digits get the number pad. Password mode, maximum length, accepted characters and placeholder are copied from `ta`.
- **Text field rows:** `ui_create_card_item()` with an `lv_textarea` now calls `ui_text_adopt()` on the watch, as dropdowns call `ui_picker_adopt()`. The field is hidden and stays the row's last child; the row shows its text ("Enter >" when empty, dots for a password) and opens the keyboard page. This covers LoRa Hub's profile Name, Seconds and Custom Sync and Walkie's Name without changes in those apps.

**Accents:**
- **Fonts:** on the watch, every Montserrat size the app uses (10–48 px) points at `font_latin_<size>` (`src/font/font_latin.c`, `#define`s in `ui_define.h`). Each holds Latin-1, Latin Extended-A, curly quotes, dashes, … and € (234 glyphs) and falls back to LVGL's built-in font, so ASCII and icons are unchanged. About 0.45 MB of flash, Ultra builds only. `tools/gen_latin_fonts.py` draws them from LVGL's own `Montserrat-Medium.ttf` with Pillow (no `lv_font_conv` needed) and checks its metrics against the built-ins.
- **Keyboard:** Settings > Keyboard > Accents picks None, Polish, Czech & Slovak, German, French, Spanish, Portuguese, Italian, Nordic, Hungarian or Turkish (`hw_get/set_watch_keyboard_accents()`, one spare settings byte). The language's letters join their base letter's group page, after it: abc → a ą b c ć, capitals below. Up to 10 letters a group (French abc and def have 7, about 62 px keys).

**WiFi Tools:** Host is a picker: Gateway (the current gateway IP), 1.1.1.1, 8.8.8.8, example.com, lilygo.cc, the last typed host, and "Other..." which opens the keyboard. The typed host is kept until restart.

**QRCode:** no typing on the watch. "Show" picks Device, WiFi, Location (with GNSS) or Website and the code updates at once; the text it holds is shown under a 240 px code. Other boards keep the text field.

**WiFi:** tapping a network pushes a page instead of the small pop-up: Signal, Security, Password (keyboard page; a saved password is filled in), Show password, and a Connect pill. Swipe right cancels. "Forget" and "Scan" are full-width pills ("Forget network" still needs a hold, "Scan networks").

**LoRa Hub chat:** the text bar becomes a "Message" pill. It opens quick replies (OK, Yes, No, On my way, Call me, Where are you?), each sent with one tap, and "Type a message" on the keyboard page.

**Not changed:** the IR apps aren't built for the Ultra (no IR transmitter). Sub-G, NRF24 and Radio are external modules; any text field they put in a card row gets the keyboard page anyway.

### Phase 7 follow-ups

- **Check on hardware:** LoRa Hub (quick replies send; profile editor fields open the number pad or keyboard and save) and Walkie's Name. Neither runs in the simulator.
- **Phone provisioning for Wi-Fi:** worth adding later. Espressif's BLE provisioning (ESP BLE Prov app) or a SoftAP page would remove the password typing; it needs a provisioning library and a pairing flow, so it is a separate piece of work.
- **Accents in titles:** page titles and the face use the Alibaba fonts (ASCII only), so an accented Wi-Fi name in a title shows boxes. Those fonts would need the same treatment.
- **Typing is two taps a letter.** A word-completion bar above the keys would cut that for messages; it needs a small word list in flash.
- **Typing on the phone:** `screen_web_server.cpp` only serves screenshots (and only with `LILYGO_SCREEN_CAPTURE_ENABLED`), and it needs Wi-Fi first, so it can't help with the Wi-Fi password. A text-entry page on it could serve LoRa messages and names; BLE provisioning (above) covers Wi-Fi.

### Phase 7 polish: steady text field, readable scan lists

**Keyboard page field** (`ui_open_text_page()`):
- **Passwords:** LVGL showed each new letter for 1.5 s and then swapped it for a much narrower dot, so the text and cursor jumped back after every key. The field now shows dots at once (`lv_textarea_set_password_show_time(edit, 0)`), 8 px apart so they can be counted. The key just pressed already shows the letter; "Show password" on the WiFi page shows the text.
- **Show password:** a password field has a round eye button between the field and ✓. A tap switches between dots and the text (crossed-out eye while it's shown). It always opens hidden. Added 2026-11-05: a slip is easy on a small screen.
- **Long text:** LVGL animated the scroll to the cursor on every key, so text past the right edge slid back and forth. `kb_snap_scroll()` jumps there instead, after each key and when the page opens.

**Scan lists** (`ui_create_card_signal_row()` in `ui_watch_pages.cpp`): a found network or device is a normal card row: the icon in the signal's colour, the name in the body font over a grey detail line, and the signal ("-58 dBm") on the right in the same colour. Same height, edges and divider as other rows; a long name ends in dots. The lists sit in the page's scroll instead of a 218 px box that scrolled inside it.
- **WiFi:** detail "Channel 6 · WPA2". A tap opens the network's page as before.
- **BLE Scanner:** detail "type · 12s ago"; an unnamed device shows its address as the name. A tap opens a page for the device (name, address, signal, type, seen, service, maker data, advert), which keeps updating; the "Selected Device" card below the list is gone on the watch. Its labels are cleared on `LV_EVENT_DELETE`.

**Card rows:** the icon sits in a 30 px column (`UI_ROW_ICON_W`), so titles start at the same x whatever the icon's width (WiFi's Signal, Security and Password rows used to be a few pixels apart).

**Simulator:** a WiFi scan returns four networks (strong to weak, one open, one with a long name) instead of one.

### Phase 7 polish follow-ups

- **Check on hardware:** BLE Scanner rows and the device page (the simulator has no BLE).
- Password dots are LVGL's bullet, which is small at 32 px. A larger dot (●) would need a glyph added to `font_latin_32`.
- BLE Scanner on the watch still opens on Overview and Controls; the device list is the third card. Moving it first would show devices at once, but the list's height changes on every refresh and would push the controls around.

### Phase 8: audit against the UX guidelines (2026-11-05)

Every app that runs in the simulator was checked against `watch-ux-guidelines.md` (numbers in brackets are its rules). Navigation, the face, Settings, pickers, level pages, hold to confirm, the Clock apps and the keyboard already follow it. The gaps, most important first:

**A. Type is a size too small everywhere** (4.1). Apple's watchOS body text on this screen is about 32 px and its minimum about 22 px. Here `UI_FONT_BODY` is 22 px (the minimum) and `UI_FONT_CAPTION`/`UI_FONT_TINY` are 16 px, below it. Below 16 px: I2C Scan's "Tap Scan to start", Sensor's Roll/Pitch/Heading labels, WiFi Analyzer's chart labels and "Last scan" line.
- Proposal: body 26–28 px, caption 22 px, nothing under 20 px; titles and big numbers stay. Rows then need about 88 px (see B). Do it in the tokens, check each app's page in the simulator, and move long values to their own page where they no longer fit.

**B. Tap targets are under the comfortable size** (3.1). 82 px is the comfortable height, 52 px the minimum.
- Card rows 76 px, app and menu list rows 72 px, action pills about 62 px: raise to 82–88 px together with A.
- Under the minimum: I2C Scan's Scan pill (about 36 px), SD Manager's "View" and "Delete" buttons inside rows (about 30 px, and a button inside a row breaks the one-target-per-row rule).

**C. Contrast fails on some themes** (5.1, 4.5 : 1). Grey secondary text `#888` on the card: Mint 4.9, Ocean 4.8, Sunset 4.7, Violet 4.8, Cherry 4.7, but Lime 4.3, Amber 4.1, Mono 4.1. Accent text on the card: Violet 4.4. Fix: secondary grey `#9A9A9A` or lighter (or per-theme), Violet accent a step lighter.

**D. The first screen isn't the thing people came for** (1.1, 1.2).
- BLE Scanner opens on counters (Shown, All Seen, Named, Strongest) and six controls; devices are the third card. WiFi opens on connection details, networks below. WiFi Analyzer: counts before the chart.
- Fix: list first (BLE devices, WiFi networks), one summary line above it, scanner options (Active Scan, Named Only, Auto Refresh, Interval) on an "Options" sub-page (Garmin settings menu, 2.6). BLE's list still changes height on refresh, so keep its rows' order stable.

**E. Rows of "--"** (1.1, 1.3). WiFi when disconnected shows SSID, IP, RSSI and MAC all "--"; WiFi Tools Gateway, DNS, MAC, RSSI; SD Card SPI, Write, Read. Show one line that says the state ("Not connected", "No card") and hide the empty rows.

**F. Icons that don't say anything** (3.7, Apple: familiar icons). A gear marks MAC, DNS, Port, Best CH, Range, Bus, Waveform, BSSID and Security; a list icon marks Shown, Named, Channel, Items and Show. Either an icon that means the thing or none: info rows can go without one, with the title at the icon column's edge.

**G. Jargon and cryptic values** (1.3, 4.4): RSSI (say "Signal"), BSSID, CTRL, PMIC, Gauge, SPI, Waveforms "1 - 117", "Best CH1 load 1:50 6:400 11:155". Plain words on the watch; the technical name can sit on the detail page.

**H. Long text on a glance screen** (4.4). NFC's instruction is five lines; make it a headline ("Hold a card to the screen") under the icon. Power's descriptions ("The PMIC cuts system power completely…") belong on the hold-to-confirm hint, not under every option.

**I. Values that wrap** (1.3). WiFi Analyzer "Strongest LilyGo-AABB0 -42 dBm" takes two lines; split name and signal, as the scan rows do.

**J. One vibration for everything** (8.1). `hw_feedback()` has 76 callers and one pattern: taps, page pushes, the swipe-back threshold, results. Add a few named patterns (tap, success, failure, start/stop, alert) and use the tap one sparingly; keep the Settings switch that turns haptics off.

**K. Smaller things**
- QRCode's Device code says "LilyGo T-LoRa-Pager (2025)" in the simulator; check the name on the Ultra.
- Sensor (IMU): numbers are body-size under a large empty dial; the numbers should be the big thing (4.3). Its "IMU" caption sits on the card edge instead of the 16 px line.
- LoRa Hub without a module says "Radio module not detected." and nothing else; say what to do (7.2).
- Power's three options use three different styles (red fill, outlined, plain); use one row style and the hold to mark the dangerous ones.
- App list: one flat list of every app (2.6 suggests seven or so a level). Pinning favourites or recent apps at the top would cut the scroll.

**Not checked:** BLE HID, Music, Net Radio, Compass and Walkie didn't open by name in the simulator, and hardware-only apps don't run there.

**Simulator note:** a sideways drag made with `cliclick` opened the row under it (BLE Scanner's Interval) instead of going back. Found in phase 8: `cliclick` moves the pointer without the button held, so LVGL saw a tap, and a scripted drag that takes longer than LVGL's long-press time (400 ms) is cancelled as a long press. A drag posted as real `leftMouseDragged` events, starting with a move to the start point and taking about 200 ms, goes back as expected (see Testing).

### What phase 8 changed

Every item above, checked app by app in the simulator. Other boards are unchanged: each change is under `FACTORY_WATCH_UI`.

**A. Type** (`ui_define.h`): `UI_FONT_BODY` 28 px (it was `MAIN_FONT`, 22 px; the Ultra's `MAIN_FONT` is 28 px too, so unstyled labels follow), `UI_FONT_SMALL` 24, `UI_FONT_CAPTION` and `UI_FONT_TINY` 22. The 10, 12, 14 and 16 px Montserrat sizes that apps set directly draw at 22 px and 18 at 24, so nothing that has to be read is under the guidelines' 22 px floor (4.1). The roadmap's proposal said 20 px; the guidelines say 22, so 22 it is. App list and pill text use the body size.

**B. Targets:** card rows, list rows and app list rows are 88 px, action pills 82 px (3.1).
- I2C Scan: the 36 px Scan button is a full-width pill; "Tap Scan to start" is gone (the Devices row says 0).
- File Manager (SD Manager): no buttons inside rows. Files come first, as card rows (name, size); a folder opens, a file pushes its page with Size, "View" (text files) and "Delete file" (hold, "Hold to delete"). The preview is a pushed page that scrolls, not a box with a small close button. Storage below: Capacity and Used only with a card, then Refresh and "Clean logs" (hold). Results ("Deleted …") are toasts.
- The touch guide's two buttons are stacked 82 px pills (the second wrapped onto two lines at the new size).

**C. Contrast:** secondary grey `#A0A0A0` on the watch (5.5 : 1 or more on every preset); Violet's accent `#B577FF` (5.7 : 1, was 4.4). Figures in the guidelines, 5.1.

**D. The thing first:**
- **BLE Scanner:** a "Nearby" card with one summary line ("12 found, 8 named") over the device list, then "Scan now" and an "Options" row. Options (pushed page): Active scan, Named only, Auto refresh, Every (15/30/60 s, kept while the page is closed) and "Clear list". The Shown / All Seen / Named / Strongest counters are gone on the watch.
- **WiFi:** with the empty rows hidden (E), the networks are on the first screen.
- **WiFi Analyzer:** a "Channels" card first: "4 networks, 1 open", the chart (170 px, axis numbers kept inside it) and "Least crowded: channel 6". Networks below as scan rows ("Channel 6 · WPA2", signal on the right), then "Scan now" and "Options" (Auto scan, Every, Export CSV). The Overview and Selected AP cards are gone on the watch.

**E. No rows of "--":** `ui_card_row_set_hidden(row, hidden)` (new, `ui_tools.cpp`) hides a card row with its divider and keeps dividers only between visible rows.
- **WiFi:** WiFi switch and "Network" (Off / Not connected / Not found / the network's name). IP, Signal and "Forget network" show only while connected. The SSID row is gone (Network says it); MAC is in Settings > About.
- **WiFi Tools:** "Network: Not connected", or the name with IP, Router, DNS server, Signal and Refresh.
- **SD Card:** one "Card" row (No card / Not mounted / Mounted) instead of the coloured dot (colour alone, 5.3) and "Status: Unknown"; Size, Format and Bus speed once mounted; the Speed Test card only with a card, Write and Read only after a test.

**F. Icons:** a gear (`LV_SYMBOL_SETTINGS`) or list icon (`LV_SYMBOL_LIST`) passed to a card row draws nothing; the icon column stays, so titles still line up (3.7). Rows that got new icons have ones that mean something (folder, file, drive).

**G. Plain words:** RSSI → Signal; Gateway → Router; Best CH and the load line → "Least crowded: channel 6"; Monitor's PMIC → Power chip, Gauge → Battery chip, VBUS → USB volts, SOC → Charge, Battery I → Battery current, NTC → Battery sensor, and so on; SD's Type → Format, SPI → Bus speed. BSSID only appears on other boards now.

**H. Short text:**
- **NFC:** "Hold a card to the screen" under the icon, in white (it was five grey lines).
- **Power:** three rows of one style (Power off, Sleep, Restart), like Settings. The overview card (eyebrow, PMIC paragraph, CTRL line) is gone; Power off and Restart need a hold, and a tap says what the hold does ("Hold to power off", "Hold to restart") through the new `ui_require_hold_hint()`. In ship mode, one line above the rows says whether USB-C must be unplugged.

**I. Values that wrap:** WiFi Analyzer's Strongest row is gone (the list is sorted by signal); Monitor shows the percentage in 48 px with the state beside it, without the chip names that wrapped (they're in Status), and drops the State row that repeated it.

**J. Haptics** (`hw_haptic()` in `hal_interface.cpp`): TAP (the effect chosen in Vibration; what `hw_feedback()` plays), SUCCESS (double click), FAILURE (triple click), START (strong click), STOP (short double click), ALERT (750 ms alert), each a DRV2605 library waveform; the chosen tap effect is restored after each.
- Hold to confirm ends with SUCCESS; WiFi connect gives SUCCESS or FAILURE; Stopwatch start/pause and Timer start/pause/cancel give START or STOP; a ringing alarm or timer gives ALERT.
- Pushing a page and opening the app list no longer buzz (8.2).
- **Settings > Vibration** (new section): one switch, saved in a spare settings byte (`hw_get/set_watch_haptics()`, 0 = on, so older saves read as on). Off silences everything except ALERT: alarms and timers still vibrate, which the page says.
- The Vibration app has one job: "Tap feel", the Effect level row and a Play pill; the DRV2605, Driver and Library rows are gone.

**K. Smaller things:**
- **QRCode** on the simulator now says "LilyGo T-Watch Ultra (2025)", as the hardware does (`hw_get_variant_name()`).
- **Sensor (IMU):** the dial on the left and Roll, Pitch and Heading in 32 px on the right; no "IMU" caption on the card edge.
- **LoRa Hub without a module:** "No LoRa radio found" and "Restart the watch to try again".
- **App list:** a "Recent" section was tried here and replaced by the person's own order (next section).

**Cards:** the last row in a card no longer draws a divider under it.

### App list order (2026-11-05)

Asked for: the app list's order is the person's, set by dragging, and kept through power off, like the face's corner shortcuts.

- **Drag:** long-press a row (it lifts: accent outline, shadow, a tap buzz) and drag it; a grey outline shows where it will land, and the other rows make room. Holding the finger within 70 px of the list's top or bottom edge scrolls the list. Letting go drops it there. The release after a drag doesn't open the app; a plain tap still does. While a row is held it doesn't pass vertical drags to the list, so the list doesn't scroll under the finger, and the back swipe is off (a long press cancels it).
- **Saved** by app name hash (`watch_app_name_hash()`, renames covered as for face slots) through `hw_get/set_watch_app_order()`. That's up to 64 hashes, too many for the settings blob's 11 spare bytes, so on the watch they have their own Preferences namespace (`app_order`, key `order`). The simulator writes `watch_app_order.bin` in its working directory, so the order survives a simulator restart too.
- **Building the list:** saved order first (apps that are gone are skipped), then any app not in it, in registry order, so a newly added app shows at the end.
- **Not done:** guideline 2.4 says a long press is never the only way to do something, and here it is. An "Arrange apps" row at the end of the list that turns on dragging without the long press, or the same list in Settings, would give it a visible way in. The face corner picker still lists apps in registry order, not this one.

### Crash recovery (2026-11-05)

Asked for: when something goes wrong, go back to the main screen instead of freezing.

What froze before: LVGL's checks (a NULL object, a failed allocation) ended in `while(1);` (`src/lv_conf.h`), and nothing watched the UI loop, so an app stuck in a loop hung the watch until the battery ran out. A CPU fault already restarted the watch (the panic handler prints and reboots), but through the logo and boot sound.

A crash leaves memory and LVGL in an unknown state, so it isn't continued in place. Instead:
- **LVGL checks** call `lilygo_lv_assert_failed()` (new `src/lilygo_lv_assert.h`). The library's weak default still halts, so other examples behave as before; the factory app's version restarts.
- **Frozen UI:** an `esp_timer` checks every second that `loop()` ran in the last 20 s (`HW_UI_WATCHDOG_MS`), and restarts the watch if not. It pauses while the watch is in light sleep, where `loop()` legitimately doesn't run. An app call that blocks for more than 20 s would also trigger it; none is known.
- **Coming back:** the open app's name is kept in RTC memory (`RTC_NOINIT_ATTR`), which survives a restart, panic or watchdog reset. After such a reset (`esp_reset_reason()`: panic, interrupt or task watchdog, or our own restart), the watch skips the logo wait and boot sound and starts on the face with a full-screen "WiFi stopped / The watch restarted to recover." A normal power on shows nothing.
- **C++ exceptions while an app opens** (for example `std::bad_alloc`) are caught in `ui_launch_app()`. The half-built page is removed and the face shows "<app> stopped / Back on the watch face.", with no restart. Exceptions thrown later, from LVGL callbacks, can't unwind through LVGL's C code; they end as a panic, so the restart path covers them.

Checked in the simulator with temporary hooks (since removed): the message after a crash restart, and an exception while opening an app from the app list.

**Check on hardware:**
- Force each case: an `LV_ASSERT_NULL(NULL)`, an endless loop in an app, and a NULL pointer write. Each should come back on the face with the app's name within about 20 s (a few seconds for the faults).
- Leave the watch asleep for a few minutes: it must not restart on waking.
- A normal power on shows no message.

### What phase 8c changed (2026-11-05)

Items 2–6 of the previous next-steps list.

- **Arrange apps:** the last row of the app list. Tapping it shows a drag handle (≡) on every app row and turns the row into "Done". Dragging a handle moves its row straight away, with no long press, and dragging elsewhere on the row scrolls the list. The long press stays as a shortcut, so it is no longer the only way to reorder (2.4). The mode ends when the list closes.
- **Corner picker order:** it lists apps in the app list's order (`watch_app_list_order()` in `ui_app_list.cpp`, which the list uses too).
- **App names = page titles** on the watch (`watch_app_names[]` in `ui_main.cpp`):
  - BLE Scan → BLE Scanner, I2C Scan → I2C Scanner, SD Info → SD Card, IMU/Accel → Sensor
  - File Manager → Files (its title too), QRCode → QR Code (its title too)
  - DisplayTest → Display Test, LoRa → LoRa Hub, IR Record/Play → IR Recorder/Player, Walkie → Walkie Talkie

  The old names are in `app_renames[]`, so saved corners and the saved order still find them. `FACTORY_SIM_APP` takes the new names.
- **Messages:** `create_msgbox()` buttons are 82 px pills with dark text and the body font, like action pills. The plain last button is 64 px text.
- **Text floor:** 20 px Montserrat draws at 22 px too.
- **Screen off inside apps:** after the display timeout an open app keeps running with the backlight off. There's no light sleep, so music, recording, scans and timers carry on. The next touch or button lights it again in the same app, and that touch presses nothing. Apps that call `set_low_power_mode_flag(false)` stay lit. The simulator shows a black overlay; click it.
- **Lock clock:** no breathing glows on the watch.
- **Pages follow the finger** (10.1, `ui_navigation.cpp`): a sideways drag moves the page with the finger instead of showing the bubble. Letting go past the threshold slides it out and goes back; short of it, it slides back. While the page follows, the long press is suspended and the 1 s swipe limit doesn't apply, so a slow or paused swipe still works and doesn't lift an app list row. Pushed pages show the page below. An app's top page shows black behind it, because the list or face is on another tile.
- **Apps outside the simulator**, from the audit:
  - **NFC Card:** one scrolling list instead of two columns. A headline state ("Waiting for phone"), "Share link / text / Wi-Fi" pills, or one "Stop" while sharing, and a plain card-type picker. The simulator shows an "NFC Card" page saying it needs the watch, instead of a black screen.
  - **Player:** 64/82 px round controls, track rows as menu rows with a speaker icon on the current one, title in the body font, a volume level row, and "No music on SD".
  - **Net Radio:** streams first, the Config path row gone, a volume level row, and "Connect to Wi-Fi first". `hw_set_volume()` scales samples in software, so volume works on the Ultra's amplifier.
  - **Walkie Talkie:** a 160 px talk button with "Hold to talk", a "Peers (N)" row that opens a pushed page, no Back button, and a Start pill. Losing the press (it turned into a scroll) also stops talking.
  - **Recorder:**
    - Record/Stop and Play are stacked pills; Stop is fully opaque while recording.
    - "00:03 / 00:05" in 48 px.
    - The microphone opens only while recording, so there's no idle level animation. The level card shows only while recording.
    - Play says "Playing" while playing; playback can't be stopped.
  - **GNSS:**
    - A headline first ("No fix · 4 satellites", or the position).
    - Position rows show only once they have a value, with separate Date and Time rows.
    - Per-system satellite counts ("GPS 8 (3 used) · Galileo 5") replace the sideways colour-only chart. "Time pulse Yes/No" replaces the blinking LED.
    - Model, First fix, Data received, NMEA to serial and Beep on fix are on a "Details" page.
    - The NMEA screen has an 82 px "Stop sending" pill.
  - **LoRa Hub:**
    - Profiles are 88 px menu rows, the active one marked "Active".
    - "Edit profile" in the session card and "Add profile" at the end of the list, instead of a nav-bar button, a floating "+" and long press only.
    - Plain labels: Frequency, Signal, Spreading, Coding rate, Network ID, Power.
    - Dark text on sent bubbles. The chat flows in the page.
    - LoRa CW opens a page saying "Attach an antenna first", with a hold-to-start pill.
    - The editor saves with one full-width pill; a swipe right discards. "Delete profile" needs a hold.

### Phase 8c follow-ups

- **Check on hardware:** every app above (none runs in the simulator, though all compile for the Ultra), and the screen going off in an app while music plays and while a scan runs.
- **LoRa Hub editor:** a swipe right now discards unsaved edits without asking. Check that this is what people expect, or save on the way out.
- **Walkie Talkie:** the talk page is about as tall as the screen, so it may scroll a few pixels.
- **Swiping back from an app's top page:** black shows behind it while it moves; the list or face is on another tile. Showing it would mean drawing the home tile under the app tile during the drag.

### Phase 8 follow-ups

- **Check on hardware:**
  - The haptic patterns: whether the six feel different on the wrist, and whether ALERT is strong enough to wake someone.
  - File Manager with a card: file rows, the file page, View and "Delete file". The simulator has no SD card.
  - WiFi Tools and WiFi while connected (the rows that only show then).
  - BLE Scanner's list and Options with real devices.
  - The firmware build: `pio run -e twatch_ultra` stops at `ImuDrv.hpp: No such file or directory` (included by `src/LilyGoWatchUltra.h`): the SensorLib in `.pio/libdeps/twatch_ultra` is older than the one this checkout expects. Not caused by phase 8; update SensorLib, then build.
- **Long values** still wrap in a few info rows at 28 px (Monitor's "Simulator PMU" on desktop). Real chip names are short; check on hardware.
- **Apps not reviewed in this pass** (not in the simulator build): BLE HID, Music, Net Radio, Compass, Walkie, GNSS, Recorder, LoRa Hub with a module. They get the new type sizes, icon and divider rules through the shared helpers, so check their layouts on hardware.
- "32.0 C" in Monitor could say "32.0 °C" (the Latin fonts have the degree sign).

### What phases 1–2 changed

**New files:**
- `ui_watch_shell.cpp/.h`: navigation, plus idle → screen off → wake to the face.
- `ui_watch_face.cpp`: the watch face.
- `ui_app_list.cpp`: the app list.

**Shared helpers:**
- **Size tokens** in `ui_define.h`:
  - `UI_FONT_TINY / CAPTION / SMALL / BODY`
  - `UI_ROW_MIN_H`, `UI_APP_NAV_H`, `UI_APP_PAD_HOR`
  - On other boards each font token is the exact font it replaces, so swapping a hard-coded font for a token changes nothing there.
- **`ui_tools.cpp`, on the watch:**
  - App page: 44 px header with the time on the right.
  - Rows: 76 px with centred content. A title takes up to about half the row.
  - Controls: sliders on their own full-width line; bigger switches, dropdowns and buttons.
- **Power menu** (`ui_main.cpp`): full screen with three large rows. Swipe right cancels.
- **App font pass:** Monitor, Power, WiFi, BLE Scan and Clock Tools now use the tokens.

### What phase 3 changed

**New file `ui_watch_pages.cpp`** (helpers are declared in `ui_define.h`):
- **`ui_push_page(from, title)`:** slides a sub-page in from the right over the page that holds `from`. The sub-page has the same layout as `ui_create_app_page()` and returns its content. Swiping right slides it away; `ui_pop_page(content)` does the same from code.
- **`ui_create_card_picker()`:** takes the same arguments as `ui_create_card_dropdown()`.
  - On the watch the row shows the current value and a chevron ("Auto ›"). Tapping it pushes a list with a check mark on the current choice.
  - Picking an option sends `LV_EVENT_VALUE_CHANGED` and returns after 200 ms.
  - On other boards it is the dropdown.
- **`ui_create_menu_row()`:** a pill row with an icon disc, title and chevron, styled like the app list.

**Navigation:** pages can be stacked now, so a back swipe goes only to the innermost page under the finger (`ui_swipe_back_root()` in `ui_navigation.cpp`).

**Settings (`ui_sys.cpp`) on the watch:**
- Settings opens on a list. On the Ultra it shows Display, Power, Devices and About. Sound, Sensors and Input appear only on boards that have those settings.
- Each entry pushes a sub-page built by the existing build steps, so the card layout on other boards is unchanged.
- Rows have no icons. Font Size, Theme and Timeout are pickers.
- Timeout offers Never, 10 s, 15 s, 30 s, 1 min, 2 min and 3 min, instead of a slider with no readout.
- Button status rows go on the Devices page. They show only on hardware; the simulator has no button monitor.

**Theme changes:**
- The app list uses the shared styles, so it follows a new theme.
- `ui_theme_apply()` no longer adds scrollbars to scaled images such as the face's slot icons.

### What phase 4 changed

**Settings storage (`hal_interface.cpp/.h`):**
- The lock mode and the 4 face slots take 21 of the 59 spare bytes in `app_settings_storage_t`. They are byte fields only, so the blob keeps its layout and size; a `static_assert` checks this.
- Zero means "default", so settings saved before phase 4 still load.
- A slot is `{type, app_hash}`. Apps are stored by an FNV-1a hash of their name, so the order apps are registered in doesn't matter.
- Accessors: `hw_get/set_watch_lock_mode()` and `hw_get/set_watch_face_slot()`. On desktop they live in memory only, like the other settings.

**Watch face (`ui_watch_face.cpp`):**
- Slot values: None, Battery, Wi-Fi, Date, Steps, Calories, or any app.
- Calories are estimated from steps at 0.04 kcal a step.
- Battery opens Power, Wi-Fi opens WiFi and Date opens Clock Tools. Steps and Calories open nothing.
- Long-pressing a slot opens the editor straight at that slot's choices; long-pressing elsewhere opens the editor.
- After a theme change, the slot icons pick up the new accent within a second.

**Face editor and lock screen pages (new `ui_watch_face_edit.cpp`):**
- Settings now starts with "Watch face" and "Lock screen".
- Watch face: one picker per corner, listing the values first and then the apps in app-list order.
- Lock screen: "When idle" is either Screen off (the old behaviour) or Clock.
- A long press on the face opens the editor as an app of its own, so going back returns to the face.

**Lock mode "Clock" (`ui_watch_shell.cpp`, `ui_main.cpp`):**
- After the screen timeout, the watch shows the old clock page (`setupClock()`) at 80 MHz.
  - After another full timeout, the screen turns off.
  - A tap or a swipe up returns to the face.
  - Waking from screen off always returns to the face.
- On the watch, the clock page has no unlock hint. It fills the screen, sits lower, and its digits have no box-shaped shadows. The battery line uses `UI_FONT_CAPTION`. Other boards keep the old lock screen.

**Pedometer (`hal_interface.cpp`):**
- `hw_get_step_count()` returns steps since midnight. It reads the BHI260AP's step counter (`BoschSensorID::STEP_COUNTER`, on-change, 1 Hz). Desktop returns a fixed 6420.
- The counter runs only while a face slot shows steps or calories, to save power.
- Its interrupt can't wake the watch from light sleep. After each wake, `hw_low_power_loop()` reads the sensor's queued data, so the interrupt line doesn't stay high and stall later updates.
- Steps start counting from 0 when the counter is switched on or the watch boots. The count resets at midnight.

## Phase 4 follow-ups

- **Check on hardware:**
  - Steps count while walking, keep counting through screen off, and reset at midnight.
  - Light-sleep current with and without a steps slot. The BHI260AP keeps running while the counter is on.
  - The settings survive a reboot, and a watch flashed with older settings still boots to the default face.
- **Steps survive only until reboot.** Keeping today's count across a reboot would need it saved, e.g. to NVS a few times a day.
- **Corner picker length:** values plus every app make a long list (about 35 rows). Grouping apps on their own page, with icons, would be quicker to scroll.
- **Lock clock:** the page still has two breathing glow animations. They redraw large areas while the clock is shown; drop them on the watch if the lock mode costs too much battery.

## Phase 3 follow-ups

- **Pickers in other apps:** 10 app files call `ui_create_card_dropdown()`. Switching one to `ui_create_card_picker()` is a rename.
  - Callbacks that use `lv_dropdown_get_selected()` keep working.
  - So does `lv_obj_get_child(row, -1)`, which still returns the dropdown.
  - Widths that apps set on the dropdown can be dropped on the watch.
  - Apps that build `lv_dropdown` directly (the rest of the 23 files) need a closer look.
- **Charge Current** is a slider of charger levels with no readout. A picker of currents in mA would be clearer.
- **Theme picker:** the options could show a colour swatch.

## Phase 2 follow-ups

- **Font migration:** about 145 hard-coded `lv_font_montserrat_10/12/14` uses remain in 39 files. Most of those in `ui_main.cpp` are in the old launcher, which the watch doesn't use. The lock-screen page is used again for lock mode "Clock", and its battery line now uses the token.
  - Migrate the rest as each app is reviewed in the simulator.
  - For each file: replace `_10`, `_12` and `_14` with `UI_FONT_TINY`, `UI_FONT_CAPTION` and `UI_FONT_SMALL`, then fix anything that no longer fits.
  - Largest: `ui_compass` 14, `ui_wifi_analyzer` 9, `ui_nfc_emulation` 9, `ui_ina219` 8, `ui_thermal` 7, `ui_music_player` 7, `ui_sensor` 6, `ui_sd_manager` 6, `ui_lora_hub` 6.
- **Compass** isn't in the simulator build (`EXCLUDE_COMPASS` on desktop). Its dial is laid out by hand, so it needs checking on hardware after its fonts change.
- **Apps that set their own sizes:** some override sizes after using the shared helpers, e.g. `lv_obj_set_width(dd, …)` on dropdowns. Clock Tools was fixed; check the others (23 files use dropdowns), or move them to pickers (see Phase 3 follow-ups).
- **App names:** shown as registered (e.g. "DisplayTest"). Watch-friendly names could be passed to `register_app()` under the flag.

## Later

- **Screen timeout inside apps:** the screen never turns off while an app is open, because the idle timer is paused in apps, as it was before this work.
  - A global timeout could respect `set_low_power_mode_flag(false)`. Today only NFC, PN532, Music Eyes, LED Strip, Display Test and message pop-ups call it.
  - Apps that must keep running with the screen off (music player, recorder, walkie) would need to call it, or keep working while the screen is off.
- **Page transitions** that slide with the finger instead of the edge-bubble indicator.
- **Always-on display:** a dim clock kept on, which needs a timer wake-up from light sleep.
- **App list ordering and favourites.**
- **Swipe down from the face for quick settings** (brightness, Wi-Fi, BLE).
- **More apps in the simulator:** audio player/recorder, LoRaWAN, GPS, Bad USB, USB disk and the sensor/track loggers are excluded from desktop builds because they use Arduino-only APIs without a desktop branch.

## Rules for changes

- `main_screen` stays the long-lived object, because `set_low_power_mode_flag()` stores the stay-awake flag on it.
- Apps run on the single active screen, inside tile (0,1) of `main_screen`. Many msgboxes and keyboards use `lv_scr_act()`.
- `menu_show()` stays the app-exit API (53 app files call it). The shell redirects it to the list or the face.
- `ui_create_app_page()` keeps its child order (nav = 0, divider = 1, content = 2, back button = nav child 0). Content's user_data still holds the page root. `ui_nfc_emulation.cpp`, the nav auto-hide code and `ui_push_page()` depend on both.
- A pushed page is a child of the page it covers, so deleting an app page also deletes its sub-pages. Objects whose pointers outlive a sub-page, such as labels the Settings refresh timer writes to, must be cleared on `LV_EVENT_DELETE`; `ui_sys.cpp` does this with `track_value_label()`.
- A picker row's last child is the `lv_dropdown` that holds the options and the choice.
- An action pill row's last child is the button, and the button's first child is its label.
- A card row's divider is the card's next child after the row, flagged `LV_OBJ_FLAG_USER_2`. On the watch `USER_3` marks the last row's divider (hidden until another row follows) and `USER_1` one that `ui_card_row_set_hidden()` hid; apps shouldn't set those flags on dividers. Hide or show card rows with `ui_card_row_set_hidden()`, not the hidden flag, so the dividers follow.
- Button and PMU callbacks fire outside the LVGL loop. Set a flag there and act on it in an LVGL timer, as `lilygo_power_menu_request()` does.

## Testing

- **Simulator**, from the sibling `LilyGoLib-PlatformIO` checkout:
  ```bash
  pio run -e emulator_watch_ultra -t upload
  ```
  - Swipe up on the face for the app list, and swipe right to go back.
  - Settings: open a section, then a picker (e.g. Display → Theme). Each swipe right goes back one page.
  - Long-press a face corner to change it. Steps show a fixed 6420 on desktop.
  - App list: long-press a row, drag it up or down and let go. Restart the simulator; the order stays (delete `watch_app_order.bin` to reset it).
  - Lock mode: set Settings → Display → Timeout to 10 s and Settings → Lock screen to Clock. Then leave the face idle; set the timeout back afterwards.
  - Clock Tools: Date → move the wheels → Set date. The face shows the new date; restarting the simulator resets it.
  - Display Test: tap, long press, then swipe right.
  - `FACTORY_SIM_APP="WiFi" .pio/build/emulator_watch_ultra/program` opens that app at start (any app list name) and skips the touch guide. The boot splash takes about 10 s.
  - WiFi: turn it on and scroll down; Scan networks lists four made-up networks. Tap one, then Password, and type: the dots and cursor stay put.
  - F2 opens the power menu, standing in for holding PWR.
  - **Scripted swipes:** post real drag events (macOS `CGEvent`: move to the start point, `leftMouseDown`, about ten `leftMouseDragged` steps over about 200 ms, `leftMouseUp`). `cliclick` drags arrive as taps, and a drag slower than 400 ms is cancelled as a long press.
  - When the screen times out it shows a black overlay; click to wake.
- **Hardware:**
  ```bash
  pio run -e twatch_ultra -t upload
  ```
  Check that PWR, BOOT and touch wake the watch to the face, and that light-sleep current is unchanged. For phase 4, also see the hardware checks under Phase 4 follow-ups.
