/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Radial launcher: the app list laid out on a clock face.
//!
//! The first app sits at the 12 o'clock position, the second at 1 o'clock and
//! so on clockwise, with Up and Down stepping by one hour position. Every app
//! keeps a fixed place on the dial rather than one that depends on how far you
//! have scrolled, and one step is exactly one crown detent.
//!
//! It implements the same launcher_menu_layer_* API as the stock list
//! launcher so launcher.c is shared; the launcher sources are globbed, so
//! menu_layer.c compiles only when CONFIG_LAUNCHER_RADIAL is off.
//!
//! Drawing is flat black on white with no animation: this is meant for a
//! panel that redraws slowly, where a moving selection costs a refresh.

#include "menu_layer.h"

#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/window.h"
#include "process_management/app_manager.h"
#include "pbl/drivers/rtc.h"
#include "pbl/util/misc.h"
#include "pbl/util/size.h"
#include "pbl/util/trig.h"
#include "shell/system_theme.h"
#include "util/time/time.h"
#include "system/passert.h"

#if CONFIG_LAUNCHER_RADIAL

//! Hour positions on the dial. More apps than this wrap onto a second turn:
//! index 12 sits back at 12 o'clock, one ring further in.
#define RADIAL_SLOTS 12

//! Radii as a percentage of the dial radius, so one set of numbers covers
//! every display this firmware builds for.
#define RADIAL_PCT_RING 78
#define RADIAL_PCT_DOT 11
#define RADIAL_PCT_DOT_SELECTED 14
#define RADIAL_PCT_HALO 20
#define RADIAL_PCT_HAND_HOUR 42
#define RADIAL_PCT_HAND_MINUTE 62

//! The app name sits in a band below the dial rather than in the middle.
//! The centre belongs to the hands: a label there has to punch a hole in
//! them to stay readable, which leaves the hands as disconnected stubs and
//! stops the dial reading as a clock at all.
#define RADIAL_LABEL_BAND 24

typedef struct {
  GPoint center;
  int16_t radius;
  int16_t ring;
  int16_t dot;
  int16_t dot_selected;
  int16_t halo;
  int16_t hand_hour;
  int16_t hand_minute;
} RadialDial;

static const LauncherMenuLayerStyle s_styles[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = {
    .title_font_key = FONT_KEY_GOTHIC_14_BOLD,
    .subtitle_font_key = FONT_KEY_GOTHIC_14,
  },
  [PreferredContentSizeMedium] = {
    .title_font_key = FONT_KEY_GOTHIC_18_BOLD,
    .subtitle_font_key = FONT_KEY_GOTHIC_14,
  },
  [PreferredContentSizeLarge] = {
    .title_font_key = FONT_KEY_GOTHIC_24_BOLD,
    .subtitle_font_key = FONT_KEY_GOTHIC_18,
  },
  [PreferredContentSizeExtraLarge] = {
    .title_font_key = FONT_KEY_GOTHIC_28_BOLD,
    .subtitle_font_key = FONT_KEY_GOTHIC_24,
  },
};

const LauncherMenuLayerStyle *launcher_menu_layer_get_style(void) {
  return &s_styles[system_theme_get_content_size()];
}

////////////////////////////
// Geometry

static RadialDial prv_dial(GRect bounds) {
  const int16_t usable_h = (int16_t)(bounds.size.h - RADIAL_LABEL_BAND);
  const int16_t span = MIN(bounds.size.w, usable_h);
  const int16_t r = (int16_t)(span / 2 - 6);
  return (RadialDial){
    .center = GPoint(bounds.size.w / 2, usable_h / 2),
    .radius = r,
    .ring = (int16_t)(r * RADIAL_PCT_RING / 100),
    .dot = (int16_t)(r * RADIAL_PCT_DOT / 100),
    .dot_selected = (int16_t)(r * RADIAL_PCT_DOT_SELECTED / 100),
    .halo = (int16_t)(r * RADIAL_PCT_HALO / 100),
    .hand_hour = (int16_t)(r * RADIAL_PCT_HAND_HOUR / 100),
    .hand_minute = (int16_t)(r * RADIAL_PCT_HAND_MINUTE / 100),
  };
}

static int32_t prv_slot_angle(uint16_t slot) {
  return (TRIG_MAX_ANGLE * (slot % RADIAL_SLOTS)) / RADIAL_SLOTS;
}

static GPoint prv_point(GPoint center, int16_t radius, int32_t angle) {
  return GPoint(center.x + (int16_t)(sin_lookup(angle) * radius / TRIG_MAX_RATIO),
                center.y - (int16_t)(cos_lookup(angle) * radius / TRIG_MAX_RATIO));
}

//! Apps past the first twelve sit on an inner ring rather than overlapping the
//! outer one, so a long list stays readable instead of piling up.
static int16_t prv_ring_radius(const RadialDial *dial, uint16_t index) {
  const uint16_t turn = index / RADIAL_SLOTS;
  return (int16_t)(dial->ring - (turn * (dial->halo * 5 / 2)));
}

