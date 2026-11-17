#include "img.h"
#include "model.h"
#include "comm.h"

static GBitmap *s_building;
static GBitmap *s_current;
static int32_t s_token;
static int32_t s_expect_token;
static uint32_t s_total, s_received;
static uint16_t s_wire_stride;
static ImgReadyHandler s_ready;
static ImgFailHandler s_fail;
static void *s_ctx;
static uint32_t s_req_ms;  // when the current request went out, for frame timing

static uint32_t now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (uint32_t)sec * 1000 + ms;
}

void img_set_handlers(ImgReadyHandler ready, ImgFailHandler fail, void *ctx) {
  s_ready = ready;
  s_fail = fail;
  s_ctx = ctx;
}

static void drop_building(void) {
  if (s_building) {
    gbitmap_destroy(s_building);
    s_building = NULL;
  }
  s_total = 0;
}

void img_release(void) {
  drop_building();
  if (s_current) {
    gbitmap_destroy(s_current);
    s_current = NULL;
  }
}

GBitmap *img_current(void) { return s_current; }

int img_progress(void) {
  if (!s_building || !s_total) return -1;
  return (int)(s_received * 100 / s_total);
}

static int32_t begin_request(void) {
  s_req_ms = now_ms();
  s_expect_token = ++s_token;
  return s_expect_token;
}

int32_t img_request_camera(const char *camera, int w, int h, int bpp, const char *fit) {
  int32_t tok = begin_request();
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_IMAGE);
  dict_write_int32(it, MESSAGE_KEY_iTok, tok);
  dict_write_cstring(it, MESSAGE_KEY_rCam, camera);
  dict_write_int32(it, MESSAGE_KEY_rW, w);
  dict_write_int32(it, MESSAGE_KEY_rH, h);
  dict_write_int32(it, MESSAGE_KEY_rBpp, bpp);
  dict_write_cstring(it, MESSAGE_KEY_rFit, fit ? fit : "auto");
  comm_commit();
  return tok;
}

int32_t img_request_event(int32_t event_id, int w, int h, int bpp) {
  int32_t tok = begin_request();
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_IMAGE);
  dict_write_int32(it, MESSAGE_KEY_iTok, tok);
  dict_write_int32(it, MESSAGE_KEY_rEv, event_id);
  dict_write_int32(it, MESSAGE_KEY_rW, w);
  dict_write_int32(it, MESSAGE_KEY_rH, h);
  dict_write_int32(it, MESSAGE_KEY_rBpp, bpp);
  comm_commit();
  return tok;
}

int32_t img_request_media(const char *player, int w, int h, int bpp) {
  int32_t tok = begin_request();
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_IMAGE);
  dict_write_int32(it, MESSAGE_KEY_iTok, tok);
  dict_write_cstring(it, MESSAGE_KEY_rMedia, player);
  dict_write_int32(it, MESSAGE_KEY_rW, w);
  dict_write_int32(it, MESSAGE_KEY_rH, h);
  dict_write_int32(it, MESSAGE_KEY_rBpp, bpp);
  dict_write_cstring(it, MESSAGE_KEY_rFit, "cover");
  comm_commit();
  return tok;
}

static int32_t tuple_int(DictionaryIterator *it, uint32_t key, int32_t dflt) {
  Tuple *t = dict_find(it, key);
  return t ? t->value->int32 : dflt;
}

static void on_begin(DictionaryIterator *it) {
  drop_building();
  int w = tuple_int(it, MESSAGE_KEY_iW, 0);
  int h = tuple_int(it, MESSAGE_KEY_iH, 0);
  int bpp = tuple_int(it, MESSAGE_KEY_iBpp, 4);
  s_wire_stride = tuple_int(it, MESSAGE_KEY_iStride, 0);
  s_total = tuple_int(it, MESSAGE_KEY_iTotal, 0);
  s_received = 0;
  if (w <= 0 || h <= 0 || w > 260 || h > 260 || s_wire_stride == 0) return;

  GSize size = GSize(w, h);
  if (bpp == 8) {
    s_building = gbitmap_create_blank(size, GBitmapFormat8Bit);
  } else {
    GBitmapFormat fmt = bpp == 1 ? GBitmapFormat1BitPalette
                      : bpp == 2 ? GBitmapFormat2BitPalette
                                 : GBitmapFormat4BitPalette;
    Tuple *pal = dict_find(it, MESSAGE_KEY_iPal);
    int colours = 1 << bpp;
    GColor *palette = malloc(sizeof(GColor) * colours);
    if (!palette) return;
    memset(palette, 0xC0, sizeof(GColor) * colours);
    if (pal) memcpy(palette, pal->value->data, pal->length < colours ? pal->length : colours);
    s_building = gbitmap_create_blank_with_palette(size, fmt, palette, true);
    if (!s_building) free(palette);
  }
  if (!s_building) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "no memory for %dx%d@%d (heap free %d)", w, h, bpp, (int)heap_bytes_free());
    if (s_fail) s_fail("Low memory", s_ctx);
  }
}

static void on_chunk(DictionaryIterator *it) {
  if (!s_building) return;
  Tuple *data = dict_find(it, MESSAGE_KEY_iData);
  if (!data) return;
  uint32_t off = tuple_int(it, MESSAGE_KEY_iOff, 0);
  uint8_t *dst = gbitmap_get_data(s_building);
  uint16_t stride = gbitmap_get_bytes_per_row(s_building);
  GSize size = gbitmap_get_bounds(s_building).size;
  const uint8_t *src = data->value->data;
  uint32_t len = data->length;
  // Wire rows are packed at s_wire_stride; memory rows may be padded.
  while (len) {
    uint32_t row = off / s_wire_stride;
    uint32_t col = off % s_wire_stride;
    if (row >= (uint32_t)size.h) break;
    uint32_t n = s_wire_stride - col;
    if (n > len) n = len;
    memcpy(dst + row * stride + col, src, n);
    off += n;
    src += n;
    len -= n;
  }
  s_received += data->length;
}

static void on_end(void) {
  if (!s_building) return;
  APP_LOG(APP_LOG_LEVEL_INFO, "frame %lu bytes in %lu ms", (unsigned long)s_received,
          (unsigned long)(now_ms() - s_req_ms));
  if (s_current) gbitmap_destroy(s_current);
  s_current = s_building;
  s_building = NULL;
  s_total = 0;
  if (s_ready) s_ready(s_current, s_ctx);
}

bool img_handle(DictionaryIterator *it, int32_t cmd) {
  if (cmd < CMD_IMG_BEGIN || (cmd > CMD_IMG_END && cmd != CMD_IMG_FAIL)) return false;
  int32_t tok = tuple_int(it, MESSAGE_KEY_iTok, 0);
  if (tok != s_expect_token) return true;  // stale transfer, ignore
  switch (cmd) {
    case CMD_IMG_BEGIN: on_begin(it); break;
    case CMD_IMG_CHUNK: on_chunk(it); break;
    case CMD_IMG_END: on_end(); break;
    case CMD_IMG_FAIL: {
      drop_building();
      Tuple *m = dict_find(it, MESSAGE_KEY_msg);
      if (s_fail) s_fail(m ? m->value->cstring : "No image", s_ctx);
      break;
    }
  }
  return true;
}
