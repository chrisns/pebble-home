// Cameras: full-bleed live view. Up/down change camera, select toggles
// full-circle and widescreen framing. Also shows event snapshots.
#include "app.h"

#define REFRESH_MS 3000   // retry after a failed frame
#define STREAM_GAP_MS 120 // between live frames: next one as soon as this lands
#define DOOR_LIVE_S 45    // a doorbell alert closes itself this long after the ring

static Window *s_win;
static Layer *s_layer;
static int s_index;
static bool s_event_mode;
static int32_t s_event_id;
static char s_title[34], s_msg[82], s_kind[12];
static bool s_has_img;
static char s_acts[92];       // notification buttons "A|B"
static int32_t s_act_flags;   // bit n: button n needs a confirm or the PIN
static char s_labels[4][24];
static int s_chosen = -1;
static char s_live_cam[40];  // event with a camera: stream it
static bool s_still_shown;   // the stored snapshot went up first
static bool s_door;          // doorbell: every button unlocks
static int s_unlock;         // 0 not yet, 1 sending, 2 unlocked, 3 sent (no lock seen), 4 failed
static bool s_exit_on_dismiss;
static AppTimer *s_dismiss;
static bool s_wide;
static AppTimer *s_timer;
static char s_fail[24];
static time_t s_got_at;

static void request(void);

static bool streaming(void) { return !s_event_mode || s_live_cam[0]; }

static void refresh_cb(void *c) {
  s_timer = NULL;
  if (streaming()) request();
}

static void request(void) {
  s_fail[0] = '\0';
  int h = s_wide ? 146 : 260;
  if (s_event_mode && (!s_live_cam[0] || (s_has_img && !s_still_shown))) {
    if (s_has_img) img_request_event(s_event_id, 260, h, 4);
    return;
  }
  if (s_event_mode) {
    img_request_camera(s_live_cam, 260, h, 4, "cover");
    return;
  }
  char cam[40];
  if (!list_field(g_home.cams, s_index, 0, cam, sizeof(cam))) {
    snprintf(s_fail, sizeof(s_fail), "No cameras");
    return;
  }
  img_request_camera(cam, 260, h, 4, "cover");
}

static void on_ready(GBitmap *b, void *ctx) {
  s_got_at = time(NULL);
  s_still_shown = true;
  if (s_layer) layer_mark_dirty(s_layer);
  if (streaming() && s_win) {
    if (s_timer) app_timer_cancel(s_timer);
    s_timer = app_timer_register(STREAM_GAP_MS, refresh_cb, NULL);
  }
}

static void on_fail(const char *why, void *ctx) {
  strncpy(s_fail, why, sizeof(s_fail) - 1);
  if (s_layer) layer_mark_dirty(s_layer);
  if (streaming() && s_win && !s_timer) s_timer = app_timer_register(REFRESH_MS, refresh_cb, NULL);
}

static void draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  GBitmap *bmp = img_current();
  GColor accent = s_event_mode ? theme_kind_colour(s_kind) : GColorPictonBlue;
  if (bmp) {
    GRect ib = gbitmap_get_bounds(bmp);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, bmp, GRect((b.size.w - ib.size.w) / 2, (b.size.h - ib.size.h) / 2,
                                                 ib.size.w, ib.size.h));
  } else {
    theme_icon(ctx, ICON_CAMERA, GPoint(b.size.w / 2 + 6, b.size.h / 2), 24, GColorDarkGray);
  }
  int p = img_progress();
  if (p >= 0) {
    graphics_context_set_fill_color(ctx, accent);
    graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(1)), GOvalScaleModeFitCircle, 5, 0, TRIG_MAX_ANGLE * p / 100);
  }
  // Caption strip at the bottom of the circle.
  char name[24] = "";
  if (s_event_mode) {
    snprintf(name, sizeof(name), "%s", s_title);
  } else {
    list_field(g_home.cams, s_index, 1, name, sizeof(name));
  }
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, b.size.h - 50, b.size.w, 50), 0, GCornerNone);
  draw_label(ctx, name, FONT_KEY_GOTHIC_18_BOLD, GRect(50, b.size.h - 50, b.size.w - 100, 22), GColorWhite,
             GTextAlignmentCenter);
  char sub[48];
  if (s_fail[0]) {
    snprintf(sub, sizeof(sub), "%s", s_fail);
  } else if (s_event_mode && s_live_cam[0] && s_still_shown && bmp) {
    snprintf(sub, sizeof(sub), "LIVE  %s", s_msg);
  } else if (s_event_mode) {
    snprintf(sub, sizeof(sub), "%s", s_msg);
  } else {
    snprintf(sub, sizeof(sub), "%d of %d  %s", s_index + 1, list_count(g_home.cams), bmp ? "live" : "loading");
  }
  draw_label(ctx, sub, FONT_KEY_GOTHIC_14, GRect(60, b.size.h - 30, b.size.w - 120, 18),
             s_fail[0] ? GColorOrange : GColorLightGray, GTextAlignmentCenter);

  // Notification buttons: a pill at the top says what Select (or a tap) does.
  int n = s_event_mode ? list_count(s_acts) : 0;
  if (n || s_door) {
    char pill[28];
    GColor pc = (s_act_flags & 1) && n == 1 ? GColorOrange : accent;
    if (s_door) {
      static const char *const say[] = {"Press to unlock", "Unlocking…", "Unlocked", "Sent", "Didn't unlock"};
      snprintf(pill, sizeof(pill), "%s", say[s_unlock]);
      pc = s_unlock == 2 ? GColorMalachite : s_unlock == 1 ? GColorLightGray
         : s_unlock == 3 ? accent : s_unlock == 4 ? GColorRed : GColorOrange;
    } else if (n == 1) {
      list_field(s_acts, 0, 0, pill, sizeof(pill));
    } else {
      snprintf(pill, sizeof(pill), "%d actions", n);
    }
    GRect pr = GRect(b.size.w / 2 - 80, 22, 160, 30);
    graphics_context_set_fill_color(ctx, pc);
    graphics_fill_rect(ctx, pr, 15, GCornersAll);
    draw_label(ctx, pill, FONT_KEY_GOTHIC_18_BOLD, GRect(pr.origin.x + 8, pr.origin.y + 2, pr.size.w - 16, 24),
               GColorBlack, GTextAlignmentCenter);
  }
}

