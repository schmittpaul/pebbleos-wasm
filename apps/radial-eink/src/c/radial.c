#include "radial.h"

Dial dial_make(GRect bounds) {
  const int16_t usable_h = (int16_t)(bounds.size.h - DIAL_HINT_H);
  const int16_t span = bounds.size.w < usable_h ? bounds.size.w : usable_h;
  const int16_t r = (int16_t)(span / 2 - 6);
  Dial d = {
    .center           = GPoint(bounds.size.w / 2, usable_h / 2),
    .tick_outer       = r,
    .tick_inner_minor = (int16_t)(r * 91 / 100),
    .tick_inner_major = (int16_t)(r * 84 / 100),
    .ring             = (int16_t)(r * 78 / 100),
    .dot              = (int16_t)(r * 11 / 100),
    .dot_selected     = (int16_t)(r * 14 / 100),
    .halo             = (int16_t)(r * 20 / 100),
    .hand_hour        = (int16_t)(r * 48 / 100),
    .hand_minute      = (int16_t)(r * 72 / 100),
  };
  return d;
}

int32_t dial_slot_angle(int slot) {
  return TRIG_MAX_ANGLE * slot / DIAL_SLOTS;
}

GPoint dial_point(GPoint center, int16_t radius, int32_t angle) {
  return GPoint(
    center.x + (int16_t)(sin_lookup(angle) * radius / TRIG_MAX_RATIO),
    center.y - (int16_t)(cos_lookup(angle) * radius / TRIG_MAX_RATIO));
}

void dial_draw_ticks(GContext *ctx, const Dial *d) {
  for (int i = 0; i < DIAL_SLOTS; i++) {
    const bool quarter = (i % 3) == 0;
    const int32_t a = dial_slot_angle(i);
    graphics_context_set_stroke_width(ctx, quarter ? 5 : 2);
    graphics_draw_line(ctx,
                       dial_point(d->center, d->tick_outer, a),
                       dial_point(d->center,
                                  quarter ? d->tick_inner_major
                                          : d->tick_inner_minor, a));
  }
}

void dial_draw_hands(GContext *ctx, const Dial *d, const struct tm *t, bool ghost) {
  const int32_t minute = TRIG_MAX_ANGLE * t->tm_min / 60;
  const int32_t hour   = TRIG_MAX_ANGLE * ((t->tm_hour % 12) * 60 + t->tm_min) / 720;

  graphics_context_set_stroke_width(ctx, ghost ? 1 : 7);
  graphics_draw_line(ctx, d->center, dial_point(d->center, d->hand_hour, hour));

  graphics_context_set_stroke_width(ctx, ghost ? 1 : 5);
  graphics_draw_line(ctx, d->center, dial_point(d->center, d->hand_minute, minute));

  if (!ghost) {
    graphics_fill_circle(ctx, d->center, 5);
  }
}
