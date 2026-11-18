// Home watchapp entry point and the dashboard menu.
#include "app.h"

typedef enum { ROW_ALARM, ROW_MUSIC, ROW_CAMERAS, ROW_EVENTS, ROW_LOCKS, ROW_ACTIONS, ROW_ASSIST, ROW_STATUS, ROW_COUNT } Row;

static Window *s_window;
static MenuLayer *s_menu;
static Window *s_toast_win;
static TextLayer *s_toast_text;
static AppTimer *s_toast_timer;
static char s_toast_buf[160];
static bool s_toast_ok;

void draw_label(GContext *ctx, const char *txt, const char *font, GRect box, GColor col, GTextAlignment al) {
  graphics_context_set_text_color(ctx, col);
  graphics_draw_text(ctx, txt, fonts_get_system_font(font), box, GTextOverflowModeTrailingEllipsis, al, NULL);
}

// ------------------------------------------------------------------- toast
static void toast_hide(void *ctx) {
  s_toast_timer = NULL;
  if (s_toast_win && window_stack_contains_window(s_toast_win)) window_stack_remove(s_toast_win, true);
}

static void toast_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  window_set_background_color(w, s_toast_ok ? GColorIslamicGreen : GColorDarkCandyAppleRed);
  s_toast_text = text_layer_create(grect_inset(b, GEdgeInsets(70, 30, 30, 30)));
  text_layer_set_background_color(s_toast_text, GColorClear);
  text_layer_set_text_color(s_toast_text, GColorWhite);
  text_layer_set_font(s_toast_text, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_toast_text, GTextAlignmentCenter);
  text_layer_set_text(s_toast_text, s_toast_buf);
  layer_add_child(root, text_layer_get_layer(s_toast_text));
}

static void toast_unload(Window *w) { text_layer_destroy(s_toast_text); }

void toast(const char *text, bool ok) {
  strncpy(s_toast_buf, text, sizeof(s_toast_buf) - 1);
  s_toast_ok = ok;
  if (!s_toast_win) {
    s_toast_win = window_create();
    window_set_window_handlers(s_toast_win, (WindowHandlers) { .load = toast_load, .unload = toast_unload });
  }
  if (window_stack_contains_window(s_toast_win)) window_stack_remove(s_toast_win, false);
  window_stack_push(s_toast_win, true);
  if (ok) vibes_short_pulse(); else vibes_double_pulse();
  if (s_toast_timer) app_timer_cancel(s_toast_timer);
  s_toast_timer = app_timer_register(ok ? 1500 : 2800, toast_hide, NULL);
}

// -------------------------------------------------------------------- menu
static void now_playing(char *buf, size_t len) {
  int n = list_count(g_home.media);
  char state[12], name[16], title[42];
  for (int i = 0; i < n; i++) {
    list_field(g_home.media, i, 2, state, sizeof(state));
    if (strcmp(state, "playing") == 0) {
      list_field(g_home.media, i, 1, name, sizeof(name));
      list_field(g_home.media_t, i, 0, title, sizeof(title));
      snprintf(buf, len, "%s: %s", name, title);
      return;
    }
  }
  snprintf(buf, len, n ? "%d speakers idle" : "No speakers", n);
}

static uint16_t num_rows(MenuLayer *m, uint16_t s, void *ctx) { return ROW_COUNT; }

static int16_t row_height(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return menu_layer_is_index_selected(m, idx) ? 72 : 44;
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  GRect b = layer_get_bounds(cell);
  bool sel = menu_cell_layer_is_highlighted(cell);
  char sub[64] = "";
  const char *title = "";
  IconKind icon = ICON_HOME;
  GColor accent = GColorVividCerulean;
  switch (idx->row) {
    case ROW_ALARM:
      title = "Alarm";
      icon = ICON_SHIELD;
      accent = theme_alarm_colour(g_home.alarm);
      snprintf(sub, sizeof(sub), "%s", g_home.have_state ? g_home.alarm_label : "Connecting…");
      break;
    case ROW_MUSIC:
      title = "Music";
      icon = ICON_NOTE;
      accent = GColorMagenta;
      now_playing(sub, sizeof(sub));
      break;
    case ROW_CAMERAS:
      title = "Cameras";
      icon = ICON_CAMERA;
      accent = GColorPictonBlue;
      snprintf(sub, sizeof(sub), "%d cameras", list_count(g_home.cams));
      break;
    case ROW_EVENTS:
      title = "Events";
      icon = ICON_BELL;
      accent = GColorChromeYellow;
      if (g_home.last.id) {
        char ago[12];
        fmt_ago(g_home.last.ts, ago, sizeof(ago));
        snprintf(sub, sizeof(sub), "%s, %s", g_home.last.title, ago);
      } else {
        snprintf(sub, sizeof(sub), "Nothing yet");
      }
      break;
    case ROW_LOCKS:
      title = "Doors";
      icon = ICON_LOCK;
      accent = g_home.open_n ? GColorOrange : GColorMediumSpringGreen;
      snprintf(sub, sizeof(sub), g_home.open_n ? "%d open" : "All shut", (int)g_home.open_n);
      break;
    case ROW_ACTIONS:
      title = "Actions";
      icon = ICON_BOLT;
      accent = GColorYellow;
      snprintf(sub, sizeof(sub), "%d shortcuts", list_count(g_home.acts));
      break;
    case ROW_ASSIST:
      title = "Assist";
      icon = ICON_MIC;
      accent = GColorVividCerulean;
      snprintf(sub, sizeof(sub), "Ask Home Assistant");
      break;
    case ROW_STATUS:
      title = "Status";
      icon = ICON_LIST;
      accent = GColorLightGray;
      snprintf(sub, sizeof(sub), "People, sensors");
      break;
  }
  GColor fg = sel ? GColorBlack : GColorWhite;
  if (sel) {
    graphics_context_set_fill_color(ctx, accent);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    theme_icon(ctx, icon, GPoint(b.size.w / 2, 18), 10, GColorBlack);
    draw_label(ctx, title, FONT_KEY_GOTHIC_24_BOLD, GRect(20, 26, b.size.w - 40, 28), fg, GTextAlignmentCenter);
    draw_label(ctx, sub, FONT_KEY_GOTHIC_14_BOLD, GRect(30, 50, b.size.w - 60, 18), fg, GTextAlignmentCenter);
  } else {
    theme_icon(ctx, icon, GPoint(b.size.w / 2 - 50, b.size.h / 2), 8, accent);
    draw_label(ctx, title, FONT_KEY_GOTHIC_24_BOLD, GRect(b.size.w / 2 - 36, b.size.h / 2 - 16, 120, 28), fg,
               GTextAlignmentLeft);
  }
}

