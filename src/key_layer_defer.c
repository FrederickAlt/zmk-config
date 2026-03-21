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
 *      Capture it, start timer for LAYER_DEFER_MS.
 *
 *  Regular key, one already held:
 *      Flush the held key to make room, then capture the new one.
 *
 *  Modifier key, nothing held:
 *      Bubble immediately.
 *
 *  Modifier key, regular key held:
 *      Reschedule the held key's timer to 1ms, then bubble the modifier.
 *      The modifier enters hold_tap (undecided) immediately.  After 1ms
 *      the held key fires through the chain — hold_tap sees it while
 *      still undecided and can decide hold, activating the layer before
 *      the held key's binding resolves.
 *
 * Release rules
 * -------------
 *  Regular key still in buffer (timer has not fired):
 *      Fire the press, bubble the release, clear the buffer.
 *
 *  Any other release (modifier, or regular whose timer already fired):
 *      Bubble immediately.
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
      LOG_DBG("key_layer_defer: registered modifier pos=%u", val);
    } else {
      s++;
    }
  }

  LOG_INF("key_layer_defer: %d modifier position(s) configured",
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
  LOG_INF("key_layer_defer: ready, window=%dms", LAYER_DEFER_MS);
}

/* =========================================================================
 * Core: fire the held press and clear the buffer
 * ========================================================================= */

static void fire_and_clear(void) {
  if (!held.active) {
    return;
  }
  k_work_cancel_delayable(&held.timer);
  LOG_DBG("key_layer_defer: firing press pos=%u uptime=%lldms", held.position,
          k_uptime_get());
  ZMK_EVENT_RELEASE(held.ev);
  held.active = false;
  LOG_DBG("key_layer_defer: buffer cleared");
}

/* =========================================================================
 * Timer callback
 * ========================================================================= */

static void defer_timeout(struct k_work *work) {
  if (!held.active) {
    LOG_DBG("key_layer_defer: timer fired but buffer already empty");
    return;
  }
  LOG_DBG("key_layer_defer: timer expired pos=%u uptime=%lldms", held.position,
          k_uptime_get());
  fire_and_clear();
}

/* =========================================================================
 * Event handlers
 * ========================================================================= */

static int on_press(const zmk_event_t *ev,
                    struct zmk_position_state_changed *data) {
  int64_t uptime = k_uptime_get();
  int64_t age_ms = uptime - data->timestamp;
  int64_t remaining = LAYER_DEFER_MS - age_ms;

  if (is_modifier_position(data->position)) {
    LOG_DBG("key_layer_defer: modifier press pos=%u uptime=%lldms "
            "buffer_active=%d buffer_pos=%u",
            data->position, uptime, held.active,
            held.active ? held.position : 0);

    if (held.active) {
      /*
       * A modifier arrived while a regular key is held.  Reschedule
       * the timer to fire in 1ms — the modifier is now entering the
       * chain and hold_tap will start its undecided window.  After
       * 1ms the held key fires through, hold_tap sees it while still
       * undecided and resolves hold, activating the layer before the
       * held key's binding is looked up.
       */
      LOG_DBG("key_layer_defer: modifier arrived, rescheduling held "
              "pos=%u timer to 1ms",
              held.position);
      k_work_reschedule(&held.timer, K_MSEC(1));
    }

    return ZMK_EV_EVENT_BUBBLE;
  }

  if (held.active) {
    LOG_DBG("key_layer_defer: second regular press pos=%u while pos=%u "
            "held, flushing held key uptime=%lldms",
            data->position, held.position, uptime);
    fire_and_clear();
  }

  held.active = true;
  held.position = data->position;
  held.ev = copy_raised_zmk_position_state_changed(data);

  if (remaining <= 0) {
    LOG_DBG("key_layer_defer: pos=%u already aged %lldms >= %dms, "
            "firing immediately uptime=%lldms",
            data->position, age_ms, LAYER_DEFER_MS, uptime);
    fire_and_clear();
    return ZMK_EV_EVENT_CAPTURED;
  }

  k_work_schedule(&held.timer, K_MSEC(remaining));
  LOG_DBG("key_layer_defer: captured pos=%u age=%lldms firing_in=%lldms "
          "uptime=%lldms",
          data->position, age_ms, remaining, uptime);
  return ZMK_EV_EVENT_CAPTURED;
}

static int on_release(const zmk_event_t *ev,
                      struct zmk_position_state_changed *data) {
  int64_t uptime = k_uptime_get();

  if (held.active && held.position == data->position) {
    LOG_DBG("key_layer_defer: early release pos=%u, firing press first "
            "uptime=%lldms",
            data->position, uptime);
    fire_and_clear();
    LOG_DBG("key_layer_defer: bubbling release pos=%u", data->position);
    return ZMK_EV_EVENT_BUBBLE;
  }

  LOG_DBG("key_layer_defer: release pos=%u not in buffer (already fired "
          "or modifier), bubbling uptime=%lldms",
          data->position, uptime);
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
