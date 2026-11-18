// Home state as the watch sees it, filled from AppMessages.
#pragma once
#include <pebble.h>

enum {
  CMD_SYNC = 1, CMD_HELLO = 2, CMD_IMAGE = 3, CMD_ACTION = 4, CMD_EVENTS = 5,
  CMD_STATE = 10, CMD_EVENT = 11, CMD_IMG_BEGIN = 12, CMD_IMG_CHUNK = 13, CMD_IMG_END = 14,
  CMD_RESULT = 15, CMD_EVITEM = 16, CMD_ERROR = 17, CMD_NOCONFIG = 18, CMD_IMG_FAIL = 19
};

typedef enum {
  ALARM_UNKNOWN = 0,
  ALARM_DISARMED,
  ALARM_ARMED_HOME,
  ALARM_ARMED_NIGHT,
  ALARM_ARMED_AWAY,
  ALARM_ARMING,
  ALARM_PENDING,
  ALARM_TRIGGERED,
} AlarmState;

typedef struct {
  int32_t id;
  int32_t ts;
  char kind[12];
  char title[34];
  char msg[82];
  char cam[40];
  char vibe[10];
  bool has_img;
  bool alert;
  char acts[92];      // notification buttons: "Unlock the door|..."
  int32_t act_flags;  // bit n: button n needs a confirm (or the PIN) first
} HomeEvent;

typedef struct {
  bool have_state;
  bool configured;
  bool online;
  char error[24];
  time_t synced_at;
  int32_t version;

  AlarmState alarm;
  char alarm_code[20];
  char alarm_label[20];
  bool alarm_ready;
  char alarm_by[22];
  int32_t alarm_since;
  time_t alarm_until;   // local time the exit/entry delay ends, 0 if none
  int32_t alarm_features; // 1 home, 2 away, 4 night
  char alarm_to[8];     // where arming is heading: away, home, night

  char people[80];   // "Chris:1|Sam:0"
  char locks[160];   // "lock.front:Front door:locked|..."
  int32_t open_n;
  char open_list[120];
  bool wx_valid;
  int32_t wx_temp;
  char wx_cond[22];
  int32_t print_pct;    // -1 when nothing is printing
  int32_t print_eta;    // epoch the print should finish, 0 unknown
  bool print_paused;
  char upd[16];         // "face,app": builds older than the integration ships
  char status[200];  // "Solar:2.1kW|..."
  char cams[300];    // "camera.porch:Porch|..."
  char acts[420];    // "script.night:Good night|..."
  bool pin_locked;
  bool pin_required;
  char media[1100];   // "id:name:state:vol:mute:lead:art|..."
  char media_t[1400]; // "title:artist|..." (same order)

  HomeEvent last;
} HomeModel;

extern HomeModel g_home;

AlarmState alarm_from_string(const char *s);
void model_apply_state(DictionaryIterator *it);
void model_read_event(DictionaryIterator *it, HomeEvent *ev);

// Iterate "a:b:c|a:b:c" lists. Returns number of items; copies field `field`
// of item `index` into out.
int list_count(const char *list);
bool list_field(const char *list, int index, int field, char *out, size_t out_len);

void fmt_ago(time_t ts, char *buf, size_t len);
// Seconds left on the exit/entry delay, or 0.
int alarm_remaining(void);
