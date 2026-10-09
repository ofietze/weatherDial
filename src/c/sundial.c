#include <pebble.h>

// ---------------------------------------------------------------------------
// Sundial: 24h dial (noon at top, midnight at bottom) with the next 24h of
// weather plotted around it. Red line = temperature, blue hills = rain,
// shaded sector = tonight (sunset -> sunrise).
// ---------------------------------------------------------------------------

// Color themes, selectable from the phone settings page (Clay)
typedef struct {
  GColor bg;     // main background
  GColor fg;     // hand, dial dots, time/date text
  GColor night;  // sunset -> sunrise sector
  GColor temp;   // temperature line + min/max text
  GColor rain;   // rain hills + total text
} Theme;

static Theme s_theme;
static int s_theme_idx;

#define THEME_LIGHT   0
#define THEME_DARK    1
#define THEME_BLUE_YELLOW 2
#define THEME_BW      3

static void apply_theme(int idx) {
  s_theme_idx = idx;
  switch (idx) {
    case THEME_DARK: // gray night so rain stays the only blue
      s_theme = (Theme){ GColorBlack, GColorWhite, GColorDarkGray,
                         GColorRed, GColorPictonBlue };
      break;
    case THEME_BLUE_YELLOW: // yellow background, dark navy night, light blue rain
      s_theme = (Theme){ GColorYellow, GColorBlack, GColorDukeBlue,
                         GColorRed, GColorVividCerulean };
      break;
    case THEME_BW: // black & white, works on non-color pebbles too
      s_theme = (Theme){ GColorWhite, GColorBlack, GColorLightGray,
                         GColorBlack, GColorDarkGray };
      break;
    default: // THEME_LIGHT
      s_theme = (Theme){ GColorWhite, GColorBlack, GColorLavenderIndigo,
                         GColorDarkCandyAppleRed, GColorBlueMoon };
      break;
  }
}

// Dial geometry (set in window_load from screen bounds)
static GPoint s_center;
static int s_r_text;   // inner circle holding the digital info
static int s_r_in;     // inner dotted ring = lower bound for data
static int s_r_out;    // outer dotted ring = upper bound for data

#define HOURS 24
#define PERSIST_KEY_VERSION 1
#define PERSIST_KEY_DATA    2
#define PERSIST_KEY_THEME   3
#define PERSIST_VERSION     1
#define WEATHER_PAYLOAD_VERSION 1

typedef struct __attribute__((packed)) {
  time_t base;          // timestamp of temps[0] / rains[0] (start of that hour)
  time_t sunrise;
  time_t sunset;
  int16_t temp_min;
  int16_t temp_max;
  uint16_t rain_total;  // mm * 10
  int8_t temps[HOURS];  // degrees C
  uint8_t rains[HOURS]; // mm * 10, per hour
  bool valid;
} WeatherData;

static WeatherData s_weather;

static Window *s_window;
static Layer *s_layer;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Angle for a wall-clock time: noon at top, midnight at bottom.
static int32_t angle_for_time(time_t t) {
  struct tm *lt = localtime(&t);
  int32_t mins = (lt->tm_hour * 60 + lt->tm_min - 720 + 1440) % 1440;
  return (int32_t)((int64_t)TRIG_MAX_ANGLE * mins / 1440);
}

// Angle for an hour mark (0-23)
static int32_t angle_for_hour(int h) {
  int32_t mins = ((h - 12 + 24) % 24) * 60;
  return (int32_t)((int64_t)TRIG_MAX_ANGLE * mins / 1440);
}

static GRect rect_for_radius(int r) {
  return GRect(s_center.x - r, s_center.y - r, 2 * r + 1, 2 * r + 1);
}

