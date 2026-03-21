/*
 * Copyright (c) 2025 Frederick Alt
 * SPDX-License-Identifier: MIT
 *
 * key_layer_defer.c
 *
 * For keys that are not part of any combo, captures the press event and
 * holds it for CONFIG_ZMK_KEY_LAYER_DEFER_MS before releasing it onward
 * to the rest of the event chain (combo subsystem, then zmk_keymap).
 *
 * Purpose
 * -------
 * When a momentary layer key (&mo) and a regular key are pressed in quick
 * succession, the regular key would normally reach zmk_keymap before &mo
 * has had time to activate its layer.  This module holds the regular key
 * for up to LAYER_DEFER_MS so that any layer change occurring within that
 * window is visible when the binding is resolved.
 *
 * Combo interaction
 * -----------------
 * Keys that appear in any combo are ignored entirely — the combo subsystem
 * runs after us (we are an external module, linked first) and handles
 * those keys with its own timeout.  Because combo timeouts are always
 * >= LAYER_DEFER_MS, the layer will have settled by the time the combo
 * system releases the key onward to zmk_keymap anyway.
 *
 * Which positions are combo candidates is determined at init time by
 * walking the zmk_combos devicetree node using the same DT macros that
 * combo.c uses.  No patch to combo.c or exported symbol is required.
 *
 * Listener priority
 * -----------------
 * External ZMK modules are linked before ZMK internals, so our listener
 * automatically runs first — before the combo subsystem and before
 * zmk_keymap.  This is the correct position.
 *
 * ZMK_EVENT_RELEASE() continues processing from the *next* listener after
 * us, so re-raised press events skip back into the combo subsystem (which
 * will not match, since this is not a combo position) and then reach
 * zmk_keymap which resolves the binding on the current layer.
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_KEY_LAYER_DEFER)

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define LAYER_DEFER_MS CONFIG_ZMK_KEY_LAYER_DEFER_MS
#define DEFER_SLOTS CONFIG_ZMK_KEY_LAYER_DEFER_SLOTS

/* =========================================================================
 * Combo position bitmap
 *
 * One bit per physical key position.  A set bit means the position
 * appears in at least one combo and should be left to the combo subsystem.
 *
 * DT_FOREACH_PROP_ELEM passes (node_id, prop_name, index) to the callback,
 * matching the three-argument form used throughout combo.c.
 * ========================================================================= */

static uint32_t combo_positions[DIV_ROUND_UP(ZMK_KEYMAP_LEN, 32)];

#define MARK_COMBO_KEY(n, prop, idx)                                           \
  sys_bitfield_set_bit((mem_addr_t)combo_positions,                            \
                       DT_PROP_BY_IDX(n, prop, idx));

#define MARK_COMBO_KEYS(n)                                                     \
  COND_CODE_1(DT_NODE_HAS_PROP(n, key_positions),                              \
              (DT_FOREACH_PROP_ELEM(n, key_positions, MARK_COMBO_KEY)), ())

static inline bool is_combo_position(uint32_t pos) {
  if (pos >= ZMK_KEYMAP_LEN) {
    return false;
  }
  return sys_bitfield_test_bit((mem_addr_t)combo_positions, pos);
}

/* =========================================================================
 * Deferred key slots
 *
 * Each captured press occupies one slot until its matching release arrives.
 * The slot stores a full copy of the original event struct so that
 * ZMK_EVENT_RELEASE can re-enter the event chain with the original data
 * (including the original timestamp) intact.
 * ========================================================================= */

struct defer_slot {
  bool active;
  bool press_fired;
  uint32_t position;
  struct zmk_position_state_changed_event ev;
  struct k_work_delayable timer;
};

static struct defer_slot slots[DEFER_SLOTS];

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
 *
 * ZMK_EVENT_RELEASE(ev) expands to zmk_event_manager_release(&(ev).header).
 * It resumes processing from the listener *after* us — skipping past
 * key_layer_defer into the combo subsystem (no match) and then into
 * zmk_keymap, which resolves the binding on whatever layer is active now.
 * Because at least LAYER_DEFER_MS have elapsed since the physical keypress,
 * any &mo pressed within the window will have already activated its layer.
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
  /* slot remains active until the matching key-up clears it */
}

