// Home face: the time, ringed by the alarm state, with the house at a glance.
// When the doorbell goes the whole face becomes the porch camera.
#include <pebble.h>
#include "shared/model.h"
#include "shared/comm.h"
#include "shared/img.h"
#include "shared/theme.h"

#define POLL_IDLE_S 60
#define POLL_HOT_S 15
#define ALERT_SHOW_S 45
#define LIVE_GAP_MS 120     // between doorbell frames: the next as soon as one lands
#define LIVE_RETRY_MS 3000  // after a failed frame

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_poll_timer;
static AppTimer *s_alert_timer;
static AppTimer *s_live_timer;
static bool s_streaming;  // doorbell: frames back to back from the live camera
static bool s_blink_on = true;
static bool s_second_ticks;

static bool s_alert;
static HomeEvent s_alert_ev;
static time_t s_alert_started;
static char s_alert_fail[24];

static int32_t s_launch_event;  // opened from a timeline pin
static uint32_t s_launch_button; // a notification button picked on a pin
static char s_note[48];          // result of that button, shown for a while
static GColor s_note_colour;
static AppTimer *s_note_timer;

// ------------------------------------------------------------------- timing
static bool hot(void) {
  if (s_alert) return true;
  // Armed all day is normal; only the transitions deserve fast polling.
  if (theme_alarm_blinks(g_home.alarm)) return true;
  return g_home.last.ts && time(NULL) - g_home.last.ts < 300;
}

static void poll_cb(void *ctx);

static void schedule_poll(void) {
  if (s_poll_timer) app_timer_cancel(s_poll_timer);
  s_poll_timer = app_timer_register((hot() ? POLL_HOT_S : POLL_IDLE_S) * 1000, poll_cb, NULL);
}

static void poll_cb(void *ctx) {
  s_poll_timer = NULL;
  comm_sync(false);
  schedule_poll();
}

static void update_second_ticks(void);

// -------------------------------------------------------------------- alert
static void live_cb(void *ctx);

static void request_alert_image(bool live) {
  if (live && s_alert_ev.cam[0]) {
    img_request_camera(s_alert_ev.cam, 260, 260, 4, "cover");
  } else if (s_alert_ev.has_img) {
    img_request_event(s_alert_ev.id, 260, 260, 4);
  }
}

static void end_alert(void *ctx) {
  s_alert_timer = NULL;
  s_alert = false;
  s_streaming = false;
  if (s_live_timer) {
    app_timer_cancel(s_live_timer);
    s_live_timer = NULL;
  }
  img_release();
  update_second_ticks();
  layer_mark_dirty(s_canvas);
}

static void live_cb(void *ctx) {
  s_live_timer = NULL;
  if (!s_alert || !s_streaming) return;
  request_alert_image(true);
}

static void live_next(uint32_t ms) {
  if (!s_alert || !s_streaming) return;
  if (s_live_timer) app_timer_cancel(s_live_timer);
  s_live_timer = app_timer_register(ms, live_cb, NULL);
}

static void start_alert(const HomeEvent *ev) {
  s_alert_ev = *ev;
  s_alert = true;
  s_alert_fail[0] = '\0';
  s_alert_started = time(NULL);
  img_release();
  theme_vibe(ev->vibe);
  light_enable_interaction();
  request_alert_image(false);
  // Shown for 45 s from the ring, not from when this face heard of it (the
  // Home app may have had the screen for most of that).
  int left = ALERT_SHOW_S - (int)(time(NULL) - ev->ts);
  if (left > ALERT_SHOW_S) left = ALERT_SHOW_S;  // clocks a little apart
  if (left < 5) left = 5;
  if (s_alert_timer) app_timer_cancel(s_alert_timer);
  s_alert_timer = app_timer_register(left * 1000, end_alert, NULL);
  // Doorbells stream from the live camera while on screen: the stored
  // snapshot goes up first (it's instant), then frame after frame.
  s_streaming = strcmp(ev->kind, "doorbell") == 0 && ev->cam[0];
  if (s_streaming && !ev->has_img) live_next(10);
  update_second_ticks();
  schedule_poll();
  layer_mark_dirty(s_canvas);
}

static void img_ready(GBitmap *bmp, void *ctx) {
  s_alert_fail[0] = '\0';
  live_next(LIVE_GAP_MS);
  layer_mark_dirty(s_canvas);
}

