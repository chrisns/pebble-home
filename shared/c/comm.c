#include "comm.h"
#include "model.h"
#include "img.h"

#define QMAX 6
#define QBUF 320

static CommHandler s_handler;
static bool s_busy;
static uint8_t s_q[QMAX][QBUF];
static uint16_t s_qlen[QMAX];
static int32_t s_qcmd[QMAX];
static int s_qn;
static uint8_t s_draft[QBUF];
static DictionaryIterator s_draft_it;
static AppTimer *s_retry;

static void pump(void);

static void inbox_received(DictionaryIterator *it, void *ctx) {
  Tuple *cmd = dict_find(it, MESSAGE_KEY_cmd);
  if (!cmd) return;
  int32_t c = cmd->value->int32;
  if (img_handle(it, c)) {
    if (s_handler) s_handler(it, c);
    return;
  }
  switch (c) {
    case CMD_STATE: model_apply_state(it); break;
    case CMD_EVENT: model_read_event(it, &g_home.last); break;
    case CMD_NOCONFIG: g_home.configured = false; break;
    case CMD_ERROR: {
      Tuple *m = dict_find(it, MESSAGE_KEY_msg);
      g_home.online = false;
      strncpy(g_home.error, m ? m->value->cstring : "Offline", sizeof(g_home.error) - 1);
      break;
    }
  }
  if (s_handler) s_handler(it, c);
}

static void dequeue(void) {
  if (!s_qn) return;
  for (int i = 1; i < s_qn; i++) {
    memcpy(s_q[i - 1], s_q[i], s_qlen[i]);
    s_qlen[i - 1] = s_qlen[i];
    s_qcmd[i - 1] = s_qcmd[i];
  }
  s_qn--;
}

static void retry_cb(void *ctx) {
  s_retry = NULL;
  pump();
}

static void outbox_sent(DictionaryIterator *it, void *ctx) {
  s_busy = false;
  dequeue();
  pump();
}

static void outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *ctx) {
  s_busy = false;
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed %d", reason);
  if (reason == APP_MSG_NOT_CONNECTED || reason == APP_MSG_SEND_TIMEOUT) g_home.online = false;
  // Drop it if the phone is gone; otherwise try again shortly.
  if (reason == APP_MSG_NOT_CONNECTED) dequeue();
  if (!s_retry) s_retry = app_timer_register(400, retry_cb, NULL);
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped %d", reason);
}

// Copy a stored dictionary into the outbox, tuple by tuple.
static void pump(void) {
  if (s_busy || !s_qn) return;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    if (!s_retry) s_retry = app_timer_register(200, retry_cb, NULL);
    return;
  }
  DictionaryIterator rd;
  for (Tuple *t = dict_read_begin_from_buffer(&rd, s_q[0], s_qlen[0]); t; t = dict_read_next(&rd)) {
    switch (t->type) {
      case TUPLE_CSTRING: dict_write_cstring(out, t->key, t->value->cstring); break;
      case TUPLE_INT: dict_write_int32(out, t->key, t->value->int32); break;
      case TUPLE_UINT: dict_write_uint32(out, t->key, t->value->uint32); break;
      case TUPLE_BYTE_ARRAY: dict_write_data(out, t->key, t->value->data, t->length); break;
    }
  }
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_busy = true;
  } else if (!s_retry) {
    s_retry = app_timer_register(200, retry_cb, NULL);
  }
}

void comm_init(CommHandler handler, uint32_t inbox) {
  s_handler = handler;
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  uint32_t max_in = app_message_inbox_size_maximum();
  app_message_open(inbox < max_in ? inbox : max_in, 512);
}

DictionaryIterator *comm_draft(void) {
  dict_write_begin(&s_draft_it, s_draft, sizeof(s_draft));
  return &s_draft_it;
}

void comm_commit(void) {
  uint32_t len = dict_write_end(&s_draft_it);
  if (!len) return;
  DictionaryIterator rd;
  Tuple *c = dict_read_begin_from_buffer(&rd, s_draft, len);
  int32_t cmd = 0;
  for (; c; c = dict_read_next(&rd)) {
    if (c->key == MESSAGE_KEY_cmd) cmd = c->value->int32;
  }
  // A newer sync or image request replaces a queued one that has not gone yet.
  int start = s_busy ? 1 : 0;
  if (cmd == CMD_SYNC || cmd == CMD_IMAGE) {
    for (int i = start; i < s_qn; i++) {
      if (s_qcmd[i] == cmd) {
        memcpy(s_q[i], s_draft, len);
        s_qlen[i] = len;
        return;
      }
    }
  }
  if (s_qn == QMAX) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox queue full, dropping cmd %d", (int)cmd);
    return;
  }
  memcpy(s_q[s_qn], s_draft, len);
  s_qlen[s_qn] = len;
  s_qcmd[s_qn] = cmd;
  s_qn++;
  pump();
}

void comm_sync(bool force) {
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_SYNC);
  dict_write_int32(it, MESSAGE_KEY_seq, force ? 1 : 0);
  comm_commit();
}

void comm_hello(void) {
  BatteryChargeState b = battery_state_service_peek();
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_HELLO);
  dict_write_int32(it, MESSAGE_KEY_bat, b.charge_percent);
  dict_write_int32(it, MESSAGE_KEY_chg, b.is_charging ? 1 : 0);
  comm_commit();
}

void comm_action(const char *kind, const char *mode, const char *id, const char *code, const char *text) {
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_ACTION);
  dict_write_cstring(it, MESSAGE_KEY_xK, kind);
  if (mode) dict_write_cstring(it, MESSAGE_KEY_xM, mode);
  if (id) dict_write_cstring(it, MESSAGE_KEY_xId, id);
  if (code) dict_write_cstring(it, MESSAGE_KEY_xCode, code);
  if (text) dict_write_cstring(it, MESSAGE_KEY_xText, text);
  comm_commit();
}

void comm_notify_action(int32_t event_ref, int index, bool guarded, const char *code) {
  char ref[12], idx[4];
  snprintf(ref, sizeof(ref), "%ld", (long)event_ref);
  snprintf(idx, sizeof(idx), "%d", index);
  comm_action("notify", idx, ref, code, guarded ? "guard" : NULL);
}

void comm_events(void) {
  DictionaryIterator *it = comm_draft();
  dict_write_int32(it, MESSAGE_KEY_cmd, CMD_EVENTS);
  comm_commit();
}