/* =========================================================================
 * Event handlers
 * ========================================================================= */

static int on_press(const zmk_event_t *ev,
                    struct zmk_position_state_changed *data) {
  /*
   * Combo candidate — hand off to the combo subsystem immediately.
   * Combo holds the event for its own timeout_ms which is always >=
   * LAYER_DEFER_MS, so layer state will have settled regardless.
   */
  if (is_combo_position(data->position)) {
    LOG_DBG("layer_defer: pos=%u is combo candidate, skipping", data->position);
    return ZMK_EV_EVENT_BUBBLE;
  }

  struct defer_slot *slot = alloc_slot();
  if (!slot) {
    LOG_WRN("layer_defer: no free slot for pos=%u, bubbling", data->position);
    return ZMK_EV_EVENT_BUBBLE;
  }

  /*
   * Calculate how much of the window remains.  For a freshly-scanned
   * key age_ms is ~0 and remaining ~= LAYER_DEFER_MS.  An event that
   * was held a long time by some other subsystem before reaching us
   * will have a large age and a small (possibly <= 0) remaining value.
   */
  int64_t age_ms = k_uptime_get() - data->timestamp;
  int64_t remaining = LAYER_DEFER_MS - age_ms;

  slot->active = true;
  slot->press_fired = false;
  slot->position = data->position;
  /*
   * copy_raised_zmk_position_state_changed(const struct
   * zmk_position_state_changed *) returns a struct
   * zmk_position_state_changed_event — a full copy of the outer event including
   * its zmk_event_t header.  Same pattern used by combo.c and
   * behavior_hold_tap.c.
   */
  slot->ev = copy_raised_zmk_position_state_changed(data);

  if (remaining <= 0) {
    /* Already past the window — fire immediately without scheduling. */
    LOG_DBG("layer_defer: pos=%u aged %lldms >= %dms, firing now",
            data->position, age_ms, LAYER_DEFER_MS);
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
    /*
     * No active slot: either a combo key we never captured, or a key
     * that exhausted the slot pool and bubbled through at press time.
     * Let the release propagate normally.
     */
    return ZMK_EV_EVENT_BUBBLE;
  }

  /*
   * Key released before the timer fired.  Deliver the press first so
   * that zmk_keymap sees press → release in the correct order.
   *
   * fire_press() calls ZMK_EVENT_RELEASE on the captured press event.
   * The event manager processes that release synchronously — it walks
   * the remaining listeners (combo → keymap) to completion before
   * returning here.  By the time we reach the bubble below, zmk_keymap
   * has already received and handled the press.
   */
  fire_press(slot);
  slot->active = false;

  /* Bubble the release so zmk_keymap also processes the key-up. */
  return ZMK_EV_EVENT_BUBBLE;
}

static int key_layer_defer_listener(const zmk_event_t *ev) {
  struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
  if (!data) {
    return ZMK_EV_EVENT_BUBBLE;
  }
  return data->state ? on_press(ev, data) : on_release(ev, data);
}

ZMK_LISTENER(key_layer_defer, key_layer_defer_listener);
ZMK_SUBSCRIPTION(key_layer_defer, zmk_position_state_changed);

/* =========================================================================
 * Init
 * ========================================================================= */

static int key_layer_defer_init(void) {
  /*
   * Walk every child of the zmk_combos DT node and mark positions.
   * DT_DRV_COMPAT is defined as zmk_combos above, so
   * DT_INST_FOREACH_CHILD(0, ...) targets the same node combo.c uses.
   */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
  DT_INST_FOREACH_CHILD(0, MARK_COMBO_KEYS)
#endif

  for (int i = 0; i < DEFER_SLOTS; i++) {
    slots[i].active = false;
    k_work_init_delayable(&slots[i].timer, defer_timeout);
  }

  LOG_INF("key_layer_defer: ready, window=%dms slots=%d", LAYER_DEFER_MS,
          DEFER_SLOTS);
  return 0;
}
SYS_INIT(key_layer_defer_init, POST_KERNEL,
         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* IS_ENABLED(CONFIG_ZMK_KEY_LAYER_DEFER) */
