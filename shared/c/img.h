// Receives a chunked, pre-converted bitmap from the phone.
#pragma once
#include <pebble.h>

typedef void (*ImgReadyHandler)(GBitmap *bitmap, void *ctx);
typedef void (*ImgFailHandler)(const char *why, void *ctx);

void img_set_handlers(ImgReadyHandler ready, ImgFailHandler fail, void *ctx);
// Ask for a live camera frame or an event snapshot. Returns the token used.
int32_t img_request_camera(const char *camera, int w, int h, int bpp, const char *fit);
int32_t img_request_event(int32_t event_id, int w, int h, int bpp);
int32_t img_request_media(const char *player, int w, int h, int bpp);
// Feed every inbound message through this; returns true if it was an image message.
bool img_handle(DictionaryIterator *it, int32_t cmd);
// Progress 0..100 of the transfer in flight, -1 if idle.
int img_progress(void);
// The last complete bitmap (owned by img.c; valid until the next one lands).
GBitmap *img_current(void);
void img_release(void);