static void select_row(MenuLayer *m, MenuIndex *idx, void *ctx) {
  switch (idx->row) {
    case ROW_ALARM: alarm_push(); break;
    case ROW_MUSIC: list_push(LIST_SPEAKERS); break;
    case ROW_CAMERAS: list_push(LIST_CAMERAS); break;
    case ROW_EVENTS: list_push(LIST_EVENTS); break;
    case ROW_LOCKS: list_push(LIST_LOCKS); break;
    case ROW_ACTIONS: list_push(LIST_ACTIONS); break;
    case ROW_ASSIST: assist_start(); break;
    case ROW_STATUS: list_push(LIST_STATUS); break;
  }
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = num_rows,
    .get_cell_height = row_height,
    .draw_row = draw_row,
    .select_click = select_row,
  });
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, GColorBlack, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void window_unload(Window *w) { menu_layer_destroy(s_menu); }

void main_menu_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) { .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
}

void main_menu_on_message(DictionaryIterator *it, int32_t cmd) {
  if (s_menu && (cmd == CMD_STATE || cmd == CMD_EVENT)) menu_layer_reload_data(s_menu);
}

// --------------------------------------------------------------- dispatch
static bool s_quick_launched;  // by Quick Launch, at s_launched_at
static bool s_quick_tap;       // ...by a single tap of Up or Down on the face
static time_t s_launched_at;

static void on_message(DictionaryIterator *it, int32_t cmd) {
  static bool said_hello;
  if (cmd == CMD_STATE && !said_hello) {
    said_hello = true;
    comm_hello();
  }
  if (cmd == CMD_EVENT && g_home.last.alert) {
    // Something happened while the app is open: show it.
    theme_vibe(g_home.last.vibe);
    bool ringing = strcmp(g_home.last.kind, "doorbell") == 0 && time(NULL) - g_home.last.ts < 45;
    bool just_launched = s_quick_launched && time(NULL) - s_launched_at < 15;
    if (ringing) {
      // A tap of Up/Down on the face while the doorbell rings means "let them in":
      // that press opened us, so unlock straight away. Either way every button
      // unlocks, and the alert goes after 45 s (back to the face if it brought us).
      camera_push_doorbell(&g_home.last, just_launched && s_quick_tap, just_launched);
      s_quick_tap = false;
    } else if (g_home.last.has_img || g_home.last.acts[0]) {
      camera_push_ev(&g_home.last);
    } else {
      toast(g_home.last.title, true);
    }
  }
  if (cmd == CMD_RESULT) {
    Tuple *ok = dict_find(it, MESSAGE_KEY_ok);
    Tuple *msg = dict_find(it, MESSAGE_KEY_msg);
    bool good = ok && ok->value->int32;
    // The alarm screen shows its own outcome (the countdown); don't cover it.
    if (assist_handle_result(it)) {
    } else if (good && alarm_is_open()) {
      vibes_short_pulse();
    } else {
      toast(msg ? msg->value->cstring : "Done", good);
    }
  }
  // An exit or entry delay anywhere in the house jumps straight to the
  // countdown, so cancelling is one press away.
  if (cmd == CMD_STATE && (g_home.alarm == ALARM_ARMING || g_home.alarm == ALARM_PENDING) && !alarm_is_open()) {
    alarm_push();
  }
  main_menu_on_message(it, cmd);
  camera_on_message(it, cmd);
  music_on_message(it, cmd);
  alarm_on_message(it, cmd);
  list_on_message(it, cmd);
}

static AppTimer *s_refresh;

static void refresh(void *ctx) {
  comm_sync(false);
  s_refresh = app_timer_register(15000, refresh, NULL);
}

int main(void) {
#if defined(PBL_TOUCH)
  // Menus scroll and rows activate by touch; plain click windows get taps
  // and swipes bridged to buttons. Music, cameras and alarm do their own.
  app_touch_navigation_enable(true);
#endif
  comm_init(on_message, 8200);
  main_menu_push();
  s_launched_at = time(NULL);
  if (launch_reason() == APP_LAUNCH_QUICK_LAUNCH) {
    s_quick_launched = true;
    // Quick Launch opens the main menu like any other launch. It's remembered
    // only so a tap of Up/Down while the doorbell rings can let them in.
    s_quick_tap = launch_get_quick_launch_action() == APP_QUICK_LAUNCH_ACTION_TAP;
  } else if (launch_reason() == APP_LAUNCH_TIMELINE_ACTION) {
    // Opened from an event pin: go straight to its snapshot.
    camera_push_event((int32_t)launch_get_args(), "Snapshot", "", "alert");
  }
  s_refresh = app_timer_register(15000, refresh, NULL);
  app_event_loop();
  img_release();
}
