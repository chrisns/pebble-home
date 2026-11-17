// Music: now playing on a speaker, album art filling the screen, volume as an
// arc around the bezel.
//   up/down       volume (hold to repeat)
//   select        play / pause
//   hold select   next track
//   touch: swipe up/down volume, left/right next/previous track, tap play/pause
#include "app.h"

#if defined(PBL_TOUCH)
#define MUSIC_HINT "swipe < > track  tap play/pause"
#else
#define MUSIC_HINT "hold select: next track"
#endif

static Window *s_win;
static Layer *s_layer;
static int s_index;
static char s_art_key[12];
static int s_vol_override = -1;   // optimistic volume until state catches up
static AppTimer *s_vol_timer;
static bool s_show_vol;
static AppTimer *s_hint_timer;
static bool s_show_hint = true;

static int count(void) { return list_count(g_home.media); }

static void field(int f, char *out, size_t len) { list_field(g_home.media, s_index, f, out, len); }

static int volume(void) {
  if (s_vol_override >= 0) return s_vol_override;
  char v[6];
  field(3, v, sizeof(v));
  return atoi(v);
}

static void request_art(void) {
  char id[40];
  field(0, id, sizeof(id));
  char key[12];
  field(6, key, sizeof(key));
  strncpy(s_art_key, key, sizeof(s_art_key) - 1);
  img_release();
  if (!key[0] || strcmp(key, "-") == 0) return;
  img_request_media(id, 260, 260, 4);
}

static void on_ready(GBitmap *b, void *c) { if (s_layer) layer_mark_dirty(s_layer); }
static void on_fail(const char *why, void *c) { if (s_layer) layer_mark_dirty(s_layer); }

static void draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  if (!count()) {
    draw_label(ctx, "No speakers set up", FONT_KEY_GOTHIC_24_BOLD, GRect(30, 110, b.size.w - 60, 30), GColorWhite,
               GTextAlignmentCenter);
    return;
  }
  char name[16], state[12], title[42], artist[32];
  field(1, name, sizeof(name));
  field(2, state, sizeof(state));
  list_field(g_home.media_t, s_index, 0, title, sizeof(title));
  list_field(g_home.media_t, s_index, 1, artist, sizeof(artist));
  bool playing = strcmp(state, "playing") == 0;

  GBitmap *art = img_current();
  if (art) {
    GRect ib = gbitmap_get_bounds(art);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, art, GRect((b.size.w - ib.size.w) / 2, (b.size.h - ib.size.h) / 2,
                                                 ib.size.w, ib.size.h));
  } else {
    theme_icon(ctx, ICON_NOTE, GPoint(cx, 112), 40, GColorDarkGray);
  }

  // Volume arc around the bezel, 300 degrees, gap at the bottom.
  int v = volume();
  if (v >= 0) {
    GRect ring = grect_inset(b, GEdgeInsets(2));
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(0)), GOvalScaleModeFitCircle, 13, DEG_TO_TRIGANGLE(210),
                         DEG_TO_TRIGANGLE(510));
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_radial(ctx, ring, GOvalScaleModeFitCircle, 8, DEG_TO_TRIGANGLE(210), DEG_TO_TRIGANGLE(510));
    graphics_context_set_fill_color(ctx, GColorMagenta);
    graphics_fill_radial(ctx, ring, GOvalScaleModeFitCircle, 8, DEG_TO_TRIGANGLE(210),
                         DEG_TO_TRIGANGLE(210) + DEG_TO_TRIGANGLE(300) * v / 100);
  }

  // Top: speaker and position in the list.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, 52), 0, GCornerNone);
  draw_label(ctx, name, FONT_KEY_GOTHIC_18_BOLD, GRect(40, 22, b.size.w - 80, 22), GColorMagenta, GTextAlignmentCenter);

  // Bottom: title and artist.
  graphics_fill_rect(ctx, GRect(0, b.size.h - 74, b.size.w, 74), 0, GCornerNone);
  draw_label(ctx, title[0] ? title : (playing ? "Playing" : "Nothing playing"), FONT_KEY_GOTHIC_18_BOLD,
             GRect(34, b.size.h - 74, b.size.w - 68, 22), GColorWhite, GTextAlignmentCenter);
  draw_label(ctx, artist, FONT_KEY_GOTHIC_14, GRect(50, b.size.h - 52, b.size.w - 100, 18), GColorLightGray,
             GTextAlignmentCenter);

  // Centre badge: pause/play state, or the volume number while adjusting.
  if (s_show_vol || !playing) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(cx, cx), 30);
    if (s_show_vol) {
      char vb[6];
      snprintf(vb, sizeof(vb), "%d", v);
      draw_label(ctx, vb, FONT_KEY_GOTHIC_28_BOLD, GRect(cx - 30, cx - 19, 60, 34), GColorMagenta, GTextAlignmentCenter);
    } else {
      graphics_context_set_fill_color(ctx, GColorWhite);
      // play triangle
      GPathInfo tri = { .num_points = 3, .points = (GPoint[]) { { -9, -14 }, { 15, 0 }, { -9, 14 } } };
      GPath *p = gpath_create(&tri);
      gpath_move_to(p, GPoint(cx + 2, cx));
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
    }
  }
  if (s_show_hint) {
    draw_label(ctx, MUSIC_HINT, FONT_KEY_GOTHIC_14, GRect(30, b.size.h - 34, b.size.w - 60, 18),
               GColorDarkGray, GTextAlignmentCenter);
  }
}

