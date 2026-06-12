#include <pebble.h>
#include "main.h"
#include <pebble-effect-layer/pebble-effect-layer.h>
#include "languages.h"

Window *my_window;
Layer *window_layer;

// emery (tall rect) shows two departure lines; 168-rect and chalk show one
#if defined(PBL_RECT) && (PBL_DISPLAY_HEIGHT != 168)
#define TWO_LINE_TRAIN 1
#endif

// compact direction glyphs for single-line displays (present in both Big Noodle TTFs)
#define GLYPH_TO_WORK "\xE2\x80\xBA" // ›
#define GLYPH_TO_HOME "\xE2\x80\xB9" // ‹

// direction icon sprite cells, indexed by train_direction: 0 = work (building), 1 = home (house)
#define DIR_ICON_W 26
#define DIR_ICON_H 26

TextLayer *text_time, *text_date, *text_dow, *text_battery, *text_temp;
Layer *graphics_layer;
BitmapLayer *temp_layer;
GBitmap *meteoicons_all, *meteoicon_current;

#ifdef TWO_LINE_TRAIN
TextLayer *text_train;
BitmapLayer *direction_layer;
GBitmap *direction_icons_all = NULL, *direction_icon_current = NULL;
GFont bn_train;
#endif

GFont bn_69, bn_30, bn_26, bn_20, bn_19;

char s_date[] = "21  FEB  2015     "; // test
char s_time[] = "88.44mm";            // test
char s_dow[] = "WEDNESDAY     ";      // test
char s_battery[] = "100%";            // test
char s_temp[] = "-100°";

static uint32_t train_dep[2] = {0, 0}; // epoch seconds of next two trains, 0 = none
static uint8_t train_direction = 0;    // 0 = to work (›), 1 = to home (‹)
static uint8_t train_express = 0;      // bitmask: bit0 = dep1 fastest, bit1 = dep2 fastest
static char s_train_platform[2][8];
static char s_train1[32]; // emery: both departures on one row; else: single glyph line

// refresh schedule (defaults give the previous all-day 15-min behaviour until JS pushes config)
static uint8_t peak1_start = 7, peak1_end = 9, peak2_start = 16, peak2_end = 19;
static uint8_t peak_interval = 15, offpeak_interval = 15;

#ifdef PBL_PLATFORM_EMERY
// Claude subscription usage, pushed from the phone. The two _pct values are the quota used
// (0-100); the two _reset values are epoch seconds when each window rolls over, from which the
// watch derives the "time remaining" bars itself every minute. reset == 0 means "no data yet".
static uint8_t usage_5h_pct = 0, usage_7d_pct = 0;
static uint32_t usage_5h_reset = 0, usage_7d_reset = 0;
static bool usage_stale = false;
static uint32_t usage_last_change = 0;  // epoch when quota pct last changed
static bool usage_band_shows_bars = false;
#define PERSIST_USAGE_LAST_CHANGE 46
#define USAGE_FRESH_SECS 7200           // show bars whenever quota changed in the last 2 hours
static uint8_t flag_usage_band_mode    = 0;   // 0=smart, 1=always bars, 2=always alt
static uint8_t flag_usage_display_mode = 0;   // 0=bars, 1=text pct
static uint8_t flag_usage_pace_offset  = 5;   // amber gate: used > elapsed + N
static uint8_t flag_usage_abs_warn     = 10;  // amber gate: used >= N (0=disabled)
static int     flag_usage_color_good   = 0x00FF00;
static int     flag_usage_color_over   = 0xFFAA00;
static int     flag_usage_color_crit   = 0xFF0000;
static bool s_usage_phase = false;
static AppTimer *s_usage_timer = NULL;
static void update_usage_band(void);    // forward declaration (defined near draw_usage)
#endif

EffectLayer *zoom_layer_time, *zoom_layer_meteoicon;

uint8_t flag_hoursMinutesSeparator, flag_dateFormat, flag_bluetooth_alert, flag_language;
int flag_textColor, flag_bgColor;
bool flag_messaging_is_busy = false, flag_js_is_ready = false;

GRect bounds;
GPoint center;

static GRect get_time_frame()
{
#ifdef PBL_RECT
  return GRect(0, 53 * PBL_DISPLAY_HEIGHT / 168 + PBL_IF_HEIGHT_168_ELSE(0, 6), bounds.size.w, 70 * PBL_DISPLAY_HEIGHT / 168);
#else
  return GRect(0, 38, bounds.size.w, 70);
#endif
}

static void set_time_frame(GRect frame)
{
  layer_set_frame(text_layer_get_layer(text_time), frame);

  if (zoom_layer_time)
  {
    effect_layer_set_frame(zoom_layer_time, frame);
  }
}

static void set_time_frame_for_unobstructed_area(GRect free_area)
{
  GRect frame = get_time_frame();
  int16_t obstruction_height = bounds.size.h - free_area.size.h;

  if (obstruction_height < 0)
  {
    obstruction_height = 0;
  }

  frame.origin.y -= obstruction_height * 15 / 100;
  set_time_frame(frame);
}

// // {*********************** THIS BLOCK PROPERLY RESTORES EFFECT LAYER AFTER A NOTIFICATION IS DISMISSED

// // when app got focus - restore and refresh window - that makes it dynamic again
// static void app_focus_changed(bool focused) {
//   if (focused && effect_layer) {
//      layer_set_hidden(window_layer, false);
//      layer_mark_dirty(window_layer);
//   }

// }

// // when app is about to regain focus - hide main window - this restores static pic of previous screen appear
// static void app_focus_changing(bool focused) {
//   if (focused && effect_layer) {
//      layer_set_hidden(window_layer, true);
//   }

// }
// // *********************** }

// asking the phone for fresh weather + train data
static void request_phone_data()
{
  // Only grab the weather if we can talk to phone AND weather is enabled AND currently message is not being processed and JS on phone is ready
  if (bluetooth_connection_service_peek() && !flag_messaging_is_busy && flag_js_is_ready)
  {
    // APP_LOG(APP_LOG_LEVEL_INFO, "**** I am inside 'request_phone_data()' about to request data from the phone ***");

    // need to have some data - sending dummy
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    Tuplet dictionary[] = {
        TupletInteger(0, 0),
    };
    dict_write_tuplet(iter, &dictionary[0]);

    flag_messaging_is_busy = true;
    app_message_outbox_send();
  }
}

// showing temp
static void show_temperature(int w_current)
{
  // APP_LOG(APP_LOG_LEVEL_INFO, "**** I am inside 'show_temperature()'; TEMP in Pebble: %d", w_current);
  static char buffer[6];
  snprintf(buffer, sizeof(buffer), "%i\u00B0", w_current);
  text_layer_set_text(text_temp, buffer);
}

// showing weather icon
static void show_icon(int w_icon)
{
  if (meteoicon_current)
    gbitmap_destroy(meteoicon_current);
  meteoicon_current = gbitmap_create_as_sub_bitmap(meteoicons_all, GRect(0, ICON_HEIGHT * w_icon, ICON_WIDTH, ICON_HEIGHT));
  bitmap_layer_set_bitmap(temp_layer, meteoicon_current);
}

