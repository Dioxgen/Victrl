#pragma once

#include <stdbool.h>
#include <stddef.h>   /* size_t — do not rely on esp_err.h to pull it in */
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HID_ABS_MAX 32767

/*
 * Typing rhythm.
 *
 * Typing here is a control channel, not a benchmark: a dropped character costs
 * a whole model round trip plus a recovery sequence, which is far more
 * expensive than the milliseconds saved by typing faster. The defaults are
 * therefore deliberately human-paced, and everything is tunable because the
 * right values depend on the target machine's load.
 */
typedef struct {
    uint32_t hold_ms;        /* how long a key is held down (15)            */
    uint32_t gap_ms;         /* base delay between keys (35)                */
    uint32_t jitter_ms;      /* random extra added to each gap (25)         */
    uint32_t word_pause_ms;  /* extra pause after a space (120)             */
} hid_type_profile_t;

/*
 * What actually happened while typing.
 *
 * This is the evidence that separates "the device failed to send it" from "the
 * target machine swallowed it". If `queued == len` the device put every
 * keystroke on the wire, so any missing or mangled text on screen is caused
 * downstream — almost always an active Chinese IME consuming the input.
 */
typedef struct {
    size_t len;       /* characters in the input string       */
    size_t queued;    /* key-down reports successfully queued */
    size_t skipped;   /* no key mapping (non-ASCII / CJK)     */
} hid_type_result_t;

esp_err_t hid_device_init(void);
void hid_device_set_screen_size(uint32_t w, uint32_t h);
void hid_device_set_type_profile(const hid_type_profile_t *profile);
const hid_type_profile_t *hid_device_get_type_profile(void);

void hid_mouse_move_abs(uint32_t pixel_x, uint32_t pixel_y,
                         uint32_t screen_w, uint32_t screen_h);
void hid_mouse_click(const char *button);
void hid_mouse_down(const char *button);
void hid_mouse_up(const char *button);
void hid_mouse_scroll(int8_t delta_x, int8_t delta_y);
void hid_key_press(const char *key_combo);
hid_type_result_t hid_type_string(const char *text);
void hid_release_all(void);

/*
 * ── Host lock state and the keypad path ──────────────────────────────
 *
 * A HID keyboard is sent exactly one piece of host state: the LED output
 * report (Num Lock / Caps Lock / Scroll Lock). It is the host TELLING us its
 * keyboard state, and it is what makes "press Num Lock" a deterministic action
 * instead of another unobservable parity — the same trap as toggling a Chinese
 * input method.
 */

/* Raw LED byte from the last host output report. Bit0 Num, bit1 Caps, bit2 Scroll. */
uint8_t hid_keyboard_led_state(void);

/* False until the host has sent an output report at least once. */
bool hid_keyboard_led_seen(void);

/*
 * Type digits as numeric-keypad scancodes instead of the main number row.
 *
 * This exists because a Chinese IME in composition mode consumes the MAIN
 * number row as candidate selectors, while the keypad is frequently passed
 * through untouched. It is one of several input channels the agent can fall
 * back on when the target machine reinterprets its keystrokes; which channel
 * works is a property of the machine, so the choice is per action.
 *
 * Off by default. Requires Num Lock ON (see hid_ensure_num_lock) — without it
 * the keypad emits arrows and Home/End, which is far worse than a lost digit.
 */
void hid_type_set_digits_on_keypad(bool on);

/*
 * Turn Num Lock on if it is off, confirmed against the host's LED report.
 * Returns false when the state could not be established or confirmed, in which
 * case the caller must NOT use keypad digits.
 */
bool hid_ensure_num_lock(void);

/*
 * Report where the pointer currently is, in HID absolute units (0..HID_ABS_MAX),
 * plus which mouse buttons are held.
 *
 * The device is the only party that knows this: the pointer is frequently
 * invisible or ambiguous in a screenshot, and after a `move` without a click the
 * model otherwise has no way to know where it left the cursor. Also returns the
 * screen size it will map against, so the caller can convert to pixels.
 */
void hid_mouse_get_state(uint16_t *abs_x, uint16_t *abs_y, uint8_t *buttons,
                          uint32_t *screen_w, uint32_t *screen_h);

#ifdef __cplusplus
}
#endif
