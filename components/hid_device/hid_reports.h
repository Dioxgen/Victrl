#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse button bits */
#define HID_MOUSE_BTN_LEFT    0x01
#define HID_MOUSE_BTN_RIGHT   0x02
#define HID_MOUSE_BTN_MIDDLE  0x04

/*
 * How long to wait for the HID IN endpoint to accept a report.
 *
 * Reports are queued into a single endpoint FIFO; if the previous one has not
 * been collected by the host yet, TinyUSB refuses the new one. The old code
 * ignored that refusal, which silently swallowed keystrokes — one of the two
 * causes of characters going missing while typing.
 */
#define HID_REPORT_TIMEOUT_MS 200

/*
 * Block until the HID interface can accept another report.
 * Returns false on timeout (device unplugged, or the host stopped polling).
 */
bool hid_wait_ready(uint32_t timeout_ms);

/* 8-byte keyboard boot report. Returns false if the report was not queued. */
bool hid_send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

/* 6-byte absolute mouse report: buttons(1) + X(2) + Y(2) + wheel(1) */
bool hid_send_abs_mouse_report(uint8_t buttons, uint16_t x, uint16_t y);

/*
 * Scroll wheel report.
 *
 * The X/Y fields are ABSOLUTE, so the current cursor position and held
 * buttons must be repeated in every report — sending zeros here teleports the
 * pointer to the top-left corner of the screen on every scroll.
 *
 * `pan` (horizontal scroll) is accepted but not transmitted: the report
 * descriptor only declares the vertical Wheel usage. Wiring it up requires
 * adding an AC Pan field to the descriptor, which is a device-visible change
 * that needs on-hardware verification.
 */
bool hid_send_mouse_scroll_report(uint8_t buttons, uint16_t x, uint16_t y,
                                  int8_t wheel);

#ifdef __cplusplus
}
#endif
