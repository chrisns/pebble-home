// Home watchapp: windows and shared helpers.
#pragma once
#include <pebble.h>
#include "shared/model.h"
#include "shared/comm.h"
#include "shared/img.h"
#include "shared/theme.h"

// Each window that cares about inbound messages exposes a hook; main.c fans out.
void main_menu_push(void);
void main_menu_on_message(DictionaryIterator *it, int32_t cmd);

typedef void (*PinDone)(const char *pin, void *ctx);
void pin_push(const char *title, GColor accent, PinDone done, void *ctx);

void camera_push(int index);          // live camera from g_home.cams
void camera_push_event(int32_t event_id, const char *title, const char *msg, const char *kind);
void camera_push_ev(const HomeEvent *ev);  // with its notification buttons
// Doorbell: live video, every button unlocks, closes 45 s after the ring
// (and leaves the app if exit_after).
void camera_push_doorbell(const HomeEvent *ev, bool unlock_now, bool exit_after);
void camera_on_message(DictionaryIterator *it, int32_t cmd);

void music_push(int index);
// First playing speaker (or -1); how_many gets how many are playing.
int music_playing_index(int *how_many);
void music_on_message(DictionaryIterator *it, int32_t cmd);

void alarm_push(void);
void alarm_on_message(DictionaryIterator *it, int32_t cmd);
bool alarm_is_open(void);

typedef enum { LIST_EVENTS, LIST_LOCKS, LIST_ACTIONS, LIST_CAMERAS, LIST_STATUS, LIST_SPEAKERS } ListKind;
void list_push(ListKind kind);
void list_on_message(DictionaryIterator *it, int32_t cmd);

void assist_start(void);
// Returns true if the result was for an Assist question.
bool assist_handle_result(DictionaryIterator *it);

// A transient message over whatever is showing.
void toast(const char *text, bool ok);

// Draw centred text in a box.
void draw_label(GContext *ctx, const char *txt, const char *font, GRect box, GColor col, GTextAlignment al);