// recolours a palettised bitmap's non-transparent entries to the current text color
static void tint_bitmap(GBitmap *bmp) {
  if (!bmp) return;
  GColor *palette = gbitmap_get_palette(bmp);
  if (!palette) return;

  int num_colors;
  switch (gbitmap_get_format(bmp)) {
    case GBitmapFormat1BitPalette: num_colors = 2; break;
    case GBitmapFormat2BitPalette: num_colors = 4; break;
    case GBitmapFormat4BitPalette: num_colors = 16; break;
    default: return;
  }

  GColor new_color = GColorFromHEX(flag_textColor);
  for (int i = 0; i < num_colors; i++) {
    if (!gcolor_equal(palette[i], GColorClear)) {
      palette[i] = new_color;
    }
  }
}

static void tint_meteoicon() {
  tint_bitmap(meteoicons_all);
  layer_mark_dirty(bitmap_layer_get_layer(temp_layer));
}

// showing day of week (used when no train countdown is available)
static void render_dow(struct tm *tick_time)
{
  if (flag_language != LANG_DEFAULT)
  { // if custom language is set - pull from language array
    text_layer_set_text(text_dow, LANG_DAY[flag_language][tick_time->tm_wday]);
  }
  else
  {
    strftime(s_dow, sizeof(s_dow), "%A", tick_time);
    text_layer_set_text(text_dow, s_dow);
  }
}

// shifts the second departure into the first slot (used when slot 1 is empty or has passed)
static void promote_train(void)
{
  train_dep[0] = train_dep[1];
  strncpy(s_train_platform[0], s_train_platform[1], sizeof(s_train_platform[0]));
  train_express >>= 1;
  train_dep[1] = 0;
  s_train_platform[1][0] = '\0';
}

// formats a departure epoch as a local clock time ("8:35"), respecting 12/24h + separator
static void format_dep_time(uint32_t epoch, char *buf, size_t bufsize)
{
  time_t t = (time_t)epoch;
  struct tm *lt = localtime(&t);

  char fmt[6];
  if (clock_is_24h_style())
    strcpy(fmt, "%H:%M");
  else
    strcpy(fmt, "%l:%M"); // leading space in 12h mode
  if (flag_hoursMinutesSeparator == 1)
    fmt[2] = '.';

  char tmp[8];
  strftime(tmp, sizeof(tmp), fmt, lt);
  char *p = (tmp[0] == ' ') ? &tmp[1] : tmp; // trim 12h leading space
  strncpy(buf, p, bufsize - 1);
  buf[bufsize - 1] = '\0';
}

// toggles between the train display and the day-of-week fallback (only on the emery two-line path)
static void show_train_layers(bool train)
{
#ifdef TWO_LINE_TRAIN
  layer_set_hidden(text_layer_get_layer(text_dow), train);
  layer_set_hidden(text_layer_get_layer(text_train), !train);
  layer_set_hidden(bitmap_layer_get_layer(direction_layer), !train);
#else
  (void)train;
#endif
}

#ifdef TWO_LINE_TRAIN
// builds one departure cell without the direction glyph: "8:35 P1*"
static void format_dep_cell(int slot, char *buf, size_t bufsize)
{
  char tbuf[8];
  format_dep_time(train_dep[slot], tbuf, sizeof(tbuf));
  snprintf(buf, bufsize, "%s %s%s",
           tbuf, s_train_platform[slot],
           (train_express & (1 << slot)) ? "*" : "");
}
#else
// builds one departure line with a direction glyph: "› 8:35 P1*"
static void format_train_line(int slot, char *buf, size_t bufsize)
{
  char tbuf[8];
  format_dep_time(train_dep[slot], tbuf, sizeof(tbuf));
  snprintf(buf, bufsize, "%s %s %s%s",
           train_direction ? GLYPH_TO_HOME : GLYPH_TO_WORK,
           tbuf, s_train_platform[slot],
           (train_express & (1 << slot)) ? "*" : "");
}
#endif

// shows the next train departure clock time(s), falling back to day of week when no usable data
static void update_train_display(struct tm *tick_time)
{
  int now = (int)time(NULL);

  // promote a stranded second departure (e.g. first slot empty after restore)
  if (train_dep[0] == 0 && train_dep[1] != 0)
    promote_train();

  // drop the first departure once it has passed and ask the phone for a fresh pair
  if (train_dep[0] != 0 && ((int)train_dep[0] - now) < 0)
  {
    promote_train();
    request_phone_data();
  }

  bool usable = train_dep[0] != 0 && (((int)train_dep[0] - now + 30) / 60) <= 180;

  if (!usable)
  { // no sensible train (unconfigured / overnight / weekend gap)
    show_train_layers(false);
    render_dow(tick_time);
    return;
  }

  show_train_layers(true);

#ifdef TWO_LINE_TRAIN
  // direction icon (building = cityward, house = homeward), shown once for the block
  if (direction_icon_current)
    gbitmap_destroy(direction_icon_current);
  direction_icon_current = gbitmap_create_as_sub_bitmap(
      direction_icons_all, GRect(0, DIR_ICON_H * (train_direction ? 1 : 0), DIR_ICON_W, DIR_ICON_H));
  bitmap_layer_set_bitmap(direction_layer, direction_icon_current);

  // both departures on one row: "8:35 P1  8:40 P1"
  char cell[12];
  format_dep_cell(0, s_train1, sizeof(s_train1));
  if (train_dep[1] != 0)
  {
    format_dep_cell(1, cell, sizeof(cell));
    strncat(s_train1, "  ", sizeof(s_train1) - strlen(s_train1) - 1);
    strncat(s_train1, cell, sizeof(s_train1) - strlen(s_train1) - 1);
  }
  text_layer_set_text(text_train, s_train1);
#else
  format_train_line(0, s_train1, sizeof(s_train1));
  text_layer_set_text(text_dow, s_train1);
#endif
}

// true during either configured commuter peak window
static bool in_peak(struct tm *t)
{
  return (t->tm_hour >= peak1_start && t->tm_hour < peak1_end) ||
         (t->tm_hour >= peak2_start && t->tm_hour < peak2_end);
}

