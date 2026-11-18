// Simple lists: cameras, events, doors and locks, quick actions, status.
#include "app.h"

#define MAX_EVENTS 15

static Window *s_win;
static MenuLayer *s_menu;
static ListKind s_kind;
static HomeEvent s_events[MAX_EVENTS];
static int s_event_n;
static bool s_events_loading;
static char s_pending_lock[40];
static time_t s_confirm_until;

static int rows(void) {
  switch (s_kind) {
    case LIST_CAMERAS: return list_count(g_home.cams);
    case LIST_SPEAKERS: return list_count(g_home.media);
    case LIST_EVENTS: return s_event_n ? s_event_n : 1;
    case LIST_LOCKS: return list_count(g_home.locks) + list_count(g_home.open_list);
    case LIST_ACTIONS: return list_count(g_home.acts);
    case LIST_STATUS: return list_count(g_home.people) + list_count(g_home.status);
  }
  return 0;
}

static uint16_t num_rows(MenuLayer *m, uint16_t s, void *c) { int n = rows(); return n ? n : 1; }

static int16_t row_height(MenuLayer *m, MenuIndex *i, void *c) {
  return menu_layer_is_index_selected(m, i) ? 64 : 40;
}

static void row_text(int i, char *title, size_t tl, char *sub, size_t sl, IconKind *icon, GColor *accent) {
  title[0] = sub[0] = 0;
  *icon = ICON_HOME;
  *accent = GColorVividCerulean;
  switch (s_kind) {
    case LIST_CAMERAS:
      list_field(g_home.cams, i, 1, title, tl);
      snprintf(sub, sl, "Live view");
      *icon = ICON_CAMERA;
      *accent = GColorPictonBlue;
      break;
    case LIST_SPEAKERS: {
      char st[12], t[42], a[32];
      list_field(g_home.media, i, 1, title, tl);
      list_field(g_home.media, i, 2, st, sizeof(st));
      list_field(g_home.media_t, i, 0, t, sizeof(t));
      list_field(g_home.media_t, i, 1, a, sizeof(a));
      bool playing = strcmp(st, "playing") == 0;
      if (t[0]) snprintf(sub, sl, "%s%s", playing ? "" : "Paused · ", t);
      else snprintf(sub, sl, "%s", strcmp(st, "idle") == 0 || strcmp(st, "off") == 0 ? "Idle" : st);
      *icon = ICON_NOTE;
      *accent = playing ? GColorMagenta : GColorDarkGray;
      if (!playing && t[0]) *accent = GColorPurple;
      break;
    }
    case LIST_EVENTS: {
      if (!s_event_n) {
        snprintf(title, tl, s_events_loading ? "Loading…" : "No events");
        break;
      }
      HomeEvent *e = &s_events[i];
      char ago[12];
      fmt_ago(e->ts, ago, sizeof(ago));
      snprintf(title, tl, "%s", e->title);
      snprintf(sub, sl, "%s%s%s", e->msg, e->msg[0] ? " · " : "", ago);
      *icon = theme_kind_icon(e->kind);
      *accent = theme_kind_colour(e->kind);
      break;
    }
    case LIST_LOCKS: {
      int nl = list_count(g_home.locks);
      if (i < nl) {
        char st[16];
        list_field(g_home.locks, i, 1, title, tl);
        list_field(g_home.locks, i, 2, st, sizeof(st));
        bool locked = strcmp(st, "locked") == 0;
        if (locked && time(NULL) <= s_confirm_until && strcmp(s_pending_lock, "") != 0) {
          snprintf(sub, sl, "Select again to unlock");
        } else {
          snprintf(sub, sl, "%s · select to %s", st, locked ? "unlock" : "lock");
        }
        *icon = locked ? ICON_LOCK : ICON_UNLOCK;
        *accent = locked ? GColorMediumSpringGreen : GColorOrange;
      } else {
        list_field(g_home.open_list, i - nl, 0, title, tl);
        snprintf(sub, sl, "Open");
        *icon = ICON_HOME;
        *accent = GColorOrange;
      }
      break;
    }
    case LIST_ACTIONS:
      list_field(g_home.acts, i, 1, title, tl);
      snprintf(sub, sl, "Select to run");
      *icon = ICON_BOLT;
      *accent = GColorYellow;
      break;
    case LIST_STATUS: {
      int np = list_count(g_home.people);
      if (i < np) {
        char h[3];
        list_field(g_home.people, i, 0, title, tl);
        list_field(g_home.people, i, 1, h, sizeof(h));
        snprintf(sub, sl, h[0] == '1' ? "Home" : "Away");
        *icon = ICON_PERSON;
        *accent = h[0] == '1' ? GColorMediumSpringGreen : GColorDarkGray;
      } else {
        list_field(g_home.status, i - np, 1, title, tl);
        list_field(g_home.status, i - np, 0, sub, sl);
        *icon = ICON_LIST;
        *accent = GColorLightGray;
      }
      break;
    }
  }
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  GRect b = layer_get_bounds(cell);
  char title[48], sub[64];
  IconKind icon;
  GColor accent;
  if (!rows() && s_kind != LIST_EVENTS) {
    draw_label(ctx, g_home.have_state ? "Nothing here" : "Loading…", FONT_KEY_GOTHIC_24_BOLD, GRect(0, b.size.h / 2 - 16, b.size.w, 28),
               GColorWhite, GTextAlignmentCenter);
    return;
  }
  row_text(idx->row, title, sizeof(title), sub, sizeof(sub), &icon, &accent);
  if (menu_cell_layer_is_highlighted(cell)) {
    graphics_context_set_fill_color(ctx, accent);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    theme_icon(ctx, icon, GPoint(b.size.w / 2, 14), 8, GColorBlack);
    draw_label(ctx, title, FONT_KEY_GOTHIC_24_BOLD, GRect(24, 20, b.size.w - 48, 28), GColorBlack, GTextAlignmentCenter);
    draw_label(ctx, sub, FONT_KEY_GOTHIC_14_BOLD, GRect(34, 44, b.size.w - 68, 18), GColorBlack, GTextAlignmentCenter);
  } else {
    draw_label(ctx, title, FONT_KEY_GOTHIC_18_BOLD, GRect(40, b.size.h / 2 - 12, b.size.w - 80, 22), GColorWhite,
               GTextAlignmentCenter);
  }
}

