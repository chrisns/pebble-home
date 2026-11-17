// Alarm: the state as a big coloured ring; up/down (or swipe) picks an action,
// select (or tap) does it. Arming away is a hold, with a fill that shows it
// coming, so it can't happen by accident. Exit/entry delays become a
// countdown with a one-touch Cancel.
#include "app.h"

#define HOLD_MS 500
#define HOLD_TICK_MS 25
#define SWIPE_PX 30

typedef struct {
  const char *label;
  const char *mode;
  GColor colour;
  bool hold;  // needs a 0.5 s hold to fire
} AlarmOpt;

static Window *s_win;
static Layer *s_layer;
static AlarmOpt s_opts[5];
static int s_n, s_sel;
static bool s_waiting;
static bool s_blink;
static AppTimer *s_blink_timer;
static time_t s_confirm_until;
static int s_count_total;
static int s_tick;

// Hold-to-confirm, shared by the select button and a finger.
static AppTimer *s_hold_timer;
static int s_hold_ms;        // 0 when not holding
static bool s_hold_fired;
static time_t s_hint_until;  // "Hold to arm away" after a short press

// Touch tracking.
static GPoint s_touch_start;
static uint32_t s_touch_start_ms;
static bool s_touch_moved;
static bool s_touching;

static uint32_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000 + ms;
}

static bool in_delay(void) { return g_home.alarm == ALARM_ARMING || g_home.alarm == ALARM_PENDING; }

static void build_opts(void) {
  s_n = 0;
  if (g_home.alarm == ALARM_ARMING) {
    s_opts[s_n++] = (AlarmOpt) { "Cancel", "cancel", GColorWhite, false };
    s_sel = 0;
    return;
  }
  if (g_home.alarm == ALARM_PENDING) {
    s_opts[s_n++] = (AlarmOpt) { "Disarm", "disarm", GColorIslamicGreen, false };
    s_sel = 0;
    return;
  }
  int32_t f = g_home.alarm_features ? g_home.alarm_features : 3;
  if (g_home.alarm != ALARM_DISARMED) s_opts[s_n++] = (AlarmOpt) { "Disarm", "disarm", GColorIslamicGreen, false };
  if ((f & 1) && g_home.alarm != ALARM_ARMED_HOME)
    s_opts[s_n++] = (AlarmOpt) { "Arm home", "arm_home", GColorChromeYellow, false };
  if ((f & 2) && g_home.alarm != ALARM_ARMED_AWAY)
    s_opts[s_n++] = (AlarmOpt) { "Arm away", "arm_away", GColorRed, true };
  if ((f & 4) && g_home.alarm != ALARM_ARMED_NIGHT)
    s_opts[s_n++] = (AlarmOpt) { "Arm night", "arm_night", GColorVividViolet, false };
  if (s_sel >= s_n) s_sel = 0;
}

// ------------------------------------------------------------------ acting
static void disarm_pin(const char *pin, void *ctx) {
  s_waiting = true;
  comm_action("alarm", "disarm", NULL, pin, NULL);
  if (s_layer) layer_mark_dirty(s_layer);
}

static void act(void) {
  if (!s_n || s_waiting) return;
  AlarmOpt *o = &s_opts[s_sel];
  if (strcmp(o->mode, "cancel") == 0) {
    // Stopping the exit delay: no PIN, the house isn't armed yet.
    s_waiting = true;
    comm_action("alarm", "disarm", NULL, NULL, NULL);
    vibes_short_pulse();
  } else if (strcmp(o->mode, "disarm") == 0) {
    if (g_home.pin_required) {
      pin_push("Disarm", GColorIslamicGreen, disarm_pin, NULL);
      return;
    }
    if (time(NULL) <= s_confirm_until) {
      s_confirm_until = 0;
      disarm_pin(NULL, NULL);
    } else {
      // No PIN set: a second press confirms, so a stray click can't disarm.
      s_confirm_until = time(NULL) + 3;
      vibes_short_pulse();
    }
  } else {
    s_waiting = true;
    comm_action("alarm", o->mode, NULL, NULL, NULL);
    vibes_short_pulse();
  }
  layer_mark_dirty(s_layer);
}