// ------------------------------------------------------- notification buttons
static void send_chosen(const char *code) {
  comm_notify_action(s_event_id, s_chosen, (s_act_flags >> s_chosen) & 1, code);
}

static void pin_done(const char *pin, void *ctx) { send_chosen(pin); }

static void perform(ActionMenu *menu, const ActionMenuItem *item, void *ctx) {
  s_chosen = (int)(intptr_t)action_menu_item_get_action_data(item);
}

static void menu_closed(ActionMenu *menu, const ActionMenuItem *item, void *ctx) {
  action_menu_hierarchy_destroy(action_menu_get_root_level(menu), NULL, NULL);
  if (s_chosen < 0) return;
  bool guarded = (s_act_flags >> s_chosen) & 1;
  if (guarded && g_home.pin_required) {
    pin_push(s_labels[s_chosen], GColorOrange, pin_done, NULL);
  } else {
    send_chosen(NULL);
  }
}

// Doorbell: any button lets them in. The notification's first button if it
// has one (so the phone's unlock automation runs), else the first lock.
static void lock_pin_done(const char *pin, void *ctx) {
  char id[40];
  list_field(g_home.locks, 0, 0, id, sizeof(id));
  comm_action("lock", "unlock", id, pin, NULL);
}

static void door_unlock(void) {
  if (s_unlock == 1 || s_unlock == 2) return;
  s_unlock = 1;
  if (s_acts[0]) {
    s_chosen = 0;
    if ((s_act_flags & 1) && g_home.pin_required) pin_push(s_labels[0], GColorOrange, pin_done, NULL);
    else send_chosen(NULL);
  } else if (list_count(g_home.locks)) {
    if (g_home.pin_required) pin_push("Unlock", GColorOrange, lock_pin_done, NULL);
    else lock_pin_done(NULL, NULL);
  } else {
    s_unlock = 0;
    toast("No lock set up", false);
  }
  if (s_layer) layer_mark_dirty(s_layer);
}

static void dismiss_cb(void *c) {
  s_dismiss = NULL;
  if (!s_win) return;
  if (s_exit_on_dismiss) window_stack_pop_all(true);  // back to the watchface
  else window_stack_remove(s_win, true);
}

static void open_actions(void) {
  int n = list_count(s_acts);
  if (n > 4) n = 4;
  if (!n) return;
  s_chosen = -1;
  ActionMenuLevel *root = action_menu_level_create(n);
  for (int i = 0; i < n; i++) {
    list_field(s_acts, i, 0, s_labels[i], sizeof(s_labels[i]));
    bool guarded = (s_act_flags >> i) & 1;
    if (guarded && !g_home.pin_required) {
      // No PIN: a deliberate second step instead.
      ActionMenuLevel *confirm = action_menu_level_create(1);
      action_menu_level_add_action(confirm, "Confirm", perform, (void *)(intptr_t)i);
      action_menu_level_add_child(root, confirm, s_labels[i]);
    } else {
      action_menu_level_add_action(root, s_labels[i], perform, (void *)(intptr_t)i);
    }
  }
  ActionMenuConfig cfg = {
    .root_level = root,
    .colors = { .background = theme_kind_colour(s_kind), .foreground = GColorBlack },
    .did_close = menu_closed,
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&cfg);
}

static void cycle(int d) {
  int n = list_count(g_home.cams);
  if (s_event_mode || n < 2) return;
  s_index = (s_index + n + d) % n;
  img_release();
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  request();
  layer_mark_dirty(s_layer);
}

static void up(ClickRecognizerRef r, void *c) {
  if (s_door) door_unlock();
  else cycle(-1);
}
static void down(ClickRecognizerRef r, void *c) {
  if (s_door) door_unlock();
  else cycle(1);
}
static void select(ClickRecognizerRef r, void *c) {
  if (s_door) {
    door_unlock();
    return;
  }
  if (s_event_mode && s_acts[0]) {
    open_actions();
    return;
  }
  s_wide = !s_wide;
  request();
}

