/**
 * lv_conf.h for the TB-4117 verification app (LVGL 9.2)
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_MEM_SIZE (48UL * 1024UL * 1024UL)

#define LV_USE_OS LV_OS_NONE

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* display / input drivers */
#define LV_USE_LINUX_DRM 1
#define LV_USE_EVDEV 1
#define LV_USE_LIBINPUT 0

/* widgets used by the app */
#define LV_USE_LABEL 1
#define LV_USE_IMAGE 1
#define LV_USE_FREETYPE 1
#if LV_USE_FREETYPE
    #define LV_FREETYPE_USE_LVGL_PORT 0
    #define LV_FREETYPE_CACHE_FREETYPE_ENGINE_SIZE 0
    #define LV_FREETYPE_CACHE_FREETYPE_SBIT_CACHE 0
    #define LV_FREETYPE_CACHE_SIZE (256 * 1024)
    #define LV_FREETYPE_CACHE_IMG_CACHE 16
    #define LV_FREETYPE_CACHE_FONT_CACHE 32
#endif
#define LV_USE_SLIDER 1
#define LV_USE_KEYBOARD 1
#define LV_USE_TEXTAREA 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_SIMSUN_16_CJK 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_INDEV_DEF_LONG_PRESS_TIME 800

#define LV_USE_THEME_DEFAULT 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif /* LV_CONF_H */
