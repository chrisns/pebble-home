#include "theme.h"

GColor theme_alarm_colour(AlarmState s) {
  switch (s) {
    case ALARM_DISARMED: return GColorIslamicGreen;
    case ALARM_ARMED_HOME: return GColorChromeYellow;
    case ALARM_ARMED_NIGHT: return GColorVividViolet;
    case ALARM_ARMED_AWAY: return GColorRed;
    case ALARM_ARMING:
    case ALARM_PENDING: return GColorOrange;
    case ALARM_TRIGGERED: return GColorRed;
    default: return GColorDarkGray;
  }
}

bool theme_alarm_blinks(AlarmState s) {
  return s == ALARM_ARMING || s == ALARM_PENDING || s == ALARM_TRIGGERED;
}

GColor theme_kind_colour(const char *k) {
  if (strcmp(k, "doorbell") == 0) return GColorChromeYellow;
  if (strcmp(k, "person") == 0) return GColorOrange;
  if (strcmp(k, "package") == 0) return GColorPictonBlue;
  if (strcmp(k, "alarm") == 0) return GColorRed;
  return GColorVividCerulean;
}

IconKind theme_kind_icon(const char *k) {
  if (strcmp(k, "doorbell") == 0) return ICON_BELL;
  if (strcmp(k, "person") == 0) return ICON_PERSON;
  if (strcmp(k, "package") == 0) return ICON_BOX;
  if (strcmp(k, "alarm") == 0) return ICON_SHIELD;
  return ICON_HOME;
}

static const uint32_t DOORBELL_SEG[] = { 180, 120, 420 };
static const uint32_t ALARM_SEG[] = { 500, 150, 500, 150, 500, 150, 900 };

void theme_vibe(const char *v) {
  if (!v || !v[0] || strcmp(v, "none") == 0) return;
  if (strcmp(v, "short") == 0) {
    vibes_short_pulse();
  } else if (strcmp(v, "double") == 0) {
    vibes_double_pulse();
  } else if (strcmp(v, "long") == 0) {
    vibes_long_pulse();
  } else if (strcmp(v, "doorbell") == 0) {
    VibePattern p = { .durations = DOORBELL_SEG, .num_segments = ARRAY_LENGTH(DOORBELL_SEG) };
    vibes_enqueue_custom_pattern(p);
  } else if (strcmp(v, "alarm") == 0) {
    VibePattern p = { .durations = ALARM_SEG, .num_segments = ARRAY_LENGTH(ALARM_SEG) };
    vibes_enqueue_custom_pattern(p);
  }
}

