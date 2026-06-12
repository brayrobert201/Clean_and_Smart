#include <pebble.h>
#pragma once

#define KEY_WEATHER_CODE 0
#define KEY_WEATHER_TEMP 1
#define KEY_JSREADY 2

#define KEY_TEMPERATURE_FORMAT 3
#define KEY_HOURS_MINUTES_SEPARATOR 4
#define KEY_DATE_FORMAT 5
#define KEY_BLUETOOTH_ALERT 7
#define KEY_LANGUAGE 10
#define KEY_TEXT_COLOR 11
#define KEY_BG_COLOR 12
#define KEY_TRAIN_DEPARTURE 13
#define KEY_TRAIN_PLATFORM 14
#define KEY_TRAIN_DIRECTION 15
#define KEY_TRAIN_DEPARTURE2 16
#define KEY_TRAIN_PLATFORM2 17
#define KEY_TRAIN_EXPRESS 18
#define KEY_PEAK1_START 19
#define KEY_PEAK1_END 20
#define KEY_PEAK2_START 21
#define KEY_PEAK2_END 22
#define KEY_PEAK_INTERVAL 23
#define KEY_OFFPEAK_INTERVAL 24

// Claude subscription usage (emery face only); reset values are epoch seconds
#define KEY_USAGE_5H_PCT 40
#define KEY_USAGE_5H_RESET 41
#define KEY_USAGE_7D_PCT 42
#define KEY_USAGE_7D_RESET 43
#define KEY_USAGE_STALE 44

// Claude usage display config (emery face only)
#define KEY_USAGE_BAND_MODE    36
#define KEY_USAGE_DISPLAY_MODE 37
#define KEY_USAGE_PACE_OFFSET  38
#define KEY_USAGE_ABS_WARN     39
#define KEY_USAGE_COLOR_GOOD   47
#define KEY_USAGE_COLOR_OVER   48
#define KEY_USAGE_COLOR_CRIT   49

#ifdef PBL_RECT
#define ICON_WIDTH 40
#define ICON_HEIGHT 20
#else
#define ICON_WIDTH 36
#define ICON_HEIGHT 18
#endif

#define BLUETOOTH_ALERT_DISABLED 0
#define BLUETOOTH_ALERT_SILENT 1
#define BLUETOOTH_ALERT_WEAK 2
#define BLUETOOTH_ALERT_NORMAL 3
#define BLUETOOTH_ALERT_STRONG 4
#define BLUETOOTH_ALERT_DOUBLE 5

// define macro comparing PBL_DISPLAY_HEIGHT with 168
#ifdef PBL_RECT
#if PBL_DISPLAY_HEIGHT == 168
  #define PBL_IF_HEIGHT_168_ELSE(expr_if_true, expr_if_false) (expr_if_true)
#else
  #define PBL_IF_HEIGHT_168_ELSE(expr_if_true, expr_if_false) (expr_if_false)
#endif
#else
#define PBL_IF_HEIGHT_168_ELSE(expr_if_true, expr_if_false) (expr_if_true)
#endif

// bluetooth vibe patterns
const VibePattern VIBE_PATTERN_WEAK = {
	.durations = (uint32_t[]){100},
	.num_segments = 1};

const VibePattern VIBE_PATTERN_NORMAL = {
	.durations = (uint32_t[]){300},
	.num_segments = 1};

const VibePattern VIBE_PATTERN_STRONG = {
	.durations = (uint32_t[]){500},
	.num_segments = 1};

const VibePattern VIBE_PATTERN_DOUBLE = {
	.durations = (uint32_t[]){500, 100, 500},
	.num_segments = 3};
