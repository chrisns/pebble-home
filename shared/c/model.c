#include "model.h"

HomeModel g_home = { .configured = true };

AlarmState alarm_from_string(const char *s) {
  if (!s || !s[0]) return ALARM_UNKNOWN;
  if (strcmp(s, "disarmed") == 0) return ALARM_DISARMED;
  if (strcmp(s, "armed_home") == 0) return ALARM_ARMED_HOME;
  if (strcmp(s, "armed_night") == 0) return ALARM_ARMED_NIGHT;
  if (strncmp(s, "armed", 5) == 0) return ALARM_ARMED_AWAY;
  if (strcmp(s, "arming") == 0) return ALARM_ARMING;
  if (strcmp(s, "pending") == 0) return ALARM_PENDING;
  if (strcmp(s, "triggered") == 0) return ALARM_TRIGGERED;
  return ALARM_UNKNOWN;
}

static void copy_str(DictionaryIterator *it, uint32_t key, char *dst, size_t len) {
  Tuple *t = dict_find(it, key);
  if (t && t->type == TUPLE_CSTRING) {
    strncpy(dst, t->value->cstring, len - 1);
    dst[len - 1] = '\0';
  }
}

static bool read_int(DictionaryIterator *it, uint32_t key, int32_t *out) {
  Tuple *t = dict_find(it, key);
  if (!t) return false;
  *out = t->value->int32;
  return true;
}

void model_apply_state(DictionaryIterator *it) {
  g_home.online = true;
  g_home.configured = true;
  g_home.error[0] = '\0';
  g_home.synced_at = time(NULL);
  read_int(it, MESSAGE_KEY_ver, &g_home.version);
  // A heartbeat carries only ver/online; full state carries the alarm.
  if (!dict_find(it, MESSAGE_KEY_ppl)) return;
  g_home.have_state = true;

  copy_str(it, MESSAGE_KEY_aS, g_home.alarm_code, sizeof(g_home.alarm_code));
  g_home.alarm = alarm_from_string(g_home.alarm_code);
  copy_str(it, MESSAGE_KEY_aL, g_home.alarm_label, sizeof(g_home.alarm_label));
  copy_str(it, MESSAGE_KEY_aBy, g_home.alarm_by, sizeof(g_home.alarm_by));
  int32_t v;
  if (read_int(it, MESSAGE_KEY_aR, &v)) g_home.alarm_ready = v;
  read_int(it, MESSAGE_KEY_aT, &g_home.alarm_since);
  int32_t rem = 0;
  read_int(it, MESSAGE_KEY_aRem, &rem);
  g_home.alarm_until = rem > 0 ? time(NULL) + rem : 0;
  g_home.alarm_features = 3;  // home + away unless the house says otherwise
  read_int(it, MESSAGE_KEY_aF, &g_home.alarm_features);
  g_home.alarm_to[0] = '\0';
  copy_str(it, MESSAGE_KEY_aTo, g_home.alarm_to, sizeof(g_home.alarm_to));

  copy_str(it, MESSAGE_KEY_ppl, g_home.people, sizeof(g_home.people));
  copy_str(it, MESSAGE_KEY_locks, g_home.locks, sizeof(g_home.locks));
  read_int(it, MESSAGE_KEY_openN, &g_home.open_n);
  copy_str(it, MESSAGE_KEY_openL, g_home.open_list, sizeof(g_home.open_list));
  g_home.wx_valid = read_int(it, MESSAGE_KEY_wxT, &g_home.wx_temp);
  copy_str(it, MESSAGE_KEY_wxC, g_home.wx_cond, sizeof(g_home.wx_cond));
  copy_str(it, MESSAGE_KEY_st, g_home.status, sizeof(g_home.status));
  copy_str(it, MESSAGE_KEY_cams, g_home.cams, sizeof(g_home.cams));
  copy_str(it, MESSAGE_KEY_acts, g_home.acts, sizeof(g_home.acts));
  if (read_int(it, MESSAGE_KEY_pinLock, &v)) g_home.pin_locked = v;
  if (read_int(it, MESSAGE_KEY_pinReq, &v)) g_home.pin_required = v;
  copy_str(it, MESSAGE_KEY_media, g_home.media, sizeof(g_home.media));
  copy_str(it, MESSAGE_KEY_mediaT, g_home.media_t, sizeof(g_home.media_t));
}

void model_read_event(DictionaryIterator *it, HomeEvent *ev) {
  memset(ev, 0, sizeof(*ev));
  read_int(it, MESSAGE_KEY_eId, &ev->id);
  read_int(it, MESSAGE_KEY_eTs, &ev->ts);
  copy_str(it, MESSAGE_KEY_eK, ev->kind, sizeof(ev->kind));
  copy_str(it, MESSAGE_KEY_eT, ev->title, sizeof(ev->title));
  copy_str(it, MESSAGE_KEY_eM, ev->msg, sizeof(ev->msg));
  copy_str(it, MESSAGE_KEY_eC, ev->cam, sizeof(ev->cam));
  copy_str(it, MESSAGE_KEY_eV, ev->vibe, sizeof(ev->vibe));
  int32_t v;
  if (read_int(it, MESSAGE_KEY_eI, &v)) ev->has_img = v;
  if (read_int(it, MESSAGE_KEY_eAlert, &v)) ev->alert = v;
  copy_str(it, MESSAGE_KEY_eA, ev->acts, sizeof(ev->acts));
  read_int(it, MESSAGE_KEY_eAf, &ev->act_flags);
}

int list_count(const char *list) {
  if (!list || !list[0]) return 0;
  int n = 1;
  for (const char *p = list; *p; p++) {
    if (*p == '|') n++;
  }
  return n;
}

bool list_field(const char *list, int index, int field, char *out, size_t out_len) {
  if (!out_len) return false;
  out[0] = '\0';
  if (!list) return false;
  const char *p = list;
  for (int i = 0; i < index; i++) {
    p = strchr(p, '|');
    if (!p) return false;
    p++;
  }
  for (int f = 0; f < field; f++) {
    const char *colon = strchr(p, ':');
    const char *bar = strchr(p, '|');
    if (!colon || (bar && colon > bar)) return false;
    p = colon + 1;
  }
  size_t n = 0;
  while (p[n] && p[n] != ':' && p[n] != '|') n++;
  if (n >= out_len) n = out_len - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

void fmt_ago(time_t ts, char *buf, size_t len) {
  int d = (int)(time(NULL) - ts);
  if (d < 60) {
    snprintf(buf, len, "just now");
  } else if (d < 3600) {
    snprintf(buf, len, "%dm ago", d / 60);
  } else if (d < 86400) {
    snprintf(buf, len, "%dh ago", d / 3600);
  } else {
    snprintf(buf, len, "%dd ago", d / 86400);
  }
}

int alarm_remaining(void) {
  if (!g_home.alarm_until) return 0;
  if (g_home.alarm != ALARM_ARMING && g_home.alarm != ALARM_PENDING) return 0;
  int left = (int)(g_home.alarm_until - time(NULL));
  return left > 0 ? left : 0;
}
