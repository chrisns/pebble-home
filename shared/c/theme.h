// Colours, vibration patterns and small vector icons.
#pragma once
#include <pebble.h>
#include "model.h"

typedef enum { ICON_BELL, ICON_PERSON, ICON_BOX, ICON_SHIELD, ICON_LOCK, ICON_UNLOCK,
               ICON_CAMERA, ICON_NOTE, ICON_HOME, ICON_MIC, ICON_BOLT, ICON_LIST } IconKind;

GColor theme_alarm_colour(AlarmState s);
bool theme_alarm_blinks(AlarmState s);
GColor theme_kind_colour(const char *kind);
IconKind theme_kind_icon(const char *kind);
void theme_vibe(const char *vibe);
void theme_icon(GContext *ctx, IconKind icon, GPoint c, int r, GColor colour);
