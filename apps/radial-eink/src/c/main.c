// Radial — an analog watch face whose menu lives on the clock face itself.
//
// Instead of a vertical list, menu items occupy hour positions: item 1 at
// 12 o'clock, item 2 at 1 o'clock, and so on clockwise. Up and Down step
// counter-clockwise and clockwise by one hour position, which is also how a
// crown maps onto the input model (the host turns detents into Up/Down).
//
// The drawing is deliberately e-ink shaped: pure black on white, no
// antialiasing, no animation, and a redraw only when something changed.

#include <pebble.h>
#include "radial.h"

typedef struct {
  const char *name;
  const char *detail;
} MenuItem;

// Slot n of the dial is item n, so the order here is the clock order.
static const MenuItem s_items[] = {
  { "Timeline", "Upcoming pins"    },
  { "Music",    "Now playing"      },
  { "Health",   "Steps and sleep"  },
  { "Alarms",   "Wake and remind"  },
  { "Timer",    "Count down"       },
  { "Weather",  "Today's forecast" },
  { "Alerts",   "Notifications"    },
  { "Settings", "Watch options"    },
};
#define ITEM_COUNT ((int)(sizeof(s_items) / sizeof(s_items[0])))

// Leaving the menu open forever would burn an e-ink refresh budget for
// nothing, so it falls back to the clock on its own.
#define MENU_IDLE_MS 8000

static Window *s_window;
static Layer *s_dial_layer;
static Window *s_detail_window;
static Layer *s_detail_layer;
static AppTimer *s_idle_timer;

static bool s_menu_open;
static int s_selected;
static struct tm s_now;

static void refresh_time(void) {
  const time_t now = time(NULL);
  s_now = *localtime(&now);
}

// ---------------------------------------------------------------- drawing

static void draw_centered_text(GContext *ctx, const char *text, GFont font,
                               GRect box) {
  const GSize used = graphics_text_layout_get_content_size(
      text, font, box, GTextOverflowModeWordWrap, GTextAlignmentCenter);
  box.origin.y += (box.size.h - used.h) / 2;
  box.size.h = used.h;
  graphics_draw_text(ctx, text, font, box,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_menu_slots(GContext *ctx, const Dial *d) {
  for (int i = 0; i < ITEM_COUNT; i++) {
    const GPoint p = dial_point(d->center, d->ring, dial_slot_angle(i));
    if (i == s_selected) {
      graphics_fill_circle(ctx, p, d->dot_selected);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, p, d->halo);
    } else {
      // Unselected slots read as outlines so the selection is the only
      // solid mass on the dial.
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, p, d->dot);
    }
  }
}

static void draw_center_label(GContext *ctx, const Dial *d) {
  // Sized so the box's corners clear the inner edge of a diagonal slot's
  // halo — otherwise the white fill below nicks the ring at 1, 2, 4 and 5.
  const int16_t half = (int16_t)(d->ring - d->halo - 8);
  // On a small dial the 18 pt name would wrap mid-word inside the ring.
  GFont font = fonts_get_system_font(half * 2 >= 80 ? FONT_KEY_GOTHIC_18_BOLD
                                                    : FONT_KEY_GOTHIC_14_BOLD);
  GRect box = GRect(d->center.x - half, d->center.y - 22, half * 2, 44);

  // Punch a hole in the ghost hands so the label stays readable.
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, box, 6, GCornersAll);

  graphics_context_set_text_color(ctx, GColorBlack);
  draw_centered_text(ctx, s_items[s_selected].name, font, box);
  graphics_context_set_fill_color(ctx, GColorBlack);
}