// Icons are drawn from primitives so they scale and take any colour.
void theme_icon(GContext *ctx, IconKind icon, GPoint c, int r, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, r > 10 ? 2 : 1);
  switch (icon) {
    case ICON_BELL: {
      // dome, flared skirt, clapper
      graphics_fill_circle(ctx, GPoint(c.x, c.y - r / 4), r * 3 / 5);
      graphics_fill_rect(ctx, GRect(c.x - r * 3 / 5, c.y - r / 4, r * 6 / 5, r * 3 / 5), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(c.x - r * 4 / 5, c.y + r / 3, r * 8 / 5, r / 4 + 1), 2, GCornersAll);
      graphics_fill_circle(ctx, GPoint(c.x, c.y + r * 3 / 4), r / 5 + 1);
      break;
    }
    case ICON_PERSON:
      graphics_fill_circle(ctx, GPoint(c.x, c.y - r / 2), r / 3 + 1);
      graphics_fill_rect(ctx, GRect(c.x - r * 3 / 5, c.y, r * 6 / 5, r * 4 / 5), r / 3, GCornersTop);
      break;
    case ICON_BOX:
      graphics_fill_rect(ctx, GRect(c.x - r * 3 / 4, c.y - r / 2, r * 3 / 2, r * 5 / 4), 2, GCornersAll);
      graphics_context_set_stroke_color(ctx, GColorBlack);
      graphics_draw_line(ctx, GPoint(c.x - r * 3 / 4, c.y - r / 8), GPoint(c.x + r * 3 / 4, c.y - r / 8));
      graphics_draw_line(ctx, GPoint(c.x, c.y - r / 2), GPoint(c.x, c.y - r / 8));
      break;
    case ICON_SHIELD: {
      GPathInfo info = { .num_points = 5, .points = (GPoint[]) {
        { -r * 3 / 4, -r * 3 / 4 }, { r * 3 / 4, -r * 3 / 4 }, { r * 3 / 4, 0 }, { 0, r }, { -r * 3 / 4, 0 } } };
      GPath *p = gpath_create(&info);
      gpath_move_to(p, c);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      break;
    }
    case ICON_LOCK:
    case ICON_UNLOCK: {
      graphics_fill_rect(ctx, GRect(c.x - r * 3 / 4, c.y - r / 6, r * 3 / 2, r), 2, GCornersAll);
      graphics_context_set_stroke_width(ctx, r > 10 ? 3 : 2);
      int sx = icon == ICON_UNLOCK ? r / 2 : 0;
      GRect arc = GRect(c.x - r / 2 + sx, c.y - r, r, r * 3 / 2);
      graphics_draw_arc(ctx, arc, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(-90), DEG_TO_TRIGANGLE(90));
      break;
    }
    case ICON_CAMERA:
      graphics_fill_rect(ctx, GRect(c.x - r, c.y - r / 2, r * 3 / 2, r), 3, GCornersAll);
      graphics_fill_rect(ctx, GRect(c.x + r / 2, c.y - r / 4, r / 2, r / 2), 0, GCornerNone);
      break;
    case ICON_NOTE:
      graphics_fill_circle(ctx, GPoint(c.x - r / 3, c.y + r / 2), r / 3 + 1);
      graphics_fill_rect(ctx, GRect(c.x - r / 3 + r / 4, c.y - r * 3 / 4, 3, r * 5 / 4), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(c.x - r / 3 + r / 4, c.y - r * 3 / 4, r * 2 / 3, r / 4), 0, GCornerNone);
      break;
    case ICON_HOME: {
      GPathInfo info = { .num_points = 5, .points = (GPoint[]) {
        { 0, -r }, { r, 0 }, { r * 2 / 3, 0 }, { r * 2 / 3, r * 3 / 4 }, { -r * 2 / 3, r * 3 / 4 } } };
      GPath *p = gpath_create(&info);
      gpath_move_to(p, c);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      GPathInfo l = { .num_points = 3, .points = (GPoint[]) { { 0, -r }, { -r, 0 }, { -r * 2 / 3, 0 } } };
      p = gpath_create(&l);
      gpath_move_to(p, c);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      break;
    }
    case ICON_MIC:
      graphics_fill_rect(ctx, GRect(c.x - r / 3, c.y - r, r * 2 / 3, r * 5 / 4), r / 3, GCornersAll);
      graphics_draw_arc(ctx, GRect(c.x - r * 2 / 3, c.y - r / 2, r * 4 / 3, r), GOvalScaleModeFitCircle,
                        DEG_TO_TRIGANGLE(90), DEG_TO_TRIGANGLE(270));
      graphics_draw_line(ctx, GPoint(c.x, c.y + r / 2), GPoint(c.x, c.y + r));
      break;
    case ICON_BOLT: {
      GPathInfo info = { .num_points = 6, .points = (GPoint[]) {
        { r / 4, -r }, { -r / 2, r / 6 }, { 0, r / 6 }, { -r / 4, r }, { r / 2, -r / 6 }, { 0, -r / 6 } } };
      GPath *p = gpath_create(&info);
      gpath_move_to(p, c);
      gpath_draw_filled(ctx, p);
      gpath_destroy(p);
      break;
    }
    case ICON_LIST:
      for (int i = -1; i <= 1; i++) {
        graphics_fill_rect(ctx, GRect(c.x - r * 3 / 4, c.y + i * r / 2 - 1, r * 3 / 2, 3), 1, GCornersAll);
      }
      break;
  }
}