static void vol_hide(void *c) {
  s_vol_timer = NULL;
  s_show_vol = false;
  s_vol_override = -1;
  if (s_layer) layer_mark_dirty(s_layer);
}

static void send_media(const char *mode) {
  char id[40];
  field(0, id, sizeof(id));
  comm_action("media", mode, id, NULL, NULL);
}

static void vol(int d) {
  int v = volume() + d * 4;
  s_vol_override = v < 0 ? 0 : v > 100 ? 100 : v;
  s_show_vol = true;
  send_media(d > 0 ? "vol_up" : "vol_down");
  if (s_vol_timer) app_timer_cancel(s_vol_timer);
  s_vol_timer = app_timer_register(1500, vol_hide, NULL);
  layer_mark_dirty(s_layer);
}

static void up(ClickRecognizerRef r, void *c) { vol(1); }
static void down(ClickRecognizerRef r, void *c) { vol(-1); }
static void select(ClickRecognizerRef r, void *c) { send_media("play_pause"); }
static void select_long(ClickRecognizerRef r, void *c) { send_media("next"); vibes_short_pulse(); }

#if defined(PBL_TOUCH)
static void on_swipe(const Recognizer *r, RecognizerEvent e) {
  if (e != RecognizerEvent_Completed) return;
  switch (swipe_recognizer_get_direction(r)) {
    case SwipeDirection_Left: send_media("next"); vibes_short_pulse(); break;
    case SwipeDirection_Right: send_media("prev"); vibes_short_pulse(); break;
    case SwipeDirection_Up: vol(1); vol(1); break;
    case SwipeDirection_Down: vol(-1); vol(-1); break;
    default: break;
  }
}

static void on_tap(const Recognizer *r, RecognizerEvent e) {
  if (e == RecognizerEvent_Completed) send_media("play_pause");
}

static void attach_touch(Window *w) {
  window_set_touch_bridge_disabled(w, true);
  Recognizer *swipe = swipe_recognizer_create(on_swipe, NULL, SwipeDirection_Up | SwipeDirection_Down |
                                              SwipeDirection_Left | SwipeDirection_Right);
  Recognizer *tap = tap_recognizer_create(on_tap, NULL);
  recognizer_set_fail_after(tap, swipe);  // a tap only once it's clearly not a swipe
  window_attach_recognizer(w, swipe);
  window_attach_recognizer(w, tap);
}
#endif

static void clicks(void *c) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

static void hint_hide(void *c) {
  s_hint_timer = NULL;
  s_show_hint = false;
  if (s_layer) layer_mark_dirty(s_layer);
}

static void appear(Window *w) {
  img_set_handlers(on_ready, on_fail, NULL);
  request_art();
}

static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
  s_show_hint = true;
  s_hint_timer = app_timer_register(3500, hint_hide, NULL);
#if defined(PBL_TOUCH)
  attach_touch(w);
#endif
}

static void unload(Window *w) {
  if (s_hint_timer) app_timer_cancel(s_hint_timer);
  if (s_vol_timer) app_timer_cancel(s_vol_timer);
  s_hint_timer = s_vol_timer = NULL;
  layer_destroy(s_layer);
  s_layer = NULL;
  img_release();
  window_destroy(s_win);
  s_win = NULL;
}

int music_playing_index(int *how_many) {
  int first = -1, n = 0;
  char st[12];
  for (int i = 0; i < count(); i++) {
    list_field(g_home.media, i, 2, st, sizeof(st));
    if (strcmp(st, "playing") == 0) {
      if (first < 0) first = i;
      n++;
    }
  }
  if (how_many) *how_many = n;
  return first;
}

void music_push(int index) {
  s_index = index >= 0 && index < count() ? index : 0;
  s_art_key[0] = 0;
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers) { .load = load, .unload = unload, .appear = appear });
  window_stack_push(s_win, true);
}

void music_on_message(DictionaryIterator *it, int32_t cmd) {
  if (!s_layer) return;
  if (cmd == CMD_STATE) {
    if (s_index >= count()) s_index = 0;
    char key[12];
    field(6, key, sizeof(key));
    if (strcmp(key, s_art_key) != 0) request_art();
  }
  layer_mark_dirty(s_layer);
}