static GPoint polar(int r, int32_t angle) {
  return gpoint_from_polar(rect_for_radius(r), GOvalScaleModeFitCircle, angle);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw_night(GContext *ctx) {
  if (!s_weather.valid || s_weather.sunset == 0 || s_weather.sunrise == 0) return;
  int32_t a_set = angle_for_time(s_weather.sunset);
  int32_t a_rise = angle_for_time(s_weather.sunrise);
  if (a_rise <= a_set) a_rise += TRIG_MAX_ANGLE;
  // Fill from the center out past the screen corners; the inner circle and
  // everything else is drawn on top.
  int big = 170; // > sqrt(100^2 + 114^2)
  graphics_context_set_fill_color(ctx, s_theme.night);
  graphics_fill_radial(ctx, rect_for_radius(big), GOvalScaleModeFitCircle, big,
                       a_set, a_rise);
}

static void draw_rain(GContext *ctx) {
  if (!s_weather.valid) return;
  // Scale: full ring span = at least 3.0 mm/h, more if it rains harder
  int max_rain = 30;
  for (int i = 0; i < HOURS; i++) {
    if (s_weather.rains[i] > max_rain) max_rain = s_weather.rains[i];
  }
  int span = s_r_out - s_r_in;
  graphics_context_set_fill_color(ctx, s_theme.rain);
  // One radial slice per half hour, values interpolated between hours
  for (int i2 = 0; i2 < HOURS * 2; i2++) {
    int i = i2 / 2;
    int v2; // mm*10, doubled
    if (i2 % 2 == 0) {
      v2 = s_weather.rains[i] * 2;
    } else {
      v2 = s_weather.rains[i] + (i < HOURS - 1 ? s_weather.rains[i + 1] : s_weather.rains[i]);
    }
    if (v2 == 0) continue;
    int h = (v2 * span) / (2 * max_rain);
    if (h < 2) h = 2;
    time_t t0 = s_weather.base + (time_t)i2 * 1800;
    int32_t a0 = angle_for_time(t0);
    int32_t a1 = a0 + TRIG_MAX_ANGLE / 48 + TRIG_MAX_ANGLE / 512; // slight overlap
    graphics_fill_radial(ctx, rect_for_radius(s_r_in + h), GOvalScaleModeFitCircle,
                         h, a0, a1);
  }
}

static void draw_temp(GContext *ctx) {
  if (!s_weather.valid) return;
  int lo = s_weather.temps[0], hi = s_weather.temps[0];
  for (int i = 1; i < HOURS; i++) {
    if (s_weather.temps[i] < lo) lo = s_weather.temps[i];
    if (s_weather.temps[i] > hi) hi = s_weather.temps[i];
  }
  // Pad the scale so a flat day doesn't pin to the bounds
  if (hi - lo < 8) {
    int pad = (8 - (hi - lo) + 1) / 2;
    lo -= pad;
    hi += pad;
  }
  int lo2 = lo * 2, hi2 = hi * 2;

  graphics_context_set_stroke_color(ctx, s_theme.temp);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_context_set_antialiased(ctx, true);

  GPoint prev = GPointZero;
  GPoint first = GPointZero;
  for (int i2 = 0; i2 < HOURS * 2; i2++) {
    int i = i2 / 2;
    int v2; // degrees C, doubled
    if (i2 % 2 == 0) {
      v2 = s_weather.temps[i] * 2;
    } else {
      v2 = s_weather.temps[i] + (i < HOURS - 1 ? s_weather.temps[i + 1] : s_weather.temps[i]);
    }
    int r = s_r_in + 2 + ((v2 - lo2) * (s_r_out - s_r_in - 4)) / (hi2 - lo2);
    time_t t = s_weather.base + (time_t)i2 * 1800;
    GPoint p = polar(r, angle_for_time(t));
    if (i2 > 0) graphics_draw_line(ctx, prev, p);
    else first = p;
    prev = p;
  }
  // Close the ring: connect the last half-hour sample back to the first
  graphics_draw_line(ctx, prev, first);
}

static void draw_dial(GContext *ctx) {
  // Black dots vanish against the navy night sector in the blue & yellow
  // theme, so back them with a background-colored halo there.
  bool outline = (s_theme_idx == THEME_BLUE_YELLOW);
  for (int h = 0; h < 24; h++) {
    int32_t a = angle_for_hour(h);
    int r = (h % 6 == 0) ? 2 : 1;
    GPoint p = polar(s_r_out, a);
    if (outline) {
      graphics_context_set_fill_color(ctx, s_theme.bg);
      graphics_fill_circle(ctx, p, r + 1);
    }
    graphics_context_set_fill_color(ctx, s_theme.fg);
    graphics_fill_circle(ctx, p, r);
  }
}

static void draw_hand(GContext *ctx, time_t now) {
  int32_t a = angle_for_time(now);
  graphics_context_set_stroke_color(ctx, s_theme.fg);
  graphics_context_set_stroke_width(ctx, 5);
  graphics_context_set_antialiased(ctx, true);
  graphics_draw_line(ctx, s_center, polar(s_r_out + 5, a));
}

static void draw_center(GContext *ctx, struct tm *now) {
  graphics_context_set_fill_color(ctx, s_theme.bg);
  graphics_fill_circle(ctx, s_center, s_r_text);
  graphics_context_set_stroke_color(ctx, s_theme.fg);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, s_center, s_r_text);

  graphics_context_set_text_color(ctx, s_theme.fg);
  GFont big = fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS);
  GFont date_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont info_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

  char buf[12];
  strftime(buf, sizeof(buf), "%H", now);
  graphics_draw_text(ctx, buf, big,
                     GRect(s_center.x - 44, s_center.y - 62, 88, 42),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  strftime(buf, sizeof(buf), "%M", now);
  graphics_draw_text(ctx, buf, big,
                     GRect(s_center.x - 44, s_center.y + 18, 88, 42),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // Date on two lines: day number, then three-letter month
  strftime(buf, sizeof(buf), "%d\n%b", now);
  graphics_draw_text(ctx, buf, date_font,
                     GRect(s_center.x - 44, s_center.y - 22, 88, 44),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  if (s_weather.valid) {
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%d°\n%d°",
             (int)s_weather.temp_max, (int)s_weather.temp_min);
    graphics_context_set_text_color(ctx, s_theme.temp);
    graphics_draw_text(ctx, tbuf, info_font,
                       GRect(s_center.x - 62, s_center.y - 30, 42, 60),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    char rbuf[12];
    int mm = (s_weather.rain_total + 5) / 10;
    snprintf(rbuf, sizeof(rbuf), "%d\nmm", mm);
    graphics_context_set_text_color(ctx, s_theme.rain);
    graphics_draw_text(ctx, rbuf, info_font,
                       GRect(s_center.x + 20, s_center.y - 30, 42, 60),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

static void layer_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_theme.bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  time_t now_t = time(NULL);
  struct tm now = *localtime(&now_t);

  draw_night(ctx);
  draw_rain(ctx);
  draw_temp(ctx);
  draw_dial(ctx);
  draw_hand(ctx, now_t);
  draw_center(ctx, &now);
}

// ---------------------------------------------------------------------------
// Weather data plumbing
// ---------------------------------------------------------------------------

static void request_weather(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
  app_message_outbox_send();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  // Settings from the Clay config page (may arrive without weather data)
  if ((t = dict_find(iter, MESSAGE_KEY_THEME))) {
    int idx = (t->type == TUPLE_CSTRING) ? (t->value->cstring[0] - '0')
                                         : (int)t->value->int32;
    apply_theme(idx);
    persist_write_int(PERSIST_KEY_THEME, idx);
    layer_mark_dirty(s_layer);
  }

  // Reject payloads from a newer/foreign companion build, but accept ones
  // without a version field for backward compatibility (e.g. config-only msgs)
  if ((t = dict_find(iter, MESSAGE_KEY_PAYLOAD_VERSION))) {
    if (t->value->int32 != WEATHER_PAYLOAD_VERSION) return;
  }

  WeatherData w = s_weather;

  if ((t = dict_find(iter, MESSAGE_KEY_BASE_TIMESTAMP))) w.base = t->value->uint32;
  if ((t = dict_find(iter, MESSAGE_KEY_SUNRISE))) w.sunrise = t->value->uint32;
  if ((t = dict_find(iter, MESSAGE_KEY_SUNSET))) w.sunset = t->value->uint32;
  if ((t = dict_find(iter, MESSAGE_KEY_TEMP_MIN))) w.temp_min = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_TEMP_MAX))) w.temp_max = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_RAIN_TOTAL))) w.rain_total = t->value->uint32;

  t = dict_find(iter, MESSAGE_KEY_TEMP);
  if (!t || t->length < HOURS) return; // incomplete payload
  for (int i = 0; i < HOURS; i++) w.temps[i] = (int8_t)(t->value->data[i] - 100);

  t = dict_find(iter, MESSAGE_KEY_RAIN);
  if (!t || t->length < HOURS) return;
  for (int i = 0; i < HOURS; i++) w.rains[i] = t->value->data[i];

  w.valid = true;
  s_weather = w;
  persist_write_int(PERSIST_KEY_VERSION, PERSIST_VERSION);
  persist_write_data(PERSIST_KEY_DATA, &s_weather, sizeof(s_weather));
  layer_mark_dirty(s_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  layer_mark_dirty(s_layer);
  // Refresh weather every 30 minutes, or sooner if we have no/stale data
  bool stale = !s_weather.valid ||
               (time(NULL) - s_weather.base) > 2 * SECONDS_PER_HOUR;
  if (tick_time->tm_min % 30 == 0 || (stale && tick_time->tm_min % 5 == 0)) {
    request_weather();
  }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_center = grect_center_point(&bounds);
  s_r_out = (bounds.size.w / 2) - 6;    // 94 on emery
  s_r_in = s_r_out - 22;                // 72
  s_r_text = s_r_in - 2;                // 70

  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, layer_update);
  layer_add_child(root, s_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_layer);
}

static void init(void) {
  apply_theme(persist_exists(PERSIST_KEY_THEME)
                  ? persist_read_int(PERSIST_KEY_THEME)
                  : THEME_DARK);

  if (persist_read_int(PERSIST_KEY_VERSION) == PERSIST_VERSION &&
      persist_exists(PERSIST_KEY_DATA)) {
    persist_read_data(PERSIST_KEY_DATA, &s_weather, sizeof(s_weather));
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_open(512, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