// handling time
void tick_handler(struct tm *tick_time, TimeUnits units_changed)
{

  char format[6];

  // building format 12h/24h
  if (clock_is_24h_style())
  {
    strcpy(format, "%H:%M"); // e.g "14:46"
  }
  else
  {
    strcpy(format, "%l:%M"); // e.g " 2:46" -- with leading space
  }

  // if separator is dot = replacing colon with it
  if (flag_hoursMinutesSeparator == 1)
    format[2] = '.';

  if (units_changed & MINUTE_UNIT)
  { // on minutes change - change time

    strftime(s_time, sizeof(s_time), format, tick_time);

    if (s_time[0] == ' ')
    { // if in 12h mode we have leading space in time - don't display it (it will screw centering of text) start with next char
      text_layer_set_text(text_time, &s_time[1]);
    }
    else
    {
      text_layer_set_text(text_time, s_time);
    }

    uint8_t interval = in_peak(tick_time) ? peak_interval : offpeak_interval;
    if (interval == 0)
      interval = 15; // guard against a bad/unset config value
    if (!(tick_time->tm_min % interval))
    { // on the configured cadence - ask the phone for fresh train (and possibly weather) data
      request_phone_data();
    }

    // refresh the departure display every minute (clock crossing a departure triggers a shift)
    update_train_display(tick_time);

#ifdef PBL_PLATFORM_EMERY
    update_usage_band();
#endif
  }

  if (units_changed & DAY_UNIT)
  { // on day change - change date (format depends on flag)

    switch (flag_dateFormat)
    {
    case 0:
      if (flag_language == LANG_RUSSIAN || flag_language == LANG_POLISH)
      {                                                             // if this is Russian - need double bytes
        strftime(s_date, sizeof(s_date), "%b   -%d-%Y", tick_time); // "DEC 10 2015"
        strncpy(&s_date[0], LANG_MONTH[flag_language][tick_time->tm_mon], 6);
      }
      else
      {
        strftime(s_date, sizeof(s_date), "%b-%d-%Y", tick_time); // "DEC 10 2015"
        if (flag_language != LANG_DEFAULT)
        { // if custom language is set - pull from language array
          strncpy(&s_date[0], LANG_MONTH[flag_language][tick_time->tm_mon], 3);
        }
      }

      break;
    case 1:
      if (flag_language == LANG_RUSSIAN)
      {                                                             // if this is Russian - need double bytes
        strftime(s_date, sizeof(s_date), "%d-%b   -%Y", tick_time); // "DEC 10 2015"
        strncpy(&s_date[3], LANG_MONTH[flag_language][tick_time->tm_mon], 6);
      }
      else
      {
        strftime(s_date, sizeof(s_date), "%d-%b-%Y", tick_time); // "DEC 10 2015"
        if (flag_language != LANG_DEFAULT)
        { // if custom language is set - pull from language array
          strncpy(&s_date[3], LANG_MONTH[flag_language][tick_time->tm_mon], 3);
        }
      }

      break;
    case 2:
      strftime(s_date, sizeof(s_date), "%Y-%m-%d", tick_time); // "2015-12-10"
      break;
    }

    text_layer_set_text(text_date, s_date);

    update_train_display(tick_time);
  }
}

void load_fonts()
{

  fonts_unload_custom_font(bn_69);
  fonts_unload_custom_font(bn_19);

  if (flag_language == LANG_RUSSIAN)
  {
    bn_69 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_BIG_NOODLE_69));
    bn_19 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_19, RESOURCE_ID_BIG_NOODLE_26)));
  }
  else
  {
    bn_69 = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_BIG_NOODLE_ENG_69));
    bn_19 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_ENG_19, RESOURCE_ID_BIG_NOODLE_ENG_26)));
  }

#ifdef PBL_RECT
  fonts_unload_custom_font(bn_30);
  fonts_unload_custom_font(bn_26);

  if (flag_language == LANG_RUSSIAN)
  {
    bn_30 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_30, RESOURCE_ID_BIG_NOODLE_41)));
    bn_26 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_26, RESOURCE_ID_BIG_NOODLE_35)));
  } 
  else
  {
    bn_30 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_ENG_30, RESOURCE_ID_BIG_NOODLE_ENG_41)));
    bn_26 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_ENG_26, RESOURCE_ID_BIG_NOODLE_ENG_35)));
  }

#ifdef TWO_LINE_TRAIN
  // slightly larger font for the single departures row (30px)
  fonts_unload_custom_font(bn_train);
  bn_train = fonts_load_custom_font(resource_get_handle(
      flag_language == LANG_RUSSIAN ? RESOURCE_ID_BIG_NOODLE_30 : RESOURCE_ID_BIG_NOODLE_ENG_30));
#endif

#else
  fonts_unload_custom_font(bn_20);

  if (flag_language == LANG_RUSSIAN)
  {
    bn_20 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_26, RESOURCE_ID_BIG_NOODLE_ENG_35)));
  }
  else
  {
    bn_20 = fonts_load_custom_font(resource_get_handle(PBL_IF_HEIGHT_168_ELSE(RESOURCE_ID_BIG_NOODLE_ENG_26, RESOURCE_ID_BIG_NOODLE_ENG_35)));
  }
