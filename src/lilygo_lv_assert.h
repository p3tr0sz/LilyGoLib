/**
 * @file      lilygo_lv_assert.h
 * @license   MIT
 *
 * LVGL calls lilygo_lv_assert_failed() when one of its checks fails (a NULL
 * object, a failed allocation). The library's default halts, as LVGL's own
 * default does; an application can define its own to recover instead (the
 * factory app records the open app and restarts).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void lilygo_lv_assert_failed(void);

#ifdef __cplusplus
}
#endif
