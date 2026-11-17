// Classic Pebble PIN entry: up/down picks a digit, select moves on, back goes back.
#include "app.h"

#define PIN_LEN 4

static Window *s_win;
static Layer *s_layer;
static char s_digits[PIN_LEN];
static int s_pos;
static const char *s_title;
static GColor s_accent;
static PinDone s_done;
static void *s_ctx;

static void draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_radial(ctx, grect_inset(b, GEdgeInsets(1)), GOvalScaleModeFitCircle, 8, 0, TRIG_MAX_ANGLE);
  theme_icon(ctx, ICON_LOCK, GPoint(cx, 42), 12, s_accent);
  draw_label(ctx, s_title, FONT_KEY_GOTHIC_24_BOLD, GRect(20, 58, b.size.w - 40, 28), GColorWhite, GTextAlignmentCenter);
  const int w = 40, gap = 10;
  int x = cx - (PIN_LEN * w + (PIN_LEN - 1) * gap) / 2;
  for (int i = 0; i < PIN_LEN; i++) {
    GRect box = GRect(x + i * (w + gap), 104, w, 54);
    if (i == s_pos) {
      graphics_context_set_fill_color(ctx, s_accent);
      graphics_fill_rect(ctx, box, 8, GCornersAll);
      char d[2] = { '0' + s_digits[i], 0 };
      draw_label(ctx, d, FONT_KEY_BITHAM_42_BOLD, GRect(box.origin.x, box.origin.y - 2, w, 50), GColorBlack, GTextAlignmentCenter);
      // up/down hints
      draw_label(ctx, "^", FONT_KEY_GOTHIC_18_BOLD, GRect(box.origin.x, box.origin.y - 22, w, 20), s_accent, GTextAlignmentCenter);
    } else {
      graphics_context_set_stroke_color(ctx, GColorDarkGray);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_round_rect(ctx, box, 8);
      if (i < s_pos) {
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, grect_center_point(&box), 7);
      }
    }
  }
  draw_label(ctx, "Select: next  Back: undo", FONT_KEY_GOTHIC_14, GRect(30, 176, b.size.w - 60, 18), GColorLightGray,
             GTextAlignmentCenter);
}

static void up(ClickRecognizerRef r, void *c) { s_digits[s_pos] = (s_digits[s_pos] + 1) % 10; layer_mark_dirty(s_layer); }
static void down(ClickRecognizerRef r, void *c) { s_digits[s_pos] = (s_digits[s_pos] + 9) % 10; layer_mark_dirty(s_layer); }

static void select(ClickRecognizerRef r, void *c) {
  if (s_pos < PIN_LEN - 1) {
    s_pos++;
    s_digits[s_pos] = 0;
    layer_mark_dirty(s_layer);
    return;
  }
  char pin[PIN_LEN + 1];
  for (int i = 0; i < PIN_LEN; i++) pin[i] = '0' + s_digits[i];
  pin[PIN_LEN] = 0;
  PinDone done = s_done;
  void *ctx = s_ctx;
  window_stack_remove(s_win, true);
  if (done) done(pin, ctx);
}

static void back(ClickRecognizerRef r, void *c) {
  if (s_pos == 0) {
    window_stack_remove(s_win, true);
    return;
  }
  s_pos--;
  layer_mark_dirty(s_layer);
}

static void clicks(void *c) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select);
  window_single_click_subscribe(BUTTON_ID_BACK, back);
}

static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
}

static void unload(Window *w) {
  layer_destroy(s_layer);
  window_destroy(s_win);
  s_win = NULL;
}

void pin_push(const char *title, GColor accent, PinDone done, void *ctx) {
  memset(s_digits, 0, sizeof(s_digits));
  s_pos = 0;
  s_title = title;
  s_accent = accent;
  s_done = done;
  s_ctx = ctx;
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers) { .load = load, .unload = unload });
  window_stack_push(s_win, true);
}