#endif
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context)
{
  // APP_LOG(APP_LOG_LEVEL_INFO, "INBOX RECEIVED");

  // Read first item
  Tuple *t = dict_read_first(iterator);

  bool need_weather = 0;
  bool need_time = 0;
  bool need_train_redraw = 0;
  bool need_usage_redraw = 0;

  // For all items
  while (t != NULL)
  {
    // Which key was received?
    switch (t->key)
    {

    // weather data keys
    case KEY_WEATHER_TEMP:
      persist_write_int(KEY_WEATHER_TEMP, t->value->int32);
      show_temperature(t->value->int32);
      break;
    case KEY_WEATHER_CODE:
      persist_write_int(KEY_WEATHER_CODE, t->value->int32);
      show_icon(t->value->int32);
      break;
    case KEY_JSREADY:
      // JS ready lets get the weather
      if (t->value->int16)
      {
        // APP_LOG(APP_LOG_LEVEL_INFO, "***** I am inside of 'inbox_received_callback()' message 'JS is ready' received !");
        flag_js_is_ready = true;
        need_weather = 1;
      }
      break;

    // train data keys
    case KEY_TRAIN_DEPARTURE:
      train_dep[0] = t->value->uint32;
      persist_write_int(KEY_TRAIN_DEPARTURE, t->value->int32);
      need_train_redraw = 1;
      break;
    case KEY_TRAIN_PLATFORM:
      strncpy(s_train_platform[0], t->value->cstring, sizeof(s_train_platform[0]) - 1);
      s_train_platform[0][sizeof(s_train_platform[0]) - 1] = '\0';
      persist_write_string(KEY_TRAIN_PLATFORM, s_train_platform[0]);
      need_train_redraw = 1;
      break;
    case KEY_TRAIN_DIRECTION:
      train_direction = t->value->uint8;
      persist_write_int(KEY_TRAIN_DIRECTION, t->value->uint8);
      need_train_redraw = 1;
      break;
    case KEY_TRAIN_DEPARTURE2:
      train_dep[1] = t->value->uint32;
      persist_write_int(KEY_TRAIN_DEPARTURE2, t->value->int32);
      need_train_redraw = 1;
      break;
    case KEY_TRAIN_PLATFORM2:
      strncpy(s_train_platform[1], t->value->cstring, sizeof(s_train_platform[1]) - 1);
      s_train_platform[1][sizeof(s_train_platform[1]) - 1] = '\0';
      persist_write_string(KEY_TRAIN_PLATFORM2, s_train_platform[1]);
      need_train_redraw = 1;
      break;
    case KEY_TRAIN_EXPRESS:
      train_express = t->value->uint8;
      persist_write_int(KEY_TRAIN_EXPRESS, t->value->uint8);
      need_train_redraw = 1;
      break;

    // refresh-schedule keys (watch drives the polling cadence)
    case KEY_PEAK1_START:
      peak1_start = t->value->uint8;
      persist_write_int(KEY_PEAK1_START, t->value->uint8);
      break;
    case KEY_PEAK1_END:
      peak1_end = t->value->uint8;
      persist_write_int(KEY_PEAK1_END, t->value->uint8);
      break;
    case KEY_PEAK2_START:
      peak2_start = t->value->uint8;
      persist_write_int(KEY_PEAK2_START, t->value->uint8);
      break;
    case KEY_PEAK2_END:
      peak2_end = t->value->uint8;
      persist_write_int(KEY_PEAK2_END, t->value->uint8);
      break;
    case KEY_PEAK_INTERVAL:
      peak_interval = t->value->uint8;
      persist_write_int(KEY_PEAK_INTERVAL, t->value->uint8);
      break;
    case KEY_OFFPEAK_INTERVAL:
      offpeak_interval = t->value->uint8;
      persist_write_int(KEY_OFFPEAK_INTERVAL, t->value->uint8);
      break;

#ifdef PBL_PLATFORM_EMERY
    // Claude usage keys (emery face)
    case KEY_USAGE_5H_PCT: {
      uint8_t v = t->value->uint8;
      if (v != usage_5h_pct) {
        usage_last_change = (uint32_t)time(NULL);
        persist_write_int(PERSIST_USAGE_LAST_CHANGE, (int32_t)usage_last_change);
      }
      usage_5h_pct = v;
      persist_write_int(KEY_USAGE_5H_PCT, usage_5h_pct);
      need_usage_redraw = 1;
      break;
    }
    case KEY_USAGE_5H_RESET: {
      uint32_t v = t->value->uint32;
      if (v != usage_5h_reset) {
        usage_last_change = (uint32_t)time(NULL);
        persist_write_int(PERSIST_USAGE_LAST_CHANGE, (int32_t)usage_last_change);
      }
      usage_5h_reset = v;
      persist_write_int(KEY_USAGE_5H_RESET, (int32_t)usage_5h_reset);
      need_usage_redraw = 1;
      break;
    }
    case KEY_USAGE_7D_PCT: {
      uint8_t v = t->value->uint8;
      if (v != usage_7d_pct) {
        usage_last_change = (uint32_t)time(NULL);
        persist_write_int(PERSIST_USAGE_LAST_CHANGE, (int32_t)usage_last_change);
      }
      usage_7d_pct = v;
      persist_write_int(KEY_USAGE_7D_PCT, usage_7d_pct);
      need_usage_redraw = 1;
      break;
    }
    case KEY_USAGE_7D_RESET: {
      uint32_t v = t->value->uint32;
      if (v != usage_7d_reset) {
        usage_last_change = (uint32_t)time(NULL);
        persist_write_int(PERSIST_USAGE_LAST_CHANGE, (int32_t)usage_last_change);
      }
      usage_7d_reset = v;
      persist_write_int(KEY_USAGE_7D_RESET, (int32_t)usage_7d_reset);
      need_usage_redraw = 1;
      break;
    }
    case KEY_USAGE_STALE:
      usage_stale = t->value->uint8 ? true : false;
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_BAND_MODE:
      flag_usage_band_mode = t->value->uint8;
      persist_write_int(KEY_USAGE_BAND_MODE, flag_usage_band_mode);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_DISPLAY_MODE:
      flag_usage_display_mode = t->value->uint8;
      persist_write_int(KEY_USAGE_DISPLAY_MODE, flag_usage_display_mode);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_PACE_OFFSET:
      flag_usage_pace_offset = t->value->uint8;
      persist_write_int(KEY_USAGE_PACE_OFFSET, flag_usage_pace_offset);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_ABS_WARN:
      flag_usage_abs_warn = t->value->uint8;
      persist_write_int(KEY_USAGE_ABS_WARN, flag_usage_abs_warn);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_COLOR_GOOD:
      flag_usage_color_good = t->value->int32;
      persist_write_int(KEY_USAGE_COLOR_GOOD, flag_usage_color_good);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_COLOR_OVER:
      flag_usage_color_over = t->value->int32;
      persist_write_int(KEY_USAGE_COLOR_OVER, flag_usage_color_over);
      need_usage_redraw = 1;
      break;
    case KEY_USAGE_COLOR_CRIT:
      flag_usage_color_crit = t->value->int32;
      persist_write_int(KEY_USAGE_COLOR_CRIT, flag_usage_color_crit);
      need_usage_redraw = 1;
      break;
#endif

      // config keys
    case KEY_TEMPERATURE_FORMAT: // if temp format changed from F to C or back - need re-request weather
      // APP_LOG(APP_LOG_LEVEL_INFO, "***** I am inside of 'inbox_received_callback()' switching temp format");
      need_weather = 1;
      break;
    case KEY_HOURS_MINUTES_SEPARATOR:
      if (t->value->int32 != flag_hoursMinutesSeparator)
      {
        persist_write_int(KEY_HOURS_MINUTES_SEPARATOR, t->value->int32);
        flag_hoursMinutesSeparator = t->value->int32;
        need_time = 1;
      }
      break;
    case KEY_DATE_FORMAT:
      if (t->value->int32 != flag_dateFormat)
      {
        persist_write_int(KEY_DATE_FORMAT, t->value->int32);
        flag_dateFormat = t->value->int32;
        need_time = 1;
      }
      break;
    case KEY_BLUETOOTH_ALERT:
      if (flag_bluetooth_alert != t->value->uint8)
      {
        persist_write_int(KEY_BLUETOOTH_ALERT, t->value->uint8);
        flag_bluetooth_alert = t->value->uint8;
        layer_mark_dirty(graphics_layer);
      }
      break;
    case KEY_LANGUAGE:
      if (t->value->int32 != flag_language)
      {
        persist_write_int(KEY_LANGUAGE, t->value->int32);
        flag_language = t->value->int32;
        load_fonts();
        need_time = 1;
      }
      break;
    case KEY_TEXT_COLOR:
      if (t->value->int32 != flag_textColor)
      {
        persist_write_int(KEY_TEXT_COLOR, t->value->int32);
        flag_textColor = t->value->int32;
        tint_meteoicon();
        text_layer_set_text_color(text_time,    GColorFromHEX(flag_textColor));
        text_layer_set_text_color(text_date,    GColorFromHEX(flag_textColor));
        text_layer_set_text_color(text_dow,     GColorFromHEX(flag_textColor));
        text_layer_set_text_color(text_battery, GColorFromHEX(flag_textColor));
        text_layer_set_text_color(text_temp,    GColorFromHEX(flag_textColor));
#ifdef TWO_LINE_TRAIN
        text_layer_set_text_color(text_train,   GColorFromHEX(flag_textColor));
        tint_bitmap(direction_icons_all);
        layer_mark_dirty(bitmap_layer_get_layer(direction_layer));
#endif
      }
      break;
    case KEY_BG_COLOR:
      if (t->value->int32 != flag_bgColor)
      {
        persist_write_int(KEY_BG_COLOR, t->value->int32);
        flag_bgColor = t->value->int32;
        window_set_background_color(my_window, GColorFromHEX(flag_bgColor));
      }
      break;
    }

    // Look for next item
    t = dict_read_next(iterator);
  }

  if (need_weather)
  {
    request_phone_data();
  }

  if (need_time)
  {
    // Get a time structure
    time_t temp = time(NULL);
    struct tm *t = localtime(&temp);

    // Manually call the tick handler
    tick_handler(t, MINUTE_UNIT | DAY_UNIT);
  }
  else if (need_train_redraw)
  {
    time_t temp = time(NULL);
    update_train_display(localtime(&temp));
  }

  if (need_usage_redraw)
  {
#ifdef PBL_PLATFORM_EMERY
    update_usage_band();
#endif
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context)
{
  // APP_LOG(APP_LOG_LEVEL_ERROR, "INBOX DROPPED reason=%d", (int)reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context)
{
  flag_messaging_is_busy = false;
  // APP_LOG(APP_LOG_LEVEL_ERROR, "____Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context)
{
  flag_messaging_is_busy = false;
  // APP_LOG(APP_LOG_LEVEL_INFO, "_____Outbox send success!");
}

// creates text layer at given coordinates, given font and alignment
TextLayer *create_text_layer(GRect coords, GFont font, GTextAlignment align)
{
  TextLayer *text_layer = text_layer_create(coords);
  text_layer_set_font(text_layer, font);
  text_layer_set_text_color(text_layer, GColorFromHEX(flag_textColor));
  text_layer_set_background_color(text_layer, GColorClear);
  text_layer_set_text_alignment(text_layer, align);
  layer_add_child(window_layer, text_layer_get_layer(text_layer));
  return text_layer;
}

static void bluetooth_handler(bool state)
{

  if (state)
  {
    request_phone_data();
  }

  // if Bluetooth alert is totally disabled - exit from here
  if (flag_bluetooth_alert == BLUETOOTH_ALERT_DISABLED)
    return;

  switch (flag_bluetooth_alert)
  {
  case BLUETOOTH_ALERT_WEAK:
    vibes_enqueue_custom_pattern(VIBE_PATTERN_WEAK);
    break;
  case BLUETOOTH_ALERT_NORMAL:
    vibes_enqueue_custom_pattern(VIBE_PATTERN_NORMAL);
    break;
  case BLUETOOTH_ALERT_STRONG:
    vibes_enqueue_custom_pattern(VIBE_PATTERN_STRONG);
    break;
  case BLUETOOTH_ALERT_DOUBLE:
    vibes_enqueue_custom_pattern(VIBE_PATTERN_DOUBLE);
    break;
  }

  layer_mark_dirty(graphics_layer);
}

#ifdef PBL_PLATFORM_EMERY
// Claude usage lives in the band between the (raised) date and the bottom Bluetooth bar.
#define USAGE_BAR_X 8
#define USAGE_BAR_W (PBL_DISPLAY_WIDTH - 2 * USAGE_BAR_X)
#define USAGE_BAR_TOP 168
#define USAGE_BAR_H 8

// fraction (0-100) of a window still remaining before its reset epoch
static uint8_t pct_time_remaining(uint32_t reset, uint32_t window)
{
  uint32_t now = (uint32_t)time(NULL);
  if (reset <= now || window == 0)
    return 0;
  uint32_t rem = reset - now;
  if (rem >= window)
    return 100;
  return (uint8_t)(rem * 100 / window);
}

// fraction (0-100) of a window already elapsed (inverse of remaining)
static uint8_t pct_time_elapsed(uint32_t reset, uint32_t window)
{
  return 100 - pct_time_remaining(reset, window);
}

// format seconds-until-reset as "Xd Yh", "Xh Ym", or "Xm"
static void fmt_countdown(char *buf, size_t n, uint32_t reset_epoch)
{
  if (reset_epoch == 0) { buf[0] = '\0'; return; }  // no data yet
  int secs = (int)reset_epoch - (int)time(NULL);
  if (secs <= 0) { snprintf(buf, n, "new"); return; }  // window rolled, fresh soon
  int h = secs / 3600, m = (secs % 3600) / 60, d = h / 24;
  h %= 24;
  if (d > 0)      snprintf(buf, n, "%dd%dh", d, h);
  else if (h > 0) snprintf(buf, n, "%dh%dm", h, m);
  else            snprintf(buf, n, "%dm", m);
}

// colour a "used" bar: two-gate — must be BOTH ahead of pace AND past the absolute floor
static GColor usage_pace_color(uint8_t used, uint8_t elapsed)
{
  if (used >= 90)
    return GColorFromHEX(flag_usage_color_crit);
  bool over_pace = used > elapsed + flag_usage_pace_offset;
  bool past_abs  = (flag_usage_abs_warn == 0) || (used >= flag_usage_abs_warn);
  if (over_pace && past_abs)
    return GColorFromHEX(flag_usage_color_over);
  return GColorFromHEX(flag_usage_color_good);
}

// decide whether the bottom band shows bars or date, then hide/show text_date accordingly
static void update_usage_band(void)
{
  time_t now = time(NULL);
  bool no_data = (usage_5h_reset == 0 && usage_7d_reset == 0);
  bool bars;
  switch (flag_usage_band_mode) {
    case 1: // always bars - show regardless of data availability
      bars = true;
      break;
    case 2: // always alternate
      bars = !no_data && s_usage_phase;
      break;
    default: // 0 = smart
      if (no_data)
        bars = false;
      else if ((usage_7d_pct >= 100 && usage_7d_reset && (uint32_t)now < usage_7d_reset) ||
               (usage_5h_pct >= 100 && usage_5h_reset && (uint32_t)now < usage_5h_reset))
        bars = true;  // limit takeover active
      else if (usage_last_change && (uint32_t)now - usage_last_change < USAGE_FRESH_SECS)
        bars = true;  // usage seen recently -> stay on bars
      else
        bars = s_usage_phase; // idle -> alternate per 15s
      break;
  }
  usage_band_shows_bars = bars;
  layer_set_hidden(text_layer_get_layer(text_date), bars);
  layer_mark_dirty(graphics_layer);
}

static void usage_timer_callback(void *context)
{
  s_usage_phase = !s_usage_phase;
  update_usage_band();
  s_usage_timer = app_timer_register(15000, usage_timer_callback, NULL);
}

// one horizontal bar: dim full-width track + bright fill scaled by pct
static void draw_usage_bar(GContext *ctx, int y, uint8_t pct, GColor fill, GColor track)
{
  if (pct > 100)
    pct = 100;

  graphics_context_set_fill_color(ctx, track);
  graphics_fill_rect(ctx, GRect(USAGE_BAR_X, y, USAGE_BAR_W, USAGE_BAR_H), 0, GCornersAll);

  int w = USAGE_BAR_W * pct / 100;
  if (w > 0)
  {
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_rect(ctx, GRect(USAGE_BAR_X, y, w, USAGE_BAR_H), 0, GCornersAll);
  }
}

// draws a single centred takeover line where the four bars would otherwise be
static void draw_usage_takeover(GContext *ctx, const char *text, GColor color)
{
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, bn_19, GRect(4, USAGE_BAR_TOP - 4, PBL_DISPLAY_WIDTH - 8, 28),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void draw_usage(GContext *ctx)
{
  if (!usage_band_shows_bars)
    return; // band shows date this minute

  GColor text_col = GColorFromHEX(flag_textColor);

  if (flag_usage_display_mode == 1)
  {
    // text mode: always show 2x2 pct+countdown grid, no takeovers
    static char b5[8], b7[8], t5[10], t7[10];
    int hw = USAGE_BAR_W / 2;
    snprintf(b5, sizeof(b5), "5h:%d%%", (int)usage_5h_pct);
    snprintf(b7, sizeof(b7), "Wk:%d%%", (int)usage_7d_pct);
    fmt_countdown(t5, sizeof(t5), usage_5h_reset);
    fmt_countdown(t7, sizeof(t7), usage_7d_reset);
    graphics_context_set_text_color(ctx, text_col);
    graphics_draw_text(ctx, b5, bn_19, GRect(USAGE_BAR_X, USAGE_BAR_TOP,      hw, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, b7, bn_19, GRect(USAGE_BAR_X + hw, USAGE_BAR_TOP, hw, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, t5, bn_19, GRect(USAGE_BAR_X, USAGE_BAR_TOP + 30,      hw, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, t7, bn_19, GRect(USAGE_BAR_X + hw, USAGE_BAR_TOP + 30, hw, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    return;
  }

  // bar mode: takeovers when limits hit, then four bars
  // weekly limit hit -> whole band becomes reset date/time
  if (usage_7d_pct >= 100 && usage_7d_reset != 0 && (uint32_t)time(NULL) < usage_7d_reset)
  {
    time_t r = (time_t)usage_7d_reset;
    static char buf[24];
    strftime(buf, sizeof(buf), "WK %a %d %b %H:%M", localtime(&r));
    draw_usage_takeover(ctx, buf, text_col);
    return;
  }

  // 5h session used up -> band becomes countdown to reset
  if (usage_5h_pct >= 100 && usage_5h_reset != 0)
  {
    int secs = (int)usage_5h_reset - (int)time(NULL);
    if (secs > 0)
    {
      static char buf[24];
      snprintf(buf, sizeof(buf), "RESET IN %dH%02dM", secs / 3600, (secs % 3600) / 60);
      draw_usage_takeover(ctx, buf, text_col);
      return;
    }
  }

  uint8_t e5 = pct_time_elapsed(usage_5h_reset, 5 * 3600);
  uint8_t e7 = pct_time_elapsed(usage_7d_reset, 7 * 86400);
  GColor track = GColorDarkGray;
  GColor ref   = GColorCyan;
  int y0 = USAGE_BAR_TOP;
  int y1 = y0 + USAGE_BAR_H + 1;
  int y2 = y1 + USAGE_BAR_H + 2;
  int y3 = y2 + USAGE_BAR_H + 1;
  draw_usage_bar(ctx, y0, usage_5h_pct, usage_pace_color(usage_5h_pct, e5), track);
  draw_usage_bar(ctx, y1, e5,           ref,                                track);
  draw_usage_bar(ctx, y2, usage_7d_pct, usage_pace_color(usage_7d_pct, e7), track);
  draw_usage_bar(ctx, y3, e7,           ref,                                track);

  // stale marker: small dim square at right edge
  if (usage_stale)
  {
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx, GRect(PBL_DISPLAY_WIDTH - 4, USAGE_BAR_TOP, 2, 2), 0, GCornersAll);
  }
}
#endif

static void graphics_update_proc(Layer *layer, GContext *ctx)
{

  static GColor color;

#ifdef PBL_COLOR

  // doing battery color in ranges with fall thru:
  //       100% - 50% - GColorGreen
  //       49% - 20% - GColorIcterine
  //       19% - 0% - GColorRed

  switch (battery_state_service_peek().charge_percent)
  {
  case 100:
  case 90:
  case 80:
  case 70:
  case 60:
  case 50:
    color = GColorGreen;
    break;
  case 40:
  case 30:
  case 20:
    color = GColorIcterine;
    break;
  case 10:
  case 0:
    color = GColorRed;
    break;
  }
#else
  color = GColorWhite;
#endif

#ifdef PBL_RECT // on Aplite & Basalt draw think line for battery
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(0, 25 * PBL_DISPLAY_HEIGHT / 168, PBL_DISPLAY_WIDTH, 3), 0, GCornersAll);
#else // on Chalk draw think circle
  graphics_context_set_stroke_width(ctx, 4);
  graphics_context_set_stroke_color(ctx, color);
  graphics_draw_circle(ctx, center, 85);
#endif

  if (flag_bluetooth_alert != BLUETOOTH_ALERT_DISABLED && bluetooth_connection_service_peek())
  { // checkin bluetooth only if check is enabled
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorCyan);
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
#endif

#ifdef PBL_RECT // on Aplite & Basalt draw thick line
    graphics_fill_rect(ctx, GRect(0, PBL_DISPLAY_HEIGHT - 3, PBL_DISPLAY_WIDTH, 3), 0, GCornersAll);
#else // on Chalk draw think circle
    graphics_context_set_stroke_color(ctx, GColorCyan);
    graphics_draw_circle(ctx, center, 76);
#endif
  }

#ifdef PBL_PLATFORM_EMERY
  draw_usage(ctx);
#endif
}

static void battery_handler(BatteryChargeState state)
{
  snprintf(s_battery, sizeof("100%"), "%d%%", state.charge_percent);
  text_layer_set_text(text_battery, s_battery);

#ifndef PBL_RECT
  static GColor color;
  // doing battery color in ranges with fall thru:
  //       100% - 50% - GColorGreen
  //       49% - 20% - GColorIcterine
  //       19% - 0% - GColorRed
  switch (state.charge_percent)
  {
  case 100:
  case 90:
  case 80:
  case 70:
  case 60:
  case 50:
    color = GColorGreen;
    break;
  case 40:
  case 30:
  case 20:
    color = GColorIcterine;
    break;
  case 10:
  case 0:
    color = GColorRed;
    break;
  }

  text_layer_set_text_color(text_battery, color);
#endif
}

// adjusting time location when timeline quickview shows.
void unobstructed_changed(AnimationProgress progress, void *context)
{
  set_time_frame_for_unobstructed_area(layer_get_unobstructed_bounds(window_layer));
}

void unobstructed_did_change(void *context)
{
  set_time_frame_for_unobstructed_area(layer_get_unobstructed_bounds(window_layer));
}

#ifdef PBL_PLATFORM_EMERY
// experimental: tapping the departures area (top band) forces a fresh fetch from the phone
static void touch_handler(const TouchEvent *event, void *context)
{
  if (event->type == TouchEvent_Touchdown && event->y < 90)
    request_phone_data();
}
#endif

void handle_init(void)
{

  //   // need to catch when app resumes focus after notification, otherwise effect layer won't restore
  //   app_focus_service_subscribe_handlers((AppFocusHandlers){
  //     .did_focus = app_focus_changed,
  //     .will_focus = app_focus_changing
  //   });

  // going international
  setlocale(LC_ALL, "");

  my_window = window_create();
  window_stack_push(my_window, true);

  window_layer = window_get_root_layer(my_window);
  bounds = layer_get_bounds(window_layer);
  center = grect_center_point(&bounds);

  graphics_layer = layer_create(bounds);
  layer_set_update_proc(graphics_layer, graphics_update_proc);
  layer_add_child(window_layer, graphics_layer);

  meteoicons_all = gbitmap_create_with_resource(RESOURCE_ID_METEOICONS);
#ifdef TWO_LINE_TRAIN
  direction_icons_all = gbitmap_create_with_resource(RESOURCE_ID_DIRECTION_ICONS);
#endif
#ifdef PBL_RECT
  temp_layer = bitmap_layer_create(GRect(51 * PBL_DISPLAY_WIDTH / 144, 1, 41 * PBL_DISPLAY_WIDTH / 144, 20 * PBL_DISPLAY_HEIGHT / 168));
#else
  temp_layer = bitmap_layer_create(GRect(86, 137, 41, 21));
#endif
  bitmap_layer_set_compositing_mode(temp_layer, GCompOpSet);
  layer_add_child(graphics_layer, bitmap_layer_get_layer(temp_layer));

  flag_hoursMinutesSeparator = persist_exists(KEY_HOURS_MINUTES_SEPARATOR) ? persist_read_int(KEY_HOURS_MINUTES_SEPARATOR) : 0;
  flag_dateFormat = persist_exists(KEY_DATE_FORMAT) ? persist_read_int(KEY_DATE_FORMAT) : 0;
  flag_bluetooth_alert = persist_exists(KEY_BLUETOOTH_ALERT) ? persist_read_int(KEY_BLUETOOTH_ALERT) : 0;
  flag_language = persist_exists(KEY_LANGUAGE) ? persist_read_int(KEY_LANGUAGE) : LANG_DEFAULT;
  flag_textColor = persist_exists(KEY_TEXT_COLOR) ? persist_read_int(KEY_TEXT_COLOR) : 0xFFFFFF;
  flag_bgColor   = persist_exists(KEY_BG_COLOR)   ? persist_read_int(KEY_BG_COLOR)   : 0x000000;
  window_set_background_color(my_window, GColorFromHEX(flag_bgColor));
  tint_meteoicon();
#ifdef TWO_LINE_TRAIN
  tint_bitmap(direction_icons_all);
#endif

  load_fonts();

#ifdef PBL_RECT
  text_dow = create_text_layer(GRect(0, 30 * PBL_DISPLAY_HEIGHT / 168, bounds.size.w, 31 * PBL_DISPLAY_HEIGHT / 168), bn_30, GTextAlignmentCenter);
  text_time = create_text_layer(get_time_frame(), bn_69, GTextAlignmentCenter);
#if PBL_DISPLAY_HEIGHT != 168
  // emery: date lives in the usage band; bars and date time-share this space (one at a time)
  text_date = create_text_layer(GRect(0, 178, bounds.size.w, 42), bn_26, GTextAlignmentCenter);
#else
  text_date = create_text_layer(GRect(0, 129 * PBL_DISPLAY_HEIGHT / 168, bounds.size.w, 27 * PBL_DISPLAY_HEIGHT / 168), bn_26, GTextAlignmentCenter);
#endif
  text_battery = create_text_layer(GRect(PBL_DISPLAY_WIDTH - 46 * PBL_DISPLAY_WIDTH / 144, 0, 43 * PBL_DISPLAY_WIDTH / 144, 21 * PBL_DISPLAY_HEIGHT / 168), bn_19, GTextAlignmentRight);
  text_temp = create_text_layer(GRect(3, 0, 80 * PBL_DISPLAY_WIDTH / 144, 21 * PBL_DISPLAY_HEIGHT / 168), bn_19, GTextAlignmentLeft);

  #if PBL_DISPLAY_HEIGHT != 168
  // direction icon + both departures on one row, in the band between the battery bar and the time
  direction_layer = bitmap_layer_create(GRect(6, 31 * PBL_DISPLAY_HEIGHT / 168, DIR_ICON_W, DIR_ICON_H));
  bitmap_layer_set_compositing_mode(direction_layer, GCompOpSet);
  layer_add_child(window_layer, bitmap_layer_get_layer(direction_layer));
  text_train = create_text_layer(GRect(34, 28 * PBL_DISPLAY_HEIGHT / 168, bounds.size.w - 38, 33 * PBL_DISPLAY_HEIGHT / 168), bn_train, GTextAlignmentCenter);
  layer_set_hidden(bitmap_layer_get_layer(direction_layer), true);
  layer_set_hidden(text_layer_get_layer(text_train), true);

  zoom_layer_time = effect_layer_create(get_time_frame());
  effect_layer_add_effect(zoom_layer_time, effect_zoom, EL_ZOOM(139, 136));
  layer_add_child(window_layer, effect_layer_get_layer(zoom_layer_time));

  zoom_layer_meteoicon = effect_layer_create(GRect(51 * PBL_DISPLAY_WIDTH / 144, 1, 41 * PBL_DISPLAY_WIDTH / 144, 20 * PBL_DISPLAY_HEIGHT / 168));
  effect_layer_add_effect(zoom_layer_meteoicon, effect_zoom, EL_ZOOM(139, 136)); 
  layer_add_child(window_layer, effect_layer_get_layer(zoom_layer_meteoicon));
  #endif
#else
  text_dow = create_text_layer(GRect(0, 23, bounds.size.w, 31), bn_20, GTextAlignmentCenter);
  text_time = create_text_layer(get_time_frame(), bn_69, GTextAlignmentCenter);
  text_date = create_text_layer(GRect(35, 111, 80, 27), bn_19, GTextAlignmentLeft);
  text_battery = create_text_layer(GRect(108, 111, 40, 21), bn_19, GTextAlignmentRight);
  text_temp = create_text_layer(GRect(48, 136, 41, 20), bn_19, GTextAlignmentRight);
#endif

  // getting battery info
  battery_state_service_subscribe(battery_handler);
  battery_handler(battery_state_service_peek());

  // Register callbacks
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage
  app_message_open(app_message_inbox_size_maximum(), 512);

  // to detect when timeline peek is shown
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers){.change = unobstructed_changed, .did_change = unobstructed_did_change}, NULL);
  set_time_frame_for_unobstructed_area(layer_get_unobstructed_bounds(window_layer));

  // reading stored value
  if (persist_exists(KEY_WEATHER_CODE))
    show_icon(persist_read_int(KEY_WEATHER_CODE));
  if (persist_exists(KEY_WEATHER_TEMP))
    show_temperature(persist_read_int(KEY_WEATHER_TEMP));
  else
    text_layer_set_text(text_temp, "...");

  // restoring train data - ignore departures that have already passed
  uint32_t now32 = (uint32_t)time(NULL);
  if (persist_exists(KEY_TRAIN_DEPARTURE))
  {
    uint32_t d = (uint32_t)persist_read_int(KEY_TRAIN_DEPARTURE);
    if (d > now32)
      train_dep[0] = d;
  }
  if (persist_exists(KEY_TRAIN_DEPARTURE2))
  {
    uint32_t d = (uint32_t)persist_read_int(KEY_TRAIN_DEPARTURE2);
    if (d > now32)
      train_dep[1] = d;
  }
  if (persist_exists(KEY_TRAIN_PLATFORM))
    persist_read_string(KEY_TRAIN_PLATFORM, s_train_platform[0], sizeof(s_train_platform[0]));
  if (persist_exists(KEY_TRAIN_PLATFORM2))
    persist_read_string(KEY_TRAIN_PLATFORM2, s_train_platform[1], sizeof(s_train_platform[1]));
  if (persist_exists(KEY_TRAIN_EXPRESS))
    train_express = persist_read_int(KEY_TRAIN_EXPRESS);
  if (persist_exists(KEY_TRAIN_DIRECTION))
    train_direction = persist_read_int(KEY_TRAIN_DIRECTION);

  // restoring refresh schedule
  if (persist_exists(KEY_PEAK1_START))      peak1_start = persist_read_int(KEY_PEAK1_START);
  if (persist_exists(KEY_PEAK1_END))        peak1_end = persist_read_int(KEY_PEAK1_END);
  if (persist_exists(KEY_PEAK2_START))      peak2_start = persist_read_int(KEY_PEAK2_START);
  if (persist_exists(KEY_PEAK2_END))        peak2_end = persist_read_int(KEY_PEAK2_END);
  if (persist_exists(KEY_PEAK_INTERVAL))    peak_interval = persist_read_int(KEY_PEAK_INTERVAL);
  if (persist_exists(KEY_OFFPEAK_INTERVAL)) offpeak_interval = persist_read_int(KEY_OFFPEAK_INTERVAL);

#ifdef PBL_PLATFORM_EMERY
  // restoring Claude usage; treat restored data as stale until the phone re-confirms
  if (persist_exists(KEY_USAGE_5H_PCT))   usage_5h_pct   = persist_read_int(KEY_USAGE_5H_PCT);
  if (persist_exists(KEY_USAGE_5H_RESET)) usage_5h_reset = (uint32_t)persist_read_int(KEY_USAGE_5H_RESET);
  if (persist_exists(KEY_USAGE_7D_PCT))   usage_7d_pct   = persist_read_int(KEY_USAGE_7D_PCT);
  if (persist_exists(KEY_USAGE_7D_RESET)) usage_7d_reset = (uint32_t)persist_read_int(KEY_USAGE_7D_RESET);
  if (persist_exists(PERSIST_USAGE_LAST_CHANGE)) usage_last_change = (uint32_t)persist_read_int(PERSIST_USAGE_LAST_CHANGE);
  if (persist_exists(KEY_USAGE_BAND_MODE))    flag_usage_band_mode    = persist_read_int(KEY_USAGE_BAND_MODE);
  if (persist_exists(KEY_USAGE_DISPLAY_MODE)) flag_usage_display_mode = persist_read_int(KEY_USAGE_DISPLAY_MODE);
  if (persist_exists(KEY_USAGE_PACE_OFFSET))  flag_usage_pace_offset  = persist_read_int(KEY_USAGE_PACE_OFFSET);
  if (persist_exists(KEY_USAGE_ABS_WARN))     flag_usage_abs_warn     = persist_read_int(KEY_USAGE_ABS_WARN);
  if (persist_exists(KEY_USAGE_COLOR_GOOD))   flag_usage_color_good   = persist_read_int(KEY_USAGE_COLOR_GOOD);
  if (persist_exists(KEY_USAGE_COLOR_OVER))   flag_usage_color_over   = persist_read_int(KEY_USAGE_COLOR_OVER);
  if (persist_exists(KEY_USAGE_COLOR_CRIT))   flag_usage_color_crit   = persist_read_int(KEY_USAGE_COLOR_CRIT);
  usage_stale = true;
#endif

  // initial bluetooth check
  flag_bluetooth_alert = 0;
  bluetooth_connection_service_subscribe(bluetooth_handler);
  bluetooth_handler(bluetooth_connection_service_peek());
  flag_bluetooth_alert = persist_exists(KEY_BLUETOOTH_ALERT) ? persist_read_int(KEY_BLUETOOTH_ALERT) : 1;

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

#ifdef PBL_PLATFORM_EMERY
  if (touch_service_is_enabled())
    touch_service_subscribe(touch_handler, NULL);
  s_usage_timer = app_timer_register(15000, usage_timer_callback, NULL);
#endif

  // Get a time structure so that the face doesn't start blank
  time_t temp = time(NULL);
  struct tm *t = localtime(&temp);

  // Manually call the tick handler when the window is loading
  tick_handler(t, DAY_UNIT | MINUTE_UNIT);
}

void handle_deinit(void)
{

  // clearning MASK
  text_layer_destroy(text_date);
  text_layer_destroy(text_time);
  text_layer_destroy(text_dow);
  text_layer_destroy(text_battery);
  text_layer_destroy(text_temp);
#ifdef TWO_LINE_TRAIN
  text_layer_destroy(text_train);
  bitmap_layer_destroy(direction_layer);
  if (direction_icon_current)
    gbitmap_destroy(direction_icon_current);
  gbitmap_destroy(direction_icons_all);
#endif

  gbitmap_destroy(meteoicons_all);
  gbitmap_destroy(meteoicon_current);
  bitmap_layer_destroy(temp_layer);

  layer_destroy(graphics_layer);

  window_destroy(my_window);
  app_message_deregister_callbacks();
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  unobstructed_area_service_unsubscribe();
#ifdef PBL_PLATFORM_EMERY
  touch_service_unsubscribe();
  if (s_usage_timer) { app_timer_cancel(s_usage_timer); s_usage_timer = NULL; }
#endif
  //   app_focus_service_unsubscribe();
}

int main(void)
{
  handle_init();
  app_event_loop();
  handle_deinit();
}
