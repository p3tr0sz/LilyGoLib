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
| 2 | Readable app pages and full-screen power menu | Done (`36a851d`) |
| 3 | Settings as nested lists | Done |
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
- Watch face and Lock screen entries come with phase 4.

**Theme changes:**
- The app list uses the shared styles, so it follows a new theme.
- `ui_theme_apply()` no longer adds scrollbars to scaled images such as the face's slot icons.

## Phase 3 follow-ups

- **Pickers in other apps:** 10 app files call `ui_create_card_dropdown()`. Switching one to `ui_create_card_picker()` is a rename.
  - Callbacks that use `lv_dropdown_get_selected()` keep working.
  - So does `lv_obj_get_child(row, -1)`, which still returns the dropdown.
  - Widths that apps set on the dropdown can be dropped on the watch.
  - Apps that build `lv_dropdown` directly (the rest of the 23 files) need a closer look.
- **Charge Current** is a slider of charger levels with no readout. A picker of currents in mA would be clearer.
- **Theme picker:** the options could show a colour swatch.
- **Watch face:** the slot icons keep the old accent colour until a reboot after a theme change. Fix this as part of phase 4.

## Phase 2 follow-ups

- **Font migration:** 147 hard-coded `lv_font_montserrat_10/12/14` uses remain in 39 files. The ones in `ui_main.cpp` are in the old launcher and lock-screen code, which the watch no longer uses.
  - Migrate the rest as each app is reviewed in the simulator.
  - For each file: replace `_10`, `_12` and `_14` with `UI_FONT_TINY`, `UI_FONT_CAPTION` and `UI_FONT_SMALL`, then fix anything that no longer fits.
  - Largest: `ui_compass` 14, `ui_wifi_analyzer` 9, `ui_nfc_emulation` 9, `ui_ina219` 8, `ui_thermal` 7, `ui_music_player` 7, `ui_sensor` 6, `ui_sd_manager` 6, `ui_lora_hub` 6.
- **Compass** isn't in the simulator build (`EXCLUDE_COMPASS` on desktop). Its dial is laid out by hand, so it needs checking on hardware after its fonts change.
- **Apps that set their own sizes:** some override sizes after using the shared helpers, e.g. `lv_obj_set_width(dd, …)` on dropdowns. Clock Tools was fixed; check the others (23 files use dropdowns), or move them to pickers (see Phase 3 follow-ups).
- **App names:** shown as registered (e.g. "Setting", "DisplayTest"). Watch-friendly names could be passed to `register_app()` under the flag.

## Phase 4: Configurable face and lock screen

- **Persisted settings** go in the 59 spare bytes of `app_settings_storage_t` (`hal_interface.cpp`). Spare bytes read as zero, which means "default", so existing saved settings still load.
  - `lock_mode`: 0 = Off (black, current behaviour), 1 = Clock.
  - 4 face slots, each `{type, id}`. An app is stored by a hash of its name, so the order apps are registered in doesn't matter.
  - New `hw_get/set_watch_*` functions, kept in memory on desktop.
- **Settings entries:** add "Watch face" and "Lock screen" at the top of the Settings list (`sys_sections` in `ui_sys.cpp`), using `ui_push_page()` for their pages.
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
  - F2 opens the power menu, standing in for holding PWR.
  - When the screen times out it shows a black overlay; click to wake.
- **Hardware:**
  ```bash
  pio run -e twatch_ultra -t upload
  ```
  Check that PWR, BOOT and touch wake the watch to the face, and that light-sleep current is unchanged.
