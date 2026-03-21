/*
 * Copyright (c) 2025 Frederick Alt
 * SPDX-License-Identifier: MIT
 *
 * key_layer_defer.c
 *
 * Captures non-modifier key presses and holds the most recent one for up
 * to CONFIG_ZMK_KEY_LAYER_DEFER_MS, giving &mo / hold-tap layer keys time
 * to activate before the held key's binding is resolved.
 *
 * "Modifier keys" are specific physical key positions declared in
 * CONFIG_ZMK_KEY_LAYER_DEFER_MOD_POSITIONS (space-separated list of
 * position numbers, e.g. "30 31 32 33").  All other positions are
 * "regular keys" subject to deferral.
 *
 * Press rules
 * -----------
 *  Regular key, nothing held:
 *      Capture it, start timer.
 *
 *  Regular key, one already held:
 *      Fire the held press, clear the buffer, capture the new key.
 *
 *  Modifier key, one regular key held:
 *      Fire the held press first (it was physically pressed earlier),
 *      clear the buffer, then bubble the modifier through.
 *
 *  Modifier key, nothing held:
 *      Bubble immediately — modifiers are never deferred.
 *
 * Release rules
 * -------------
 *  Regular key, still in buffer (timer has not fired):
 *      Fire the press, bubble the release, clear the buffer.
 *
 *  Any other release (modifier, or regular whose timer already fired):
 *      Bubble immediately — nothing to do.
 *
 * Combo interaction
 * -----------------
 * We delay ALL regular key events before combo.c sees them.  combo.c
 * computes its timeout from the original physical-press timestamp, so
 * our deferral consumes part of the combo window.  Keep all combo
 * timeout-ms strictly greater than CONFIG_ZMK_KEY_LAYER_DEFER_MS.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_KEY_LAYER_DEFER)

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define LAYER_DEFER_MS CONFIG_ZMK_KEY_LAYER_DEFER_MS
#define MAX_MOD_POSITIONS 32

/* =========================================================================
 * Modifier-position table
 *
 * Populated at init by parsing CONFIG_ZMK_KEY_LAYER_DEFER_MOD_POSITIONS,
 * a space-separated string of decimal key-position numbers.
 * ========================================================================= */

static uint32_t mod_positions[MAX_MOD_POSITIONS];
static int mod_positions_count = 0;

static void parse_mod_positions(void) {
  const char *s = CONFIG_ZMK_KEY_LAYER_DEFER_MOD_POSITIONS;
  mod_positions_count = 0;

  while (*s != '\0' && mod_positions_count < MAX_MOD_POSITIONS) {
    while (*s == ' ' || *s == '\t') {
      s++;
    }
    if (*s == '\0') {
      break;
    }

    uint32_t val = 0;
    bool got_digit = false;
    while (*s >= '0' && *s <= '9') {
      val = val * 10 + (uint32_t)(*s - '0');
      got_digit = true;
      s++;
    }
    if (got_digit) {
      mod_positions[mod_positions_count++] = val;
    } else {
      s++; /* skip unexpected character */
    }
  }

  LOG_INF("layer_defer: %d modifier position(s) configured",
          mod_positions_count);
}

static bool is_modifier_position(uint32_t pos) {
  for (int i = 0; i < mod_positions_count; i++) {
    if (mod_positions[i] == pos) {
      return true;
    }
  }
  return false;
}

/* =========================================================================
 * Single held-key buffer
 * ========================================================================= */

struct held_key {
  bool active;
  uint32_t position;
  struct zmk_position_state_changed_event ev;
  struct k_work_delayable timer;
};

static struct held_key held;

/* =========================================================================
 * Forward declaration
 * ========================================================================= */

static void defer_timeout(struct k_work *work);

/* =========================================================================
 * Init (lazy, on first event)
 * ========================================================================= */

static bool initialized = false;

static void key_layer_defer_init(void) {
  parse_mod_positions();
  held.active = false;
  k_work_init_delayable(&held.timer, defer_timeout);
  LOG_INF("layer_defer: ready, window=%dms", LAYER_DEFER_MS);
}

