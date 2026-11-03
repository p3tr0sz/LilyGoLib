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

## Status

| Phase | What | State |
|---|---|---|
| 1 | Navigation shell: face, app list, swipe-right back, screen off | Done (`78c918b`) |
| 2 | Readable app pages and full-screen power menu | Done |
| 3 | Settings as nested lists | To do |
| 4 | Configurable face and lock screen | To do |

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

## Phase 2 follow-ups

- **Font migration:** 147 hard-coded `lv_font_montserrat_10/12/14` uses remain in 39 files. The ones in `ui_main.cpp` are in the old launcher and lock-screen code, which the watch no longer uses.
  - Migrate the rest as each app is reviewed in the simulator.
  - For each file: replace `_10`, `_12` and `_14` with `UI_FONT_TINY`, `UI_FONT_CAPTION` and `UI_FONT_SMALL`, then fix anything that no longer fits.
  - Largest: `ui_compass` 14, `ui_wifi_analyzer` 9, `ui_nfc_emulation` 9, `ui_ina219` 8, `ui_thermal` 7, `ui_music_player` 7, `ui_sensor` 6, `ui_sd_manager` 6, `ui_lora_hub` 6.
- **Compass** isn't in the simulator build (`EXCLUDE_COMPASS` on desktop). Its dial is laid out by hand, so it needs checking on hardware after its fonts change.
- **Apps that set their own sizes:** some override sizes after using the shared helpers, e.g. `lv_obj_set_width(dd, …)` on dropdowns. Clock Tools was fixed; check the others (23 files use dropdowns).
- **App names:** shown as registered (e.g. "Setting", "DisplayTest"). Watch-friendly names could be passed to `register_app()` under the flag.

## Phase 3: Settings as nested lists

- **New helper `ui_push_page(title)`:** a sub-page stacked inside the app tile. Swiping right pops it, reusing `ui_enable_edge_swipe_back()` (`ui_navigation.cpp`) with a hidden back target, as the app list does.
- **New helper `ui_create_card_picker()`:**
  - On the watch, tapping it pushes a list page with a check mark on the current choice.
  - On other boards it falls back to `ui_create_card_dropdown()`.
  - Use it in place of dropdowns, since a dropdown popup is cramped on a watch.
- **`ui_sys.cpp`:** split today's 8 cards (Display, Input, Audio, Power, Buttons, Environment, Devices, System Info) into a top-level list: Watch face, Lock screen, Display, Sound, Power, Sensors, Devices, About. Each opens a sub-list.
- **Drop the repeated gear icon** (`LV_SYMBOL_SETTINGS` on almost every row). Use a distinct icon or none.

## Phase 4: Configurable face and lock screen

- **Persisted settings** go in the 59 spare bytes of `app_settings_storage_t` (`hal_interface.cpp`). Spare bytes read as zero, which means "default", so existing saved settings still load.
  - `lock_mode`: 0 = Off (black, current behaviour), 1 = Clock.
  - 4 face slots, each `{type, id}`. An app is stored by a hash of its name, so the order apps are registered in doesn't matter.
  - New `hw_get/set_watch_*` functions, kept in memory on desktop.
- **Slot editor:** opened from Settings → Watch face, or by long-pressing the face. Tap a slot, then choose a value or an app with the Phase 3 picker.
- **Values for slots:** battery and Wi-Fi exist today. Add date, steps and calories (estimated from steps).
  - Steps needs HAL work: the Ultra's BHI260AP is only used as an IMU, so its step-counter sensor must be enabled. This can only be verified on hardware; desktop returns fixed values.
- **Lock mode "Clock":** reuse the old clock page (`setupClock()` in `ui_main.cpp`, without the unlock hint) until the display timeout, then go black.

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
- `ui_create_app_page()` keeps its child order (nav = 0, divider = 1, back button = nav child 0). Content's user_data still holds the page root. `ui_nfc_emulation.cpp` and the nav auto-hide code depend on both.
- Button and PMU callbacks fire outside the LVGL loop. Set a flag there and act on it in an LVGL timer, as `lilygo_power_menu_request()` does.

## Testing

- **Simulator**, from the sibling `LilyGoLib-PlatformIO` checkout:
  ```bash
  pio run -e emulator_watch_ultra -t upload
  ```
  - Swipe up on the face for the app list, and swipe right to go back.
  - F2 opens the power menu, standing in for holding PWR.
  - When the screen times out it shows a black overlay; click to wake.
- **Hardware:**
  ```bash
  pio run -e twatch_ultra -t upload
  ```
  Check that PWR, BOOT and touch wake the watch to the face, and that light-sleep current is unchanged.