static void hold_tick(void *c) {
  s_hold_timer = NULL;
  if (!s_hold_ms) return;
  s_hold_ms += HOLD_TICK_MS;
  if (s_hold_ms >= HOLD_MS) {
    s_hold_ms = 0;
    s_hold_fired = true;
    act();
  } else {
    s_hold_timer = app_timer_register(HOLD_TICK_MS, hold_tick, NULL);
  }
  if (s_layer) layer_mark_dirty(s_layer);
}

static void hold_begin(void) {
  s_hold_fired = false;
  if (!s_n || !s_opts[s_sel].hold) return;
  s_hold_ms = 1;
  if (s_hold_timer) app_timer_cancel(s_hold_timer);
  s_hold_timer = app_timer_register(HOLD_TICK_MS, hold_tick, NULL);
}

// Returns true if the press was a hold option let go too early.
static bool hold_end(void) {
  bool was_holding = s_hold_ms > 0;
  s_hold_ms = 0;
  if (s_hold_timer) {
    app_timer_cancel(s_hold_timer);
    s_hold_timer = NULL;
  }
  if (was_holding && !s_hold_fired) {
    s_hint_until = time(NULL) + 2;
    vibes_short_pulse();
    if (s_layer) layer_mark_dirty(s_layer);
    return true;
  }
  return false;
}

static void move_sel(int d) {
  if (!s_n || in_delay()) return;
  s_sel = (s_sel + s_n + d) % s_n;
  s_confirm_until = 0;
  layer_mark_dirty(s_layer);
}

// ----------------------------------------------------------------- buttons
static void up(ClickRecognizerRef r, void *c) { move_sel(-1); }
static void down(ClickRecognizerRef r, void *c) { move_sel(1); }

static void select_down(ClickRecognizerRef r, void *c) { hold_begin(); }

static void select_up(ClickRecognizerRef r, void *c) {
  if (s_n && s_opts[s_sel].hold) {
    hold_end();
    return;
  }
  act();
}

static void clicks(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, up);
  window_single_click_subscribe(BUTTON_ID_DOWN, down);
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_down, select_up, NULL);
}

// ------------------------------------------------------------------- touch
#if defined(PBL_TOUCH)
static void on_touch(const TouchEvent *e, void *ctx) {
  switch (e->type) {
    case TouchEvent_Touchdown:
      s_touching = true;
      s_touch_moved = false;
      s_touch_start = GPoint(e->x, e->y);
      s_touch_start_ms = now_ms();
      // A finger held on the screen is the same as holding select.
      if (!e->non_navigational) hold_begin();
      break;
    case TouchEvent_PositionUpdate: {
      int dx = e->x - s_touch_start.x, dy = e->y - s_touch_start.y;
      if (!s_touch_moved && (abs(dx) > SWIPE_PX || abs(dy) > SWIPE_PX)) {
        s_touch_moved = true;
        // A swipe, not a hold: abandon any fill quietly.
        s_hold_ms = 0;
        if (s_hold_timer) {
          app_timer_cancel(s_hold_timer);
          s_hold_timer = NULL;
        }
        layer_mark_dirty(s_layer);
      }
      break;
    }
    case TouchEvent_Liftoff: {
      if (!s_touching) break;
      s_touching = false;
      int dx = e->x - s_touch_start.x, dy = e->y - s_touch_start.y;
      if (s_touch_moved) {
        // Swipe left/up for the next option, right/down for the previous.
        if (abs(dx) >= abs(dy)) move_sel(dx < 0 ? 1 : -1);
        else move_sel(dy < 0 ? 1 : -1);
        break;
      }
      if (e->non_navigational) break;  // brushed an idle watch: ignore
      if (s_n && s_opts[s_sel].hold) {
        hold_end();
      } else if (now_ms() - s_touch_start_ms < 600) {
        act();  // a tap anywhere does the selected thing
      }
      break;
    }
  }
}
#endif

