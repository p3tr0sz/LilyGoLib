/**
 * @file      main.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-08
 *
 */

#ifndef ARDUINO
#include <LilyGoLog.h>
#include <stdio.h>
#include "lvgl.h"
#include <unistd.h>
#define SDL_MAIN_HANDLED        /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "demos/lv_demos.h"
#include "examples/lv_examples.h"

#include "hal_interface.h"

extern void setupGui();
extern void hw_init();

static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;


#if FACTORY_WATCH_UI
extern "C" void lilygo_power_menu_request(void);

/* Simulator stand-in for holding the PWR button: F2 opens the power menu. */
static int sdl_power_key_watch(void *userdata, SDL_Event *event)
{
    LV_UNUSED(userdata);
    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_F2 && !event->key.repeat) {
        lilygo_power_menu_request();
    }
    return 1;
}
#endif

#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf)
{
    LV_UNUSED(level);
    LV_UNUSED(buf);
    LILYGO_LOG_PRINTF("%s", buf);
}
#endif


void hal_setup(void)
{
    // Workaround for sdl2 `-m32` crash
    // https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1775067/comments/7
#ifndef WIN32
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
#endif

#if LV_USE_LOG != 0
    lv_log_register_print_cb(lv_log_print_g_cb);
#endif

    /* Add a display
     * Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/


    lvDisplay = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    lvMouse = lv_sdl_mouse_create();
    lvMouseWheel = lv_sdl_mousewheel_create();
    lvKeyboard = lv_sdl_keyboard_create();
#if FACTORY_WATCH_UI
    SDL_AddEventWatch(sdl_power_key_watch, NULL);
#endif
}

void hal_loop(void)
{
    Uint32 lastTick = SDL_GetTicks();
    while (1) {
        fflush(stdout);
        SDL_Delay(5);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick); // Update the tick timer. Tick is new for LVGL 9
        lastTick = current;
        lv_timer_handler(); // Update the UI-
    }
}


extern "C" int main(void)
{
    lv_init();

    hal_setup();
    LILYGO_LOG_PRINTF("hello lvgl\n");
    //****************** */
    hw_init();
    setupGui();


    //****************** */
    hal_loop();
}
#endif