static void unlock_pin(const char *pin, void *ctx) {
  comm_action("lock", "unlock", s_pending_lock, pin, NULL);
}

static void select_row(MenuLayer *m, MenuIndex *idx, void *c) {
  int i = idx->row;
  switch (s_kind) {
    case LIST_CAMERAS:
      camera_push(i);
      break;
    case LIST_SPEAKERS:
      music_push(i);
      break;
    case LIST_EVENTS:
      if (s_event_n && (s_events[i].has_img || s_events[i].acts[0])) camera_push_ev(&s_events[i]);
      break;
    case LIST_LOCKS: {
      if (i >= list_count(g_home.locks)) break;
      char st[16];
      list_field(g_home.locks, i, 0, s_pending_lock, sizeof(s_pending_lock));
      list_field(g_home.locks, i, 2, st, sizeof(st));
      if (strcmp(st, "locked") == 0 && g_home.pin_required) {
        pin_push("Unlock", GColorOrange, unlock_pin, NULL);
      } else if (strcmp(st, "locked") == 0) {
        if (time(NULL) <= s_confirm_until) {
          s_confirm_until = 0;
          unlock_pin(NULL, NULL);
        } else {
          // No PIN: a second press confirms. Shown in the row, not a toast,
          // so the toast window can't swallow that second press.
          s_confirm_until = time(NULL) + 3;
          vibes_short_pulse();
          menu_layer_reload_data(s_menu);
        }
      } else {
        comm_action("lock", "lock", s_pending_lock, NULL, NULL);
      }
      break;
    }
    case LIST_ACTIONS: {
      char id[40];
      list_field(g_home.acts, i, 0, id, sizeof(id));
      comm_action("run", NULL, id, NULL, NULL);
      break;
    }
    case LIST_STATUS:
      break;
  }
}

static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = num_rows, .get_cell_height = row_height, .draw_row = draw_row, .select_click = select_row });
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, GColorBlack, GColorBlack);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void unload(Window *w) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  window_destroy(s_win);
  s_win = NULL;
}

void list_push(ListKind kind) {
  s_kind = kind;
  if (kind == LIST_EVENTS) {
    s_event_n = 0;
    s_events_loading = true;
    comm_events();
  }
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_window_handlers(s_win, (WindowHandlers) { .load = load, .unload = unload });
  window_stack_push(s_win, true);
  if (kind == LIST_SPEAKERS) {
    int playing = music_playing_index(NULL);
    if (playing > 0) menu_layer_set_selected_index(s_menu, MenuIndex(0, playing), MenuRowAlignCenter, false);
  }
}

void list_on_message(DictionaryIterator *it, int32_t cmd) {
  if (!s_menu) return;
  if (cmd == CMD_EVITEM && s_kind == LIST_EVENTS) {
    Tuple *idx = dict_find(it, MESSAGE_KEY_eIdx);
    Tuple *cnt = dict_find(it, MESSAGE_KEY_eCount);
    s_events_loading = false;
    if (cnt && cnt->value->int32 == 0) s_event_n = 0;
    if (idx && cnt && cnt->value->int32 > 0 && idx->value->int32 < MAX_EVENTS) {
      model_read_event(it, &s_events[idx->value->int32]);
      if (idx->value->int32 + 1 > s_event_n) s_event_n = idx->value->int32 + 1;
    }
  }
  if (cmd == CMD_STATE || cmd == CMD_EVITEM) menu_layer_reload_data(s_menu);
}