#if defined(PBL_TOUCH)
static void on_swipe(const Recognizer *r, RecognizerEvent e) {
  if (e != RecognizerEvent_Completed) return;
  SwipeDirection d = swipe_recognizer_get_direction(r);
  cycle(d == SwipeDirection_Left || d == SwipeDirection_Up ? 1 : -1);
}

static void on_tap(const Recognizer *r, RecognizerEvent e) {
  if (e != RecognizerEvent_Completed) return;
  if (s_event_mode && s_acts[0]) {
    open_actions();
    return;
  }
  s_wide = !s_wide;
  request();
}
#endif

static void clicks(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, up);
  window_single_click_subscribe(BUTTON_ID_DOWN, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select);
}

static void appear(Window *w) {
  img_set_handlers(on_ready, on_fail, NULL);
  img_release();
  request();
}

static void disappear(Window *w) {
  if (s_dismiss) {
    app_timer_cancel(s_dismiss);
    s_dismiss = NULL;
  }
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
}

static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
#if defined(PBL_TOUCH)
  window_set_touch_bridge_disabled(w, true);
  Recognizer *swipe = swipe_recognizer_create(on_swipe, NULL, SwipeDirection_Up | SwipeDirection_Down |
                                              SwipeDirection_Left | SwipeDirection_Right);
  Recognizer *tap = tap_recognizer_create(on_tap, NULL);
  recognizer_set_fail_after(tap, swipe);
  window_attach_recognizer(w, swipe);
  window_attach_recognizer(w, tap);
#endif
}

static void unload(Window *w) {
  layer_destroy(s_layer);
  s_layer = NULL;
  img_release();
  window_destroy(s_win);
  s_win = NULL;
}

static void push(void) {
  if (s_win) window_stack_remove(s_win, false);
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers) {
    .load = load, .unload = unload, .appear = appear, .disappear = disappear });
  window_stack_push(s_win, true);
}

void camera_push(int index) {
  s_event_mode = false;
  s_door = false;
  s_live_cam[0] = '\0';
  s_acts[0] = '\0';
  s_index = index;
  s_wide = false;
  push();
}

void camera_push_event(int32_t event_id, const char *title, const char *msg, const char *kind) {
  HomeEvent ev = { .id = event_id, .has_img = true };
  strncpy(ev.title, title, sizeof(ev.title) - 1);
  strncpy(ev.msg, msg, sizeof(ev.msg) - 1);
  strncpy(ev.kind, kind, sizeof(ev.kind) - 1);
  camera_push_ev(&ev);
}

void camera_push_ev(const HomeEvent *ev) {
  s_event_mode = true;
  s_event_id = ev->id;
  s_wide = false;
  s_has_img = ev->has_img;
  snprintf(s_title, sizeof(s_title), "%s", ev->title);
  snprintf(s_msg, sizeof(s_msg), "%s", ev->msg);
  snprintf(s_kind, sizeof(s_kind), "%s", ev->kind);
  snprintf(s_acts, sizeof(s_acts), "%s", ev->acts);
  s_act_flags = ev->act_flags;
  snprintf(s_live_cam, sizeof(s_live_cam), "%s", ev->cam);
  s_still_shown = false;
  s_door = false;
  s_unlock = 0;
  s_exit_on_dismiss = false;
  push();
}

void camera_push_doorbell(const HomeEvent *ev, bool unlock_now, bool exit_after) {
  camera_push_ev(ev);
  s_door = true;
  s_exit_on_dismiss = exit_after;
  for (int i = 0; i < 4; i++) list_field(s_acts, i, 0, s_labels[i], sizeof(s_labels[i]));
  int left = DOOR_LIVE_S - (int)(time(NULL) - ev->ts);
  if (left < 10) left = 10;
  if (left > DOOR_LIVE_S) left = DOOR_LIVE_S;
  if (s_dismiss) app_timer_cancel(s_dismiss);
  s_dismiss = app_timer_register(left * 1000, dismiss_cb, NULL);
  if (unlock_now) door_unlock();
}

void camera_on_message(DictionaryIterator *it, int32_t cmd) {
  if (s_layer && cmd >= CMD_IMG_BEGIN && cmd <= CMD_IMG_CHUNK) layer_mark_dirty(s_layer);
  if (cmd == CMD_RESULT && s_door && s_unlock == 1) {
    // Only "unlocked" when HA saw the lock open: the button may just fire an
    // automation, and a lock can say "unlocking" and then give up.
    Tuple *ok = dict_find(it, MESSAGE_KEY_ok);
    Tuple *msg = dict_find(it, MESSAGE_KEY_msg);
    bool opened = msg && (strstr(msg->value->cstring, "unlocked") || strstr(msg->value->cstring, " open"));
    if (!(ok && ok->value->int32)) {
      s_unlock = 4;  // main shows why; a press tries again
    } else {
      s_unlock = opened || !s_acts[0] ? 2 : 3;
    }
    if (s_layer) layer_mark_dirty(s_layer);
  }
}
