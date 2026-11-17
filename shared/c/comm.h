// AppMessage plumbing shared by the face and the app. Outbound messages are
// queued so a button press never gets lost while another message is in flight.
#pragma once
#include <pebble.h>

typedef void (*CommHandler)(DictionaryIterator *it, int32_t cmd);

void comm_init(CommHandler handler, uint32_t inbox);
// Start a message; write tuples into the iterator, then comm_commit().
DictionaryIterator *comm_draft(void);
void comm_commit(void);
void comm_sync(bool force);
void comm_hello(void);
void comm_action(const char *kind, const char *mode, const char *id, const char *code, const char *text);
void comm_events(void);
// A notification button. event_ref: the event id, or its low 28 bits.
void comm_notify_action(int32_t event_ref, int index, bool guarded, const char *code);