// ------------------------------------------------------------------- draw
static void draw_countdown(GContext *ctx, GRect b) {
  int cx = b.size.w / 2;
  int left = alarm_remaining();
  if (left > s_count_total) s_count_total = left;
  int total = s_count_total ? s_count_total : 30;
  bool exit_delay = g_home.alarm == ALARM_ARMING;
  GColor col = exit_delay ? GColorOrange : GColorRed;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(4)), GOvalScaleModeFitCircle, 14, 0, TRIG_MAX_ANGLE);
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(4)), GOvalScaleModeFitCircle, 14, 0,
                       TRIG_MAX_ANGLE * left / total);
  char label[24];
  snprintf(label, sizeof(label), exit_delay ? "Arming %s" : "Entry delay", g_home.alarm_to);
  draw_label(ctx, label, FONT_KEY_GOTHIC_24_BOLD, GRect(30, 40, b.size.w - 60, 30), col, GTextAlignmentCenter);
  char secs[6];
  snprintf(secs, sizeof(secs), "%d", left);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, left ? secs : "…", fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM),
                     GRect(0, 72, b.size.w, 70), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  draw_label(ctx, s_waiting ? "Sending…" : (exit_delay ? "Leave now, or cancel" : "Disarm now"),
             FONT_KEY_GOTHIC_18, GRect(36, 138, b.size.w - 72, 22), GColorLightGray, GTextAlignmentCenter);
  GRect pill = GRect(cx - 74, 168, 148, 42);
  graphics_context_set_fill_color(ctx, exit_delay ? GColorWhite : GColorIslamicGreen);
  graphics_fill_rect(ctx, pill, 21, GCornersAll);
  draw_label(ctx, exit_delay ? "Cancel" : (time(NULL) <= s_confirm_until ? "Press again" : "Disarm"),
             FONT_KEY_GOTHIC_24_BOLD, GRect(pill.origin.x, pill.origin.y + 5, pill.size.w, 30),
             exit_delay ? GColorBlack : GColorWhite, GTextAlignmentCenter);
}

static void draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  if (in_delay()) {
    draw_countdown(ctx, b);
    return;
  }
  s_count_total = 0;
  GColor ring = theme_alarm_colour(g_home.alarm);
  if (!theme_alarm_blinks(g_home.alarm) || s_blink) {
    graphics_context_set_fill_color(ctx, ring);
    graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(1)), GOvalScaleModeFitCircle, 22, 0, TRIG_MAX_ANGLE);
  }
  theme_icon(ctx, ICON_SHIELD, GPoint(cx, 62), 18, ring);
  draw_label(ctx, g_home.alarm_label[0] ? g_home.alarm_label : "Unknown", FONT_KEY_GOTHIC_28_BOLD,
             GRect(26, 84, b.size.w - 52, 34), GColorWhite, GTextAlignmentCenter);

  AlarmOpt *o = s_n ? &s_opts[s_sel] : NULL;
  char sub[48] = "";
  GColor subc = GColorLightGray;
  if (s_waiting) {
    snprintf(sub, sizeof(sub), "Sending…");
  } else if (o && time(NULL) <= s_confirm_until) {
    snprintf(sub, sizeof(sub), "Press again to disarm");
    subc = GColorWhite;
  } else if (o && o->hold && (s_hold_ms || time(NULL) <= s_hint_until)) {
    snprintf(sub, sizeof(sub), "Hold to %s", "arm away");
    subc = GColorWhite;
  } else if (g_home.open_n && g_home.alarm == ALARM_DISARMED) {
    snprintf(sub, sizeof(sub), "%d open", (int)g_home.open_n);
    subc = GColorOrange;
  } else if (g_home.alarm_by[0] && g_home.alarm_since > 1000000000) {
    char ago[12];
    fmt_ago(g_home.alarm_since, ago, sizeof(ago));
    snprintf(sub, sizeof(sub), "%s · %s", g_home.alarm_by, ago);
  }
  draw_label(ctx, sub, FONT_KEY_GOTHIC_18_BOLD, GRect(30, 122, b.size.w - 60, 22), subc, GTextAlignmentCenter);
  if (!o) return;

  GRect pill = GRect(cx - 74, 156, 148, 42);
  GColor fg = gcolor_equal(o->colour, GColorChromeYellow) ? GColorBlack : GColorWhite;
  if (o->hold) {
    // Outline, filling left to right as the hold completes.
    graphics_context_set_stroke_color(ctx, o->colour);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_round_rect(ctx, pill, 21);
    if (s_hold_ms) {
      int w = pill.size.w * s_hold_ms / HOLD_MS;
      graphics_context_set_fill_color(ctx, o->colour);
      graphics_fill_rect(ctx, GRect(pill.origin.x, pill.origin.y, w < 21 ? 21 : w, pill.size.h), 21, GCornersAll);
    }
    fg = GColorWhite;
  } else {
    graphics_context_set_fill_color(ctx, o->colour);
    graphics_fill_rect(ctx, pill, 21, GCornersAll);
  }
  draw_label(ctx, o->label, FONT_KEY_GOTHIC_24_BOLD, GRect(pill.origin.x, pill.origin.y + 5, pill.size.w, 30), fg,
             GTextAlignmentCenter);
  for (int i = 0; i < s_n && s_n > 1; i++) {
    graphics_context_set_fill_color(ctx, i == s_sel ? GColorWhite : GColorDarkGray);
    graphics_fill_circle(ctx, GPoint(cx - (s_n - 1) * 6 + i * 12, 210), 3);
  }
}

