#pragma once

#include <pebble.h>

// The dial is the whole UI: twelve hour positions that serve both as the
// clock's tick marks and as the menu's slots. Every radius is derived from
// one base radius so the same layout holds on any of the target boards.
typedef struct {
  GPoint center;
  int16_t tick_outer;
  int16_t tick_inner_minor;
  int16_t tick_inner_major;
  int16_t ring;           // where menu slots sit
  int16_t dot;            // unselected slot
  int16_t dot_selected;
  int16_t halo;           // ring drawn around the selected slot
  int16_t hand_hour;
  int16_t hand_minute;
} Dial;

#define DIAL_SLOTS 12

// The button-hint row sits below the dial and is excluded from the radius,
// so the dial never runs into it on a taller or rounder screen.
#define DIAL_HINT_H 22

Dial dial_make(GRect bounds);

// Slot 0 is 12 o'clock; slots advance clockwise, one hour position each.
int32_t dial_slot_angle(int slot);
GPoint dial_point(GPoint center, int16_t radius, int32_t angle);

// `ghost` draws thin hands meant to sit behind the menu rather than read as
// the primary content.
void dial_draw_ticks(GContext *ctx, const Dial *d);
void dial_draw_hands(GContext *ctx, const Dial *d, const struct tm *t, bool ghost);