static void img_fail(const char *why, void *ctx) {
  strncpy(s_alert_fail, why, sizeof(s_alert_fail) - 1);
  live_next(LIVE_RETRY_MS);
  layer_mark_dirty(s_canvas);
}

// ------------------------------------------------------------------ drawing
static void draw_text(GContext *ctx, const char *txt, const char *font, GRect box, GColor col) {
  graphics_context_set_text_color(ctx, col);
  graphics_draw_text(ctx, txt, fonts_get_system_font(font), box, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

// ---- TimeStyle-inspired layout: stacked time left, magenta sidebar right.
#define SIDEBAR_X 182
#define SIDE_CX 214
#define ACCENT GColorMagenta

static const char *alarm_word(AlarmState a) {
  switch (a) {
    case ALARM_DISARMED: return "OFF";
    case ALARM_ARMED_HOME: return "HOME";
    case ALARM_ARMED_NIGHT: return "NIGHT";
    case ALARM_ARMED_AWAY: return "AWAY";
    case ALARM_TRIGGERED: return "ALARM";
    default: return "--";
  }
}

static void draw_battery(GContext *ctx, GPoint c, GColor fg) {
  BatteryChargeState b = battery_state_service_peek();
  GRect body = GRect(c.x - 14, c.y - 7, 26, 14);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, body, 2);
  graphics_context_set_fill_color(ctx, fg);
  graphics_fill_rect(ctx, GRect(c.x + 13, c.y - 3, 3, 6), 1, GCornersRight);
  int w = 20 * b.charge_percent / 100;
  graphics_context_set_fill_color(ctx, b.charge_percent <= 20 ? GColorRed : fg);
  graphics_fill_rect(ctx, GRect(body.origin.x + 3, body.origin.y + 3, w < 2 ? 2 : w, 8), 0, GCornerNone);
  char pct[6];
  snprintf(pct, sizeof(pct), "%d", b.charge_percent);
  draw_text(ctx, pct, FONT_KEY_GOTHIC_14_BOLD, GRect(c.x - 22, c.y + 7, 44, 18), fg);
}

static void draw_sidebar(GContext *ctx, GRect b, AlarmState a, bool stale) {
  int left = alarm_remaining();
  bool delay = (a == ALARM_ARMING || a == ALARM_PENDING);
  GColor bg = ACCENT;
  if (stale || !g_home.have_state) bg = GColorDarkGray;
  if (delay) bg = a == ALARM_ARMING ? GColorOrange : GColorRed;
  if (a == ALARM_TRIGGERED) bg = s_blink_on ? GColorRed : GColorDarkCandyAppleRed;
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(SIDEBAR_X, 0, b.size.w - SIDEBAR_X, b.size.h), 0, GCornerNone);
  GColor fg = GColorWhite;

  if (delay) {
    // The exit/entry delay takes the sidebar over: label, seconds, where to.
    draw_text(ctx, a == ALARM_ARMING ? "EXIT" : "ENTRY", FONT_KEY_GOTHIC_18_BOLD,
              GRect(SIDE_CX - 30, 78, 60, 22), fg);
    char secs[6];
    snprintf(secs, sizeof(secs), "%d", left);
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, secs, fonts_get_system_font(FONT_KEY_LECO_36_BOLD_NUMBERS),
                       GRect(SIDE_CX - 34, 100, 68, 44), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    char to[10] = "";
    for (int i = 0; g_home.alarm_to[i] && i < 8; i++) {
      char ch = g_home.alarm_to[i];
      to[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    draw_text(ctx, a == ALARM_ARMING ? to : "DISARM", FONT_KEY_GOTHIC_14_BOLD, GRect(SIDE_CX - 30, 146, 60, 18), fg);
    // Time left also drains round the rim of the sidebar.
    int total = 30;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(1)), GOvalScaleModeFitCircle, 5,
                         DEG_TO_TRIGANGLE(45), DEG_TO_TRIGANGLE(45) + DEG_TO_TRIGANGLE(90) * (left > total ? total : left) / total);
    return;
  }

  // Weather
  if (g_home.wx_valid) {
    char t[8];
    snprintf(t, sizeof(t), "%d°", (int)g_home.wx_temp);
    draw_text(ctx, t, FONT_KEY_GOTHIC_24_BOLD, GRect(SIDE_CX - 30, 40, 60, 28), fg);
    char cond[12];
    strncpy(cond, g_home.wx_cond, sizeof(cond) - 1);
    cond[sizeof(cond) - 1] = 0;
    char *sp = strchr(cond, ' ');
    if (sp && strlen(cond) > 7) *sp = 0;  // "Partly cloudy" -> "Partly"
    draw_text(ctx, cond, FONT_KEY_GOTHIC_14, GRect(SIDE_CX - 32, 66, 64, 18), fg);
  }
  // Alarm: a shield in the state colour on a dark disc so it reads on magenta.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(SIDE_CX, 106), 15);
  theme_icon(ctx, ICON_SHIELD, GPoint(SIDE_CX, 105), 9, stale ? GColorLightGray : theme_alarm_colour(a));
  draw_text(ctx, stale ? (g_home.configured ? "OFFLINE" : "SETUP") : alarm_word(a), FONT_KEY_GOTHIC_14_BOLD,
            GRect(SIDE_CX - 32, 121, 64, 18), fg);
  // Date
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char day[6], num[4];
  strftime(day, sizeof(day), "%a", t);
  for (char *p = day; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  strftime(num, sizeof(num), "%e", t);
  draw_text(ctx, day, FONT_KEY_GOTHIC_14_BOLD, GRect(SIDE_CX - 30, 144, 60, 18), fg);
  draw_text(ctx, num[0] == ' ' ? num + 1 : num, FONT_KEY_GOTHIC_24_BOLD, GRect(SIDE_CX - 30, 156, 60, 28), fg);
  // Watch battery
  draw_battery(ctx, GPoint(SIDE_CX - 2, 198), fg);
}

static void draw_face(GContext *ctx, GRect b) {
  bool stale = !g_home.online || (g_home.synced_at && time(NULL) - g_home.synced_at > 300);
  AlarmState a = (stale || !g_home.have_state) ? ALARM_UNKNOWN : g_home.alarm;
  const int lx = 14, lw = SIDEBAR_X - 22, lcx = lx + lw / 2;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  draw_sidebar(ctx, b, a, stale);

  // Stacked time, hours over minutes, in Leco.
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char hh[4], mm[4];
  strftime(hh, sizeof(hh), clock_is_24h_style() ? "%H" : "%I", t);
  strftime(mm, sizeof(mm), "%M", t);
  graphics_context_set_text_color(ctx, GColorWhite);
  GFont big = fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM);
  graphics_draw_text(ctx, hh, big, GRect(lx, 56, lw, 66), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, mm, big, GRect(lx, 122, lw, 66), GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Above the time: the latest event, or what's playing, in the accent colour.
  char line[48] = "";
  GColor lc = ACCENT;
  if (g_home.last.id && time(NULL) - g_home.last.ts < 7200) {
    char ago[12];
    fmt_ago(g_home.last.ts, ago, sizeof(ago));
    snprintf(line, sizeof(line), "%s · %s", g_home.last.title, ago);
    lc = theme_kind_colour(g_home.last.kind);
  } else {
    for (int i = 0, n = list_count(g_home.media); i < n; i++) {
      char st[12], title[42];
      list_field(g_home.media, i, 2, st, sizeof(st));
      if (strcmp(st, "playing") != 0) continue;
      list_field(g_home.media_t, i, 0, title, sizeof(title));
      snprintf(line, sizeof(line), "%s", title);
      break;
    }
  }
  if (line[0]) draw_text(ctx, line, FONT_KEY_GOTHIC_14_BOLD, GRect(lx + 30, 36, lw - 34, 18), lc);

  // Below the time: lock and openings. (No presence: phone location isn't
  // reliable enough to put on the wrist.)
  if (g_home.have_state && !stale) {
    char lock_state[12] = "", status[40];
    list_field(g_home.locks, 0, 2, lock_state, sizeof(lock_state));
    bool locked = strcmp(lock_state, "locked") == 0;
    if (g_home.open_n) {
      snprintf(status, sizeof(status), "%s · %d open", locked ? "Locked" : "Unlocked", (int)g_home.open_n);
    } else {
      snprintf(status, sizeof(status), "%s · All shut", locked ? "Locked" : "Unlocked");
    }
    draw_text(ctx, status, FONT_KEY_GOTHIC_14_BOLD, GRect(lx + 10, 194, lw - 10, 18),
              (!locked || g_home.open_n) ? GColorOrange : GColorLightGray);
  } else if (!g_home.configured) {
    draw_text(ctx, "Set up in the Pebble app", FONT_KEY_GOTHIC_14, GRect(lx + 6, 192, lw - 6, 18), GColorLightGray);
  }
}

static void draw_alert(GContext *ctx, GRect b) {
  GColor kc = theme_kind_colour(s_alert_ev.kind);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  GBitmap *bmp = img_current();
  if (bmp) {
    GRect ib = gbitmap_get_bounds(bmp);
    GRect dst = GRect((b.size.w - ib.size.w) / 2, (b.size.h - ib.size.h) / 2, ib.size.w, ib.size.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, bmp, dst);
  } else {
    theme_icon(ctx, theme_kind_icon(s_alert_ev.kind), GPoint(b.size.w / 2, b.size.h / 2 - 6), 34, kc);
    int p = img_progress();
    if (p >= 0) {
      graphics_context_set_fill_color(ctx, kc);
      graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(70)), GOvalScaleModeFitCircle, 4, 0,
                           TRIG_MAX_ANGLE * p / 100);
    } else if (s_alert_fail[0]) {
      draw_text(ctx, s_alert_fail, FONT_KEY_GOTHIC_14, GRect(40, 176, b.size.w - 80, 18), GColorLightGray);
    }
  }
  // Banner across the top cap of the circle, caption across the bottom.
  graphics_context_set_fill_color(ctx, kc);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, 50), 0, GCornerNone);
  theme_icon(ctx, theme_kind_icon(s_alert_ev.kind), GPoint(b.size.w / 2 - 50, 34), 9, GColorBlack);
  draw_text(ctx, s_alert_ev.title, FONT_KEY_GOTHIC_24_BOLD, GRect(b.size.w / 2 - 36, 16, 110, 28), GColorBlack);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, b.size.h - 54, b.size.w, 54), 0, GCornerNone);
  char cap[48], when[8];
  time_t ts = s_alert_ev.ts;
  strftime(when, sizeof(when), clock_is_24h_style() ? "%H:%M" : "%l:%M", localtime(&ts));
  snprintf(cap, sizeof(cap), "%s  %s", s_alert_ev.msg, when);
  draw_text(ctx, cap, FONT_KEY_GOTHIC_18_BOLD, GRect(40, b.size.h - 52, b.size.w - 80, 22), GColorWhite);
  if (s_streaming && bmp) {
    graphics_context_set_fill_color(ctx, s_blink_on ? GColorRed : GColorDarkCandyAppleRed);
    graphics_fill_circle(ctx, GPoint(b.size.w / 2 - 58, b.size.h - 24), 4);
    draw_text(ctx, "LIVE", FONT_KEY_GOTHIC_14_BOLD, GRect(b.size.w / 2 - 52, b.size.h - 34, 36, 18), GColorWhite);
  }
  if (strcmp(s_alert_ev.kind, "doorbell") == 0) {
    // Tap Up or Down (set to Home in Quick Launch) opens Home and unlocks.
    draw_text(ctx, "Up/Down: unlock", FONT_KEY_GOTHIC_14_BOLD, GRect(b.size.w / 2 - 14, b.size.h - 34, 100, 18),
              GColorOrange);
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (s_alert) draw_alert(ctx, b);
  else draw_face(ctx, b);
  if (s_note[0]) {
    GRect r = GRect(b.size.w / 2 - 95, b.size.h / 2 - 34, 190, 68);
    graphics_context_set_fill_color(ctx, s_note_colour);
    graphics_fill_rect(ctx, r, 18, GCornersAll);
    draw_text(ctx, s_note, FONT_KEY_GOTHIC_24_BOLD, GRect(r.origin.x + 8, r.origin.y + 4, r.size.w - 16, 60),
              GColorBlack);
  }
}