/* =========================================================================
 * Core: fire the held press and clear the buffer
 *
 * "Fire" means pushing the stored press event copy onward through the
 * chain via ZMK_EVENT_RELEASE.  This is distinct from "bubble", which
 * just passes the currently-handled incoming event to the next listener.
 *
 * ZMK_EVENT_RELEASE is synchronous — combo and keymap have processed the
 * press before this function returns.
 * ========================================================================= */

static void fire_and_clear(void) {
  if (!held.active) {
    return;
  }
  k_work_cancel_delayable(&held.timer);
  LOG_DBG("layer_defer: firing press pos=%u", held.position);
  ZMK_EVENT_RELEASE(held.ev);
  held.active = false;
}

/* =========================================================================
 * Timer callback — window expired, send the press and clear
 * ========================================================================= */

static void defer_timeout(struct k_work *work) {
  if (!held.active) {
    return;
  }
  LOG_DBG("layer_defer: timer expired pos=%u", held.position);
  fire_and_clear();
}

/* =========================================================================
 * Event handlers
 * ========================================================================= */

static int on_press(const zmk_event_t *ev,
                    struct zmk_position_state_changed *data) {
  if (is_modifier_position(data->position)) {
    /*
     * Modifier pressed.  If a regular key is buffered, fire it first
     * so downstream sees it before the modifier — preserving the
     * physical press order.  Then bubble the modifier through.
     */
    if (held.active) {
      LOG_DBG("layer_defer: modifier pos=%u arrived, firing held pos=%u first",
              data->position, held.position);
      fire_and_clear();
    }
    return ZMK_EV_EVENT_BUBBLE;
  }

  /*
   * Regular key pressed.  If one is already buffered, fire it first.
   * Then capture the new key.
   */
  if (held.active) {
    LOG_DBG("layer_defer: new regular pos=%u arrived, firing held pos=%u first",
            data->position, held.position);
    fire_and_clear();
  }

  int64_t age_ms = k_uptime_get() - data->timestamp;
  int64_t remaining = LAYER_DEFER_MS - age_ms;

  held.active = true;
  held.position = data->position;
  held.ev = copy_raised_zmk_position_state_changed(data);

  if (remaining <= 0) {
    /* Already older than the window — fire immediately. */
    LOG_DBG("layer_defer: pos=%u already aged %lldms, firing now",
            data->position, age_ms);
    fire_and_clear();
    return ZMK_EV_EVENT_CAPTURED;
  }

  k_work_schedule(&held.timer, K_MSEC(remaining));
  LOG_DBG("layer_defer: captured pos=%u, firing in %lldms", data->position,
          remaining);
  return ZMK_EV_EVENT_CAPTURED;
}

static int on_release(const zmk_event_t *ev,
                      struct zmk_position_state_changed *data) {
  if (held.active && held.position == data->position) {
    /*
     * The buffered key was released before its timer fired.
     * Fire the press first (synchronous), clear the buffer,
     * then bubble the release so keymap sees press → release
     * in the correct order.
     */
    LOG_DBG("layer_defer: early release pos=%u, firing press first",
            data->position);
    fire_and_clear();
    return ZMK_EV_EVENT_BUBBLE;
  }

  /*
   * Anything else — modifier release, or release of a regular key
   * whose press was already sent (timer fired or flushed by a
   * subsequent key).  Just pass through.
   */
  return ZMK_EV_EVENT_BUBBLE;
}

/* =========================================================================
 * Listener
 * ========================================================================= */

static int key_layer_defer_listener(const zmk_event_t *ev) {
  if (!initialized) {
    key_layer_defer_init();
    initialized = true;
  }

  struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
  if (!data) {
    return ZMK_EV_EVENT_BUBBLE;
  }
  return data->state ? on_press(ev, data) : on_release(ev, data);
}

ZMK_LISTENER(key_layer_defer, key_layer_defer_listener);
ZMK_SUBSCRIPTION(key_layer_defer, zmk_position_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_KEY_LAYER_DEFER) */