////////////////////////////
// Drawing

static uint16_t prv_count(LauncherMenuLayer *lml) {
  return lml->data_source ? app_menu_data_source_get_count(lml->data_source) : 0;
}

//! Largest to smallest; an app name that fits none of them is ellipsized at
//! the last rather than wrapped, because wrapping a single long word breaks
//! it mid-syllable ("Notification-s").
static const char *const s_label_fonts[] = {
  FONT_KEY_GOTHIC_18_BOLD,
  FONT_KEY_GOTHIC_14_BOLD,
};

//! The launcher runs in place of the watchface, so the watchface's own hands
//! are not available to draw underneath — its process is gone and there is
//! one app framebuffer. Drawing the time here keeps the clock present, and
//! keeps the dial reading as a clock face rather than a ring of dots.
//!
//! They are drawn first so the apps sit on top of them, and the centre is
//! left clear for them: see RADIAL_LABEL_BAND.
static void prv_draw_hands(GContext *ctx, const RadialDial *dial) {
  struct tm now;
  const time_t utc = rtc_get_time();
  localtime_r(&utc, &now);

  const int32_t minute_angle = (TRIG_MAX_ANGLE * now.tm_min) / 60;
  const int32_t hour_angle = (TRIG_MAX_ANGLE * ((now.tm_hour % 12) * 60 + now.tm_min)) / 720;

  graphics_context_set_stroke_width(ctx, 5);
  graphics_draw_line(ctx, dial->center, prv_point(dial->center, dial->hand_hour, hour_angle));
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_line(ctx, dial->center, prv_point(dial->center, dial->hand_minute, minute_angle));
  graphics_fill_circle(ctx, dial->center, 4);
}

static void prv_draw_label(GContext *ctx, const char *text, GRect box) {
  GFont font = fonts_get_system_font(s_label_fonts[ARRAY_LENGTH(s_label_fonts) - 1]);
  GSize used = { 0, 0 };

  for (size_t i = 0; i < ARRAY_LENGTH(s_label_fonts); i++) {
    GFont candidate = fonts_get_system_font(s_label_fonts[i]);
    const GSize size = app_graphics_text_layout_get_content_size(
        text, candidate, box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
    if (size.w <= box.size.w) {
      font = candidate;
      used = size;
      break;
    }
    if (i == ARRAY_LENGTH(s_label_fonts) - 1) {
      used = size;
    }
  }

  box.origin.y += (box.size.h - used.h) / 2;
  box.size.h = used.h;
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void prv_dial_layer_update(Layer *layer, GContext *ctx) {
  // The dial layer is a member of LauncherMenuLayer, so the owner is the
  // layer's container; the firmware's layer_init takes no data block.
  LauncherMenuLayer *lml = container_of(layer, LauncherMenuLayer, dial_layer);
  GRect bounds;
  layer_get_bounds(layer, &bounds);
  const RadialDial dial = prv_dial(bounds);
  const uint16_t count = prv_count(lml);

  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, &bounds);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);

  prv_draw_hands(ctx, &dial);

  for (uint16_t i = 0; i < count; i++) {
    const GPoint p = prv_point(dial.center, prv_ring_radius(&dial, i), prv_slot_angle(i));
    if (i == lml->selected_index) {
      graphics_fill_circle(ctx, p, dial.dot_selected);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, p, dial.halo);
    } else {
      // Unselected apps are outlines so the selection is the only solid
      // mass on the dial.
      graphics_context_set_stroke_width(ctx, 3);
      graphics_draw_circle(ctx, p, dial.dot);
    }
  }

  AppMenuNode *node = (count > 0) ? app_menu_data_source_get_node_at_index(
                                        lml->data_source, lml->selected_index)
                                  : NULL;
  if (!node || !node->name) {
    return;
  }

  graphics_context_set_text_color(ctx, GColorBlack);
  prv_draw_label(ctx, node->name,
                 GRect(2, bounds.size.h - RADIAL_LABEL_BAND, bounds.size.w - 4,
                       RADIAL_LABEL_BAND));
}

////////////////////////////
// Input

static void prv_step(LauncherMenuLayer *lml, int delta) {
  const uint16_t count = prv_count(lml);
  if (count == 0) {
    return;
  }
  lml->selected_index = (uint16_t)((lml->selected_index + count + delta) % count);
  layer_mark_dirty(&lml->dial_layer);
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  prv_step((LauncherMenuLayer *)context, -1);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  prv_step((LauncherMenuLayer *)context, +1);
}

