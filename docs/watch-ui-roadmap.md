# T-Watch Ultra: smartwatch UI roadmap

This fork changes the factory app on the **T-Watch Ultra** (502×410, about 315 ppi) so it behaves like a Xiaomi or Apple watch, not a small tablet. Other boards keep the original launcher.

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
| 5 | One-gesture shared controls: level pages, hold to confirm, whole-row switches, full-screen messages | Done |
| 6 | One-gesture app controls: pickers, action pills, Clock Tools time page, Display Test | Planned |
| 7 | No typing: presets instead of keyboards | Planned |

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
- **App sliders:** volume in Player, Net Radio and Recorder, and the Mi Light Bar sliders, open the level page from phase 5.

### Phase 7: no typing

- **WiFi Tools host:** a list of presets (gateway, 1.1.1.1, 8.8.8.8, example.com, last used).
- **QRCode:** content from templates only (device info, Wi-Fi, URL); the free-text field goes on other boards.
- **WiFi password:** keep the keyboard, but full screen with large keys. Check whether provisioning from a phone (BLE or SoftAP) is worth it.
- **LoRa Hub, Walkie, IR:** review each text field; replace with presets or numeric pickers where possible.

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
- Button and PMU callbacks fire outside the LVGL loop. Set a flag there and act on it in an LVGL timer, as `lilygo_power_menu_request()` does.

## Testing

- **Simulator**, from the sibling `LilyGoLib-PlatformIO` checkout:
  ```bash
  pio run -e emulator_watch_ultra -t upload
  ```
  - Swipe up on the face for the app list, and swipe right to go back.
  - Settings: open a section, then a picker (e.g. Display → Theme). Each swipe right goes back one page.
  - Long-press a face corner to change it. Steps show a fixed 6420 on desktop.
  - Lock mode: set Settings → Display → Timeout to 10 s and Settings → Lock screen to Clock. Then leave the face idle; set the timeout back afterwards.
  - F2 opens the power menu, standing in for holding PWR.
  - When the screen times out it shows a black overlay; click to wake.
- **Hardware:**
  ```bash
  pio run -e twatch_ultra -t upload
  ```
  Check that PWR, BOOT and touch wake the watch to the face, and that light-sleep current is unchanged. For phase 4, also see the hardware checks under Phase 4 follow-ups.