static void draw_hint(GContext *ctx, GRect bounds, const char *hint) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, hint, font,
                     GRect(0, bounds.size.h - DIAL_HINT_H, bounds.size.w, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void dial_layer_update(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const Dial d = dial_make(bounds);

  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  if (s_menu_open) {
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_circle(ctx, d.center, d.ring);
    dial_draw_hands(ctx, &d, &s_now, true);
    draw_menu_slots(ctx, &d);
    draw_center_label(ctx, &d);
    draw_hint(ctx, bounds, s_items[s_selected].detail);
  } else {
    dial_draw_ticks(ctx, &d);
    dial_draw_hands(ctx, &d, &s_now, false);
    draw_hint(ctx, bounds, "UP / DOWN for menu");
  }
}

static void detail_layer_update(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const MenuItem *item = &s_items[s_selected];

  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  draw_centered_text(ctx, item->name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(4, bounds.size.h / 2 - 44, bounds.size.w - 8, 44));
  draw_centered_text(ctx, item->detail,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(4, bounds.size.h / 2, bounds.size.w - 8, 40));
  draw_hint(ctx, bounds, "BACK to the dial");
}

// ------------------------------------------------------------------ input

static void close_menu(void *unused) {
  s_idle_timer = NULL;
  if (!s_menu_open) return;
  s_menu_open = false;
  layer_mark_dirty(s_dial_layer);
}

static void poke_idle_timer(void) {
  // reschedule fails once the timer has already fired, so fall through to a
  // fresh registration rather than trusting a stale handle.
  if (s_idle_timer && app_timer_reschedule(s_idle_timer, MENU_IDLE_MS)) return;
  s_idle_timer = app_timer_register(MENU_IDLE_MS, close_menu, NULL);
}

// Up and Down open the menu on first press rather than jumping the
// selection, so the dial never moves under a press that was meant to wake it.
static void step(int delta) {
  refresh_time();
  if (s_menu_open) {
    s_selected = (s_selected + delta + ITEM_COUNT) % ITEM_COUNT;
  } else {
    s_menu_open = true;
    s_selected = 0;
  }
  poke_idle_timer();
  layer_mark_dirty(s_dial_layer);
}

static void up_click(ClickRecognizerRef recognizer, void *context)   { step(-1); }
static void down_click(ClickRecognizerRef recognizer, void *context) { step(+1); }

static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (!s_menu_open) {
    step(0);  // opens the menu at 12 o'clock
    return;
  }
  // Hold the menu while the detail screen is up: coming back to a dial that
  // timed out underneath you loses your place for no reason.
  if (s_idle_timer) { app_timer_cancel(s_idle_timer); s_idle_timer = NULL; }
  window_stack_push(s_detail_window, false);
}

static void back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_menu_open) {
    if (s_idle_timer) { app_timer_cancel(s_idle_timer); s_idle_timer = NULL; }
    close_menu(NULL);
    return;
  }
  window_stack_pop_all(false);
}

static void click_config(void *context) {
  // Repeating subscriptions cover the single click too, and let a held
  // button (or a fast crown spin) walk the dial at a rate e-ink can follow.
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 250, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 250, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// Returning from the detail screen restarts the idle countdown.
static void dial_appeared(Window *window) {
  if (s_menu_open) poke_idle_timer();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  s_now = *tick_time;
  layer_mark_dirty(s_dial_layer);
}

// ------------------------------------------------------------------- init

static void init(void) {
  refresh_time();

  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers) { .appear = dial_appeared });
  s_dial_layer = layer_create(layer_get_bounds(window_get_root_layer(s_window)));
  layer_set_update_proc(s_dial_layer, dial_layer_update);
  layer_add_child(window_get_root_layer(s_window), s_dial_layer);

  s_detail_window = window_create();
  window_set_background_color(s_detail_window, GColorWhite);
  s_detail_layer = layer_create(
      layer_get_bounds(window_get_root_layer(s_detail_window)));
  layer_set_update_proc(s_detail_layer, detail_layer_update);
  layer_add_child(window_get_root_layer(s_detail_window), s_detail_layer);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  window_stack_push(s_window, false);
}

static void deinit(void) {
  if (s_idle_timer) app_timer_cancel(s_idle_timer);
  tick_timer_service_unsubscribe();
  layer_destroy(s_detail_layer);
  window_destroy(s_detail_window);
  layer_destroy(s_dial_layer);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