static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  LauncherMenuLayer *lml = context;
  if (prv_count(lml) == 0) {
    return;
  }
  AppMenuNode *node = app_menu_data_source_get_node_at_index(lml->data_source,
                                                             lml->selected_index);
  if (!node) {
    return;
  }
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){
    .id = node->install_id,
    .common.reason = APP_LAUNCH_USER,
    .common.button = BUTTON_ID_SELECT,
  });
}

static void prv_click_config_provider(void *context) {
  // Auto-repeat is deliberately slower than a slow panel's refresh: a faster
  // repeat turns one deliberate press into two steps and overshoots.
  window_set_click_context(BUTTON_ID_UP, context);
  window_set_click_context(BUTTON_ID_DOWN, context);
  window_set_click_context(BUTTON_ID_SELECT, context);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 400, prv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 400, prv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
}

////////////////////////////
// Public API

void launcher_menu_layer_init(LauncherMenuLayer *lml, AppMenuDataSource *data_source) {
  PBL_ASSERTN(lml && data_source);
  *lml = (LauncherMenuLayer){
    .data_source = data_source,
    .content_size = system_theme_get_content_size(),
    .app_to_launch_after_next_render = INSTALL_ID_INVALID,
  };

  const GRect bounds = GRect(0, 0, DISP_COLS, DISP_ROWS);
  layer_init(&lml->container_layer, &bounds);
  layer_init(&lml->dial_layer, &bounds);
  layer_set_update_proc(&lml->dial_layer, prv_dial_layer_update);
  layer_add_child(&lml->container_layer, &lml->dial_layer);
}

Layer *launcher_menu_layer_get_layer(LauncherMenuLayer *lml) {
  return lml ? &lml->container_layer : NULL;
}

void launcher_menu_layer_set_click_config_onto_window(LauncherMenuLayer *lml, Window *window) {
  if (!lml || !window) {
    return;
  }
  window_set_click_config_provider_with_context(window, prv_click_config_provider, lml);
}

void launcher_menu_layer_reload_data(LauncherMenuLayer *lml) {
  if (!lml) {
    return;
  }
  const uint16_t count = prv_count(lml);
  if (count > 0 && lml->selected_index >= count) {
    lml->selected_index = (uint16_t)(count - 1);
  }
  layer_mark_dirty(&lml->dial_layer);
}

void launcher_menu_layer_update_content_size(LauncherMenuLayer *lml) {
  if (!lml || lml->content_size == system_theme_get_content_size()) {
    return;
  }
  // Only the centre label's font depends on the content size, so there is
  // nothing cached to rebuild the way the list launcher has to.
  lml->content_size = system_theme_get_content_size();
  layer_mark_dirty(&lml->dial_layer);
}

void launcher_menu_layer_set_selection_state(LauncherMenuLayer *lml,
                                             const LauncherMenuLayerSelectionState *new_state) {
  if (!lml || !lml->data_source || !new_state) {
    return;
  }
  const uint16_t count = prv_count(lml);
  lml->selected_index = (count > 0 && new_state->row_index < count) ? new_state->row_index : 0;
  layer_mark_dirty(&lml->dial_layer);
}

void launcher_menu_layer_get_selection_vertical_range(const LauncherMenuLayer *lml,
                                                      GRangeVertical *vertical_range_out) {
  if (!lml || !vertical_range_out) {
    return;
  }
  // The compositor uses this to line a launch transition up with the
  // selection. The selected app is a dot on the dial, so report the band it
  // occupies.
  GRect global;
  layer_get_global_frame((Layer *)&lml->dial_layer, &global);
  GRect bounds;
  layer_get_bounds((Layer *)&lml->dial_layer, &bounds);
  const RadialDial dial = prv_dial(bounds);
  const GPoint p = prv_point(dial.center, prv_ring_radius(&dial, lml->selected_index),
                             prv_slot_angle(lml->selected_index));
  *vertical_range_out = (GRangeVertical){
    .origin_y = (int16_t)(global.origin.y + p.y - dial.halo),
    .size_h = (int16_t)(dial.halo * 2),
  };
}

void launcher_menu_layer_get_selection_state(const LauncherMenuLayer *lml,
                                             LauncherMenuLayerSelectionState *state_out) {
  if (!lml || !lml->data_source || !state_out) {
    return;
  }
  *state_out = (LauncherMenuLayerSelectionState){
    .row_index = lml->selected_index,
    .scroll_offset_y = 0,  // nothing scrolls
    .content_size = lml->content_size,
  };
}

void launcher_menu_layer_set_selection_animations_enabled(LauncherMenuLayer *lml, bool enabled) {
  if (!lml) {
    return;
  }
  // Kept for API parity: there are no glances to play or rewind.
  lml->selection_animations_enabled = enabled;
}

void launcher_menu_layer_deinit(LauncherMenuLayer *lml) {
  if (!lml) {
    return;
  }
  layer_deinit(&lml->dial_layer);
  layer_deinit(&lml->container_layer);
}

#endif  // CONFIG_LAUNCHER_RADIAL
