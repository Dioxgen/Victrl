# hid_device — USB HID keyboard + absolute-coordinate mouse

> **English** | [中文](README_CN.md)

## Overview

A TinyUSB composite HID device: one HID interface exposes both a 6KRO keyboard and an absolute-coordinate mouse, so a host can inject keystrokes, text and mouse actions into a target machine over USB.

## HID report descriptor

| Item | Details |
|------|---------|
| Interface | 1 HID interface, 2 top-level collections (keyboard, mouse) |
| Keyboard | Report ID `1`, standard boot keyboard, 8-byte boot report |
| Mouse | Report ID `2`, absolute X/Y (0~32767), 3 button bits, 8-bit relative wheel (-127~127), 6 bytes total |
| Endpoint | Interrupt IN endpoint `0x81`, packet size 16, polling interval 1 ms |

## Device identity (USB descriptors)

The strings the host sees are deliberate rather than incidental: this device is meant to be identifiable, not concealed, and a host that cannot tell what is attached cannot audit it either.

| Field | Value |
|------|---------|
| Manufacturer (string 1) | `Victrl` |
| Product (string 2) | `Victrl HID Bridge` |
| Serial (string 3) | `VIC-` followed by the six bytes of the chip's base MAC in hex, e.g. `VIC-3C71BF0A1B2C` |
| HID interface (string 4) | `Victrl HID` |
| VID : PID | `0x303A` : `0x4001` |

`hid_device_init()` builds the serial from `esp_read_mac(mac, ESP_MAC_BASE)` and writes it into `s_usb_strings[3]` **before** calling `tinyusb_driver_install()`. The order matters: `descriptors_control.c` copies the *pointers* out of the array at install time, and because the buffer is static it stays valid for the life of the device. If the MAC cannot be read the placeholder `0001` is kept and a warning is logged.

Every unit used to report the fixed serial `0001`, which identified the model but never the unit. Both `CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID` and `CONFIG_TINYUSB_DESC_USE_DEFAULT_PID` are enabled, so `0x303A:0x4001` is Espressif's default and is shared with other ESP32 boards — the strings, and the serial in particular, are what identify a specific device. The `CONFIG_TINYUSB_DESC_*` strings in `sdkconfig` are not what the host receives: the firmware passes its own `s_usb_strings` array, and an application-supplied array takes precedence over the defaults.

## Absolute coordinate mapping

```
abs = (pixel × 32767) / screen_width
```

A `pixel` value larger than the screen size is clamped to the screen size first (i.e. pinned to the bottom-right corner); a zero screen dimension falls back to the default 1920×1080.

Mouse reports are **full snapshots**: X/Y are absolute fields, so every move, click, drag and scroll has to repeat the current position and button state. The module keeps `s_last_x`, `s_last_y` and `s_last_buttons` internally; scrolling changes only the wheel byte.

## Public API

| Function | Purpose |
|----------|---------|
| `hid_device_init()` | Initialise TinyUSB and register the device/config/string descriptors and the HID report descriptor |
| `hid_device_set_screen_size()` | Set the default screen size (only affects what `hid_mouse_get_state()` reports) |
| `hid_device_set_type_profile()` | Set the typing rhythm (`hold_ms`, `gap_ms`, `jitter_ms`, `word_pause_ms`) |
| `hid_device_get_type_profile()` | Read the current typing rhythm |
| `hid_mouse_move_abs()` | Move to an absolute position (pixel coordinates plus screen size) |
| `hid_mouse_click()` | Click: `left` / `right` / `middle` / `double_left` |
| `hid_mouse_down()` / `hid_mouse_up()` | Press/release a button (for dragging) |
| `hid_mouse_scroll()` | Vertical wheel (`delta_x` is not supported yet — see Notes) |
| `hid_mouse_get_state()` | Report the current absolute pointer position, held buttons and the screen size used for mapping |
| `hid_key_press()` | Key combination such as `ctrl+c`; up to 6 non-modifier keys |
| `hid_type_string()` | Type ASCII text, returns `hid_type_result_t` |
| `hid_release_all()` | Release all keyboard keys and mouse buttons |
| `hid_keyboard_led_state()` | Raw LED byte from the last host output report (bit0 Num, bit1 Caps, bit2 Scroll) |
| `hid_keyboard_led_seen()` | Whether the host has sent an output report at least once |
| `hid_type_set_digits_on_keypad()` | Whether digits go through the numeric keypad (off by default) |
| `hid_ensure_num_lock()` | Confirm Num Lock against the host's LED report and turn it on if needed |

