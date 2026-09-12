#pragma once

#include "class/hid/hid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint8_t code;
} key_entry_t;

/* Modifier keys (first byte of keyboard report) */
#define MOD_CTRL    0x01
#define MOD_SHIFT   0x02
#define MOD_ALT     0x04
#define MOD_GUI     0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

/* Look up a key code by name. Returns 0 if not found. */
uint8_t hid_keymap_lookup(const char *name);

/* Get modifier mask for a key name. Returns 0 if it's not a modifier. */
uint8_t hid_keymap_modifier(const char *name);

/*
 * Check whether TYPING the single character `sym` requires Shift (US layout).
 *
 * Look the character up in the shifted-symbol table directly. Do NOT resolve
 * it to a base keycode first and test that keycode: every unshifted symbol
 * shares its keycode with a shifted one ("-" and "_" are both HID_KEY_MINUS,
 * "=" and "+" both HID_KEY_EQUAL, ";" and ":" both HID_KEY_SEMICOLON, ...),
 * so a keycode-based test reports Shift for plain "-", "=", ";", "/" and
 * makes hid_type_string emit "_", "+", ":", "?" instead.
 */
int hid_keymap_char_needs_shift(const char *sym);

#ifdef __cplusplus
}
#endif