static void note_clear(void *c) {
  s_note_timer = NULL;
  s_note[0] = '\0';
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void note(const char *text, GColor colour, uint32_t ms) {
  snprintf(s_note, sizeof(s_note), "%s", text);
  s_note_colour = colour;
  if (s_note_timer) app_timer_cancel(s_note_timer);
  s_note_timer = app_timer_register(ms, note_clear, NULL);
  if (s_canvas) layer_mark_dirty(s_canvas);
}

// ------------------------------------------------------------------- events
static void update_second_ticks(void);

static void tick(struct tm *t, TimeUnits changed) {
  s_blink_on = !s_blink_on || (changed & MINUTE_UNIT);
  // Countdown just ran out: ask for the new state rather than wait for the poll.
  static int last_left = -1;
  int left = alarm_remaining();
  if (last_left > 0 && left == 0) comm_sync(false);
  last_left = left;
  layer_mark_dirty(s_canvas);
}

static void update_second_ticks(void) {
  bool want = s_alert || (g_home.have_state && theme_alarm_blinks(g_home.alarm));
  if (want == s_second_ticks) return;
  s_second_ticks = want;
  tick_timer_service_subscribe(want ? SECOND_UNIT : MINUTE_UNIT, tick);
}

static void on_message(DictionaryIterator *it, int32_t cmd) {
  bool rung_out = strcmp(g_home.last.kind, "doorbell") == 0 && time(NULL) - g_home.last.ts >= ALERT_SHOW_S;
  if (cmd == CMD_EVENT && g_home.last.alert && !rung_out && !(s_alert && s_alert_ev.id == g_home.last.id)) {
    if (g_home.last.has_img || strcmp(g_home.last.kind, "alarm") != 0) {
      start_alert(&g_home.last);
    } else {
      // Alarm changes buzz; the sidebar shows the state and any countdown.
      theme_vibe(g_home.last.vibe);
    }
  }
  static bool said_hello;
  if (cmd == CMD_STATE && s_launch_button) {
    // A notification button from a pin: top bit, event id low 28 bits, button + 1.
    int32_t ref = (int32_t)((s_launch_button & 0x7FFFFFFF) >> 3);
    int idx = (int)(s_launch_button & 7) - 1;
    s_launch_button = 0;
    if (g_home.pin_required) {
      // A face has no buttons to type a PIN with.
      note("PIN needed: use the Home app", GColorOrange, 5000);
    } else {
      comm_notify_action(ref, idx, true, NULL);
      note("Sending…", GColorLightGray, 15000);
    }
  }
  if (cmd == CMD_RESULT && s_note[0]) {
    Tuple *ok = dict_find(it, MESSAGE_KEY_ok);
    Tuple *msg = dict_find(it, MESSAGE_KEY_msg);
    bool good = ok && ok->value->int32;
    if (good) vibes_short_pulse(); else vibes_double_pulse();
    note(msg ? msg->value->cstring : (good ? "Done" : "Failed"), good ? GColorMalachite : GColorRed, 4000);
  }
  if (cmd == CMD_STATE && s_launch_event) {
    // Opened from an event pin: show that snapshot.
    HomeEvent ev = { .id = s_launch_event, .ts = time(NULL), .has_img = true };
    strcpy(ev.kind, "alert");
    strcpy(ev.title, "Snapshot");
    strcpy(ev.vibe, "none");
    if (g_home.last.id == s_launch_event) ev = g_home.last;
    ev.alert = false;
    s_launch_event = 0;
    start_alert(&ev);
  }
  if (cmd == CMD_STATE) {
    schedule_poll();
    if (!said_hello) {
      said_hello = true;
      comm_hello();
    }
  }
  update_second_ticks();
  layer_mark_dirty(s_canvas);
}

static void tap(AccelAxisType axis, int32_t dir) {
  if (s_alert) {
    // Raising the wrist to look fires a tap too, so give it a few seconds.
    if (time(NULL) - s_alert_started < 5) return;
    // Flick to dismiss.
    if (s_alert_timer) app_timer_cancel(s_alert_timer);
    end_alert(NULL);
  } else {
    comm_sync(false);
  }
}

static void battery(BatteryChargeState s) {
  layer_mark_dirty(s_canvas);
  comm_hello();
}

static void bt(bool connected) {
  g_home.online = connected;
  if (connected) comm_sync(true);
  layer_mark_dirty(s_canvas);
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *w) { layer_destroy(s_canvas); }

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) { .load = window_load, .unload = window_unload });
  window_stack_push(s_window, false);

  img_set_handlers(img_ready, img_fail, NULL);
  if (launch_reason() == APP_LAUNCH_TIMELINE_ACTION) {
    uint32_t args = launch_get_args();
    if (args & 0x80000000u) s_launch_button = args;
    else s_launch_event = (int32_t)args;
  }
  comm_init(on_message, 8200);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);
  accel_tap_service_subscribe(tap);
  battery_state_service_subscribe(battery);
  connection_service_subscribe((ConnectionHandlers) { .pebble_app_connection_handler = bt });
  schedule_poll();
}

static void deinit(void) {
  img_release();
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