static void blink_cb(void *c) {
  s_blink = !s_blink;
  // While counting down, check in every 3 s so "Armed" lands promptly, and
  // buzz for the last five seconds.
  if (in_delay() && ++s_tick % 6 == 0) comm_sync(false);
  int left = alarm_remaining();
  if (in_delay() && left > 0 && left <= 5 && s_blink) vibes_short_pulse();
  if (s_layer) layer_mark_dirty(s_layer);
  s_blink_timer = app_timer_register(500, blink_cb, NULL);
}

// ------------------------------------------------------------------ window
static void appear(Window *w) {
#if defined(PBL_TOUCH)
  if (touch_service_is_enabled()) touch_service_subscribe(on_touch, NULL);
#endif
}

static void disappear(Window *w) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
  hold_end();
}

static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
  s_blink_timer = app_timer_register(500, blink_cb, NULL);
#if defined(PBL_TOUCH)
  window_set_touch_bridge_disabled(w, true);  // we read touches ourselves
#endif
}

static void unload(Window *w) {
  if (s_blink_timer) app_timer_cancel(s_blink_timer);
  if (s_hold_timer) app_timer_cancel(s_hold_timer);
  s_blink_timer = s_hold_timer = NULL;
  layer_destroy(s_layer);
  s_layer = NULL;
  window_destroy(s_win);
  s_win = NULL;
}

bool alarm_is_open(void) { return s_win != NULL; }

void alarm_push(void) {
  s_sel = 0;
  s_waiting = false;
  s_hold_ms = 0;
  build_opts();
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers) {
    .load = load, .unload = unload, .appear = appear, .disappear = disappear });
  window_stack_push(s_win, true);
  comm_sync(true);
}

void alarm_on_message(DictionaryIterator *it, int32_t cmd) {
  static AlarmState prev = ALARM_UNKNOWN;
  if (cmd == CMD_STATE && g_home.have_state) {
    if (prev == ALARM_ARMING && g_home.alarm != ALARM_ARMING && s_layer) vibes_double_pulse();
    prev = g_home.alarm;
  }
  if (!s_layer) return;
  if (cmd == CMD_RESULT) s_waiting = false;
  if (cmd == CMD_STATE || cmd == CMD_RESULT) {
    build_opts();
    layer_mark_dirty(s_layer);
  }
}
