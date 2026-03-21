/*
 * Copyright (c) 2025 Frederick Alt
 * SPDX-License-Identifier: MIT
 *
 * key_layer_defer.c
 *
 * Captures every position-press event and holds it for
 * CONFIG_ZMK_KEY_LAYER_DEFER_MS before releasing it onward to the rest
 * of the event chain (combo subsystem, then zmk_keymap).
 *
 * This gives &mo / hold-tap layer activations time to take effect before
 * the key binding is resolved against the layer stack.
 *
 * Because this is an external ZMK module it is linked before ZMK
 * internals, so our listener runs first in the event chain.
 *
 * ZMK_EVENT_RELEASE() continues processing from the next listener after
 * us, so the event passes through combo (which may or may not match it)
 * and then reaches zmk_keymap which resolves it on the current layer.
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
#define DEFER_SLOTS CONFIG_ZMK_KEY_LAYER_DEFER_SLOTS

/* =========================================================================
 * Deferred key slots
 * ========================================================================= */

struct defer_slot {
  bool active;
  bool press_fired;
  uint32_t position;
  struct zmk_position_state_changed_event ev;
  struct k_work_delayable timer;
};

static struct defer_slot slots[DEFER_SLOTS];
static bool initialized = false;

static struct defer_slot *find_active(uint32_t pos) {
  for (int i = 0; i < DEFER_SLOTS; i++) {
    if (slots[i].active && slots[i].position == pos) {
      return &slots[i];
    }
  }
  return NULL;
}

static struct defer_slot *alloc_slot(void) {
  for (int i = 0; i < DEFER_SLOTS; i++) {
    if (!slots[i].active) {
      return &slots[i];
    }
  }
  return NULL;
}

/* =========================================================================
 * Firing a deferred press
 * ========================================================================= */

static void fire_press(struct defer_slot *slot) {
  if (slot->press_fired) {
    return;
  }
  slot->press_fired = true;
  k_work_cancel_delayable(&slot->timer);
  LOG_DBG("layer_defer: firing press pos=%u", slot->position);
  ZMK_EVENT_RELEASE(slot->ev);
}

/* =========================================================================
 * Timer callback
 * ========================================================================= */

static void defer_timeout(struct k_work *work) {
  struct defer_slot *slot =
      CONTAINER_OF(CONTAINER_OF(work, struct k_work_delayable, work),
                   struct defer_slot, timer);
  if (!slot->active || slot->press_fired) {
    return;
  }
  LOG_DBG("layer_defer: timer expired pos=%u", slot->position);
  fire_press(slot);
}

/* =========================================================================
 * Init (lazy, on first event)
 * ========================================================================= */

static void key_layer_defer_init(void) {
  for (int i = 0; i < DEFER_SLOTS; i++) {
    slots[i].active = false;
    k_work_init_delayable(&slots[i].timer, defer_timeout);
  }
  LOG_INF("layer_defer: ready, window=%dms slots=%d", LAYER_DEFER_MS,
          DEFER_SLOTS);
}

/* =========================================================================
 * Event handlers
 * ========================================================================= */

static int on_press(const zmk_event_t *ev,
                    struct zmk_position_state_changed *data) {
  struct defer_slot *slot = alloc_slot();
  if (!slot) {
    LOG_WRN("layer_defer: no free slot for pos=%u, bubbling", data->position);
    return ZMK_EV_EVENT_BUBBLE;
  }

  int64_t age_ms = k_uptime_get() - data->timestamp;
  int64_t remaining = LAYER_DEFER_MS - age_ms;

  slot->active = true;
  slot->press_fired = false;
  slot->position = data->position;
  slot->ev = copy_raised_zmk_position_state_changed(data);

  if (remaining <= 0) {
    LOG_DBG("layer_defer: pos=%u already aged %lldms, firing now",
            data->position, age_ms);
    fire_press(slot);
    slot->active = false;
    return ZMK_EV_EVENT_CAPTURED;
  }

  k_work_schedule(&slot->timer, K_MSEC(remaining));
  LOG_DBG("layer_defer: captured pos=%u, firing in %lldms", data->position,
          remaining);
  return ZMK_EV_EVENT_CAPTURED;
}

static int on_release(const zmk_event_t *ev,
                      struct zmk_position_state_changed *data) {
  struct defer_slot *slot = find_active(data->position);
  if (!slot) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  /*
   * Key released before timer fired. Deliver press first so zmk_keymap
   * sees press → release in the correct order. ZMK_EVENT_RELEASE on the
   * press is processed synchronously before we return, so keymap has
   * already handled it by the time we bubble the release.
   */
  fire_press(slot);
  slot->active = false;

  return ZMK_EV_EVENT_BUBBLE;
}

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