### Typing result and typing rhythm

`hid_type_string()` returns a `hid_type_result_t`: `len` (characters in the input string), `queued` (key-down reports successfully queued) and `skipped` (characters dropped because they have no key mapping). When `queued == len` the device put every keystroke on the wire, so any missing or mangled text on screen comes from downstream — usually an active Chinese IME.

`hid_type_profile_t` defaults: `hold_ms = 15`, `gap_ms = 35`, `jitter_ms = 25`, `word_pause_ms = 120`. The inter-key delay is `gap_ms + (random 0 ~ jitter_ms-1)`, a space adds `word_pause_ms` on top, and a `hold_ms` of 0 is forced back to 15.

## Sub-modules

| File | Purpose |
|------|---------|
| `hid_device.c` | TinyUSB init, descriptors, HID operation implementations, state tracking, typing rhythm |
| `hid_reports.c` | Low-level report sending: endpoint-ready wait, keyboard/mouse/scroll reports |
| `hid_keymap.c` | Key name → HID key code mapping (letters, digits, F1~F12, editing keys, arrows, keypad, symbols, ...), modifier detection, Shift-symbol decision |
| `hid_test.c` | Keyboard and mouse self-test, `hid_run_self_test()` |
| `mouse_test.c` | Mouse self-test, `mouse_run_test()` |

## Notes

- The device layer uses `TINYUSB_PORT_FULL_SPEED_0` (USB OTG 1.1, Full Speed) for HID so it does not collide with the MS2109 capture device on OTG 0.
- Mouse reports must go through `tud_hid_n_report(0, 2, ...)` with report_id=2 directly, never `tud_hid_report(0, ...)`; keyboard reports use report_id=1.
- Every send waits for the endpoint to become ready, with a timeout of `HID_REPORT_TIMEOUT_MS = 200` ms; when the device is not mounted all sends fail immediately.
- A mouse report is attempted 5 times with a 2 ms delay between attempts; each attempt first waits for the endpoint and that wait alone can last up to 200 ms, so "5 × 2 ms" is not the real cost.
- `hid_key_press()` holds the keys for 50 ms before releasing; `hid_mouse_click()` holds the button for 100 ms, and `double_left` is two left clicks 50 ms apart.
- On a failed send `hid_type_string()` retries the same character once (after 20 ms) and otherwise logs an error and drops it; characters with no key mapping (non-ASCII/CJK) are counted in `skipped`.
- The key intervals are a deliberately slow, human-paced rhythm: reliability matters more than speed, because one mistyped character costs far more than the milliseconds saved.
- Horizontal scrolling is not declared in the report descriptor: the `delta_x` argument of `hid_mouse_scroll()` only produces a warning log and is never transmitted. Wiring it up requires adding an AC Pan field.
- Digits can take the numeric-keypad path to bypass a Chinese IME that swallows the main number row. It is off by default and requires Num Lock to be confirmed on by `hid_ensure_num_lock()` first — otherwise the keypad emits arrow keys and Home/End, which is worse than losing a digit.
- `hid_ensure_num_lock()` returns false outright when the host has not sent an LED report yet (it refuses to guess), and when turning Num Lock on it polls at most 12 × 50 ms (about 600 ms). The keypad digit codes are 0x59~0x61 (`1`~`9`) and 0x62 (`0`).
- The LED output report is recorded by `tud_hid_set_report_cb()`, making Num/Caps/Scroll Lock visible to the device — the only host-side keyboard state it is given.
- `hid_keymap_lookup()` matches named keys case-insensitively and symbol keys exactly; single letters and digits map even when absent from the table.
- `hid_keymap.h` defines the right-hand modifier masks `MOD_RCTRL`, `MOD_RSHIFT`, `MOD_RALT` and `MOD_RGUI`, but combination parsing only recognises `ctrl`, `shift`, `alt` and `gui`/`win`, so the right-hand modifiers are currently unreachable.
- Both `hid_test.c` and `mouse_test.c` first wait for the USB mount (up to 150 × 100 ms, about 15 s) and skip the test if it never mounts.
