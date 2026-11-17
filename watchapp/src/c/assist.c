// Assist: speak to Home Assistant through the watch microphone.
#include "app.h"

static DictationSession *s_dictation;
static bool s_waiting;
static Window *s_win;
static ScrollLayer *s_scroll;
static TextLayer *s_text;
static char s_question[200];
static char s_answer[420];

static void show_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_scroll, w);
  scroll_layer_set_shadow_hidden(s_scroll, true);
  s_text = text_layer_create(GRect(0, 0, b.size.w, 2000));
  text_layer_set_background_color(s_text, GColorClear);
  text_layer_set_text_color(s_text, GColorWhite);
  text_layer_set_font(s_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_text, GTextAlignmentCenter);
  text_layer_set_text(s_text, s_answer);
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_text));
  layer_add_child(root, scroll_layer_get_layer(s_scroll));
  // Flow text inside the circle and page it on round screens.
  text_layer_enable_screen_text_flow_and_paging(s_text, 6);
  scroll_layer_set_paging(s_scroll, true);
  GSize sz = text_layer_get_content_size(s_text);
  scroll_layer_set_content_size(s_scroll, GSize(b.size.w, sz.h + 60));
}

static void show_unload(Window *w) {
  text_layer_destroy(s_text);
  scroll_layer_destroy(s_scroll);
  window_destroy(s_win);
  s_win = NULL;
}

static void show(const char *text) {
  snprintf(s_answer, sizeof(s_answer), "\n%s", text);
  if (s_win) {
    text_layer_set_text(s_text, s_answer);
    return;
  }
  s_win = window_create();
  window_set_background_color(s_win, GColorDukeBlue);
  window_set_window_handlers(s_win, (WindowHandlers) { .load = show_load, .unload = show_unload });
  window_stack_push(s_win, true);
}

static void dictated(DictationSession *session, DictationSessionStatus status, char *transcription, void *ctx) {
  if (status != DictationSessionStatusSuccess) return;
  strncpy(s_question, transcription, sizeof(s_question) - 1);
  s_waiting = true;
  comm_action("assist", NULL, NULL, NULL, s_question);
  char buf[240];
  snprintf(buf, sizeof(buf), "\"%s\"\n\nAsking…", s_question);
  show(buf);
}

void assist_start(void) {
  if (!s_dictation) s_dictation = dictation_session_create(sizeof(s_question), dictated, NULL);
  if (!s_dictation) {
    toast("No microphone", false);
    return;
  }
  dictation_session_enable_confirmation(s_dictation, false);
  dictation_session_start(s_dictation);
}

bool assist_handle_result(DictionaryIterator *it) {
  if (!s_waiting) return false;
  s_waiting = false;
  Tuple *msg = dict_find(it, MESSAGE_KEY_msg);
  vibes_short_pulse();
  show(msg ? msg->value->cstring : "No answer");
  return true;
}
