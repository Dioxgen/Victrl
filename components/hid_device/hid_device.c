#include "hid_device.h"

#include <stdio.h>
#include <string.h>
#include "hid_keymap.h"
#include "hid_reports.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "class/hid/hid_device.h"
#include "device/usbd.h"

static const char *TAG = "hid";

/* ── HID Report Descriptor (1 interface, 2 collections, explicit IDs) ─ */

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_hid_report_desc[] = {
    /* Keyboard (report ID 1) — boot keyboard */
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),

    /* Absolute Mouse (report ID 2) */
    HID_USAGE_PAGE (HID_USAGE_PAGE_DESKTOP),
    HID_USAGE      (HID_USAGE_DESKTOP_MOUSE),
    HID_COLLECTION (HID_COLLECTION_APPLICATION),
        HID_REPORT_ID   (2)
        HID_USAGE       (HID_USAGE_DESKTOP_POINTER),
        HID_COLLECTION  (HID_COLLECTION_PHYSICAL),

            HID_USAGE_PAGE   (HID_USAGE_PAGE_BUTTON),
            HID_USAGE_MIN    (1),
            HID_USAGE_MAX    (3),
            HID_LOGICAL_MIN  (0),
            HID_LOGICAL_MAX  (1),
            HID_REPORT_COUNT (3),
            HID_REPORT_SIZE  (1),
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
            HID_REPORT_COUNT (1),
            HID_REPORT_SIZE  (5),
            HID_INPUT        (HID_CONSTANT),

            HID_USAGE_PAGE   (HID_USAGE_PAGE_DESKTOP),
            HID_USAGE        (HID_USAGE_DESKTOP_X),
            HID_LOGICAL_MIN  (0),
            HID_LOGICAL_MAX_N(32767, 2),
            HID_REPORT_SIZE  (16),
            HID_REPORT_COUNT (1),
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

            HID_USAGE        (HID_USAGE_DESKTOP_Y),
            HID_LOGICAL_MIN  (0),
            HID_LOGICAL_MAX_N(32767, 2),
            HID_REPORT_SIZE  (16),
            HID_REPORT_COUNT (1),
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_ABSOLUTE),

            HID_USAGE        (HID_USAGE_DESKTOP_WHEEL),
            HID_LOGICAL_MIN  (-127),
            HID_LOGICAL_MAX  (127),
            HID_REPORT_SIZE  (8),
            HID_REPORT_COUNT (1),
            HID_INPUT        (HID_DATA | HID_VARIABLE | HID_RELATIVE),

        HID_COLLECTION_END,
    HID_COLLECTION_END,
};

/* ── USB Descriptors ───────────────────────────────────────────────── */

/* The identity this device presents is part of the compliance story, not an
 * implementation detail: it is deliberately NOT a cloaked peripheral. The host
 * can see exactly what it is (manufacturer, product, HID interface) in Device
 * Manager or `lsusb`, which is what "no concealment" has to mean in practice.
 * A host that cannot tell what is attached cannot audit it either. */
static char s_serial[24] = "0001";

static const char *s_usb_strings[] = {
    (char[]){0x09, 0x04},  /* 0: English */
    "Victrl",              /* 1: Manufacturer */
    "Victrl HID Bridge",   /* 2: Product */
    s_serial,              /* 3: Serial — replaced with the chip MAC at init */
    "Victrl HID",          /* 4: HID Interface */
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* Last parameter is the endpoint polling interval in milliseconds.
     *
     * This was 10, which meant the host collected at most one keyboard report
     * every 10ms. Typing faster than that silently lost keystrokes: a key-down
     * and its key-up could both fall between two polls, so the character never
     * reached the host at all ("notepad" arriving as "ntpd" is the signature —
     * every other character missing).
     *
     * 1ms is the standard interval for a boot keyboard and the host honours it
     * for interrupt IN endpoints. */
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_desc),
                       0x81, 16, 1),
};

/* ── TinyUSB HID callbacks ─────────────────────────────────────────── */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_descriptor_report_cb_len(uint8_t instance)
{
    (void)instance;
    return sizeof(s_hid_report_desc);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    return 0;
}

/*
 * ── Host lock state ──────────────────────────────────────────────────
 *
 * Windows tells the keyboard which lock LEDs are lit through the HID output
 * report, and this callback used to discard it. It is the ONLY host-side state
 * a HID device is given for free, and it is exactly what decides whether the
 * numeric keypad produces digits or arrow keys — i.e. whether the "type the
 * digits on the keypad" workaround for a hijacking input method is usable at
 * all, or just another blind toggle.
 *
 * Bit 0 Num Lock, bit 1 Caps Lock, bit 2 Scroll Lock (HID keyboard LED report).
 */
#define HID_LED_NUM_LOCK  0x01
#define HID_LED_CAPS_LOCK 0x02
#define HID_LED_SCROLL    0x04

static volatile uint8_t s_led_state = 0;
static volatile bool    s_led_seen = false;

uint8_t hid_keyboard_led_state(void) { return s_led_state; }
bool    hid_keyboard_led_seen(void)  { return s_led_seen; }

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT && buffer && bufsize >= 1) {
        s_led_state = buffer[0];
        s_led_seen = true;
    }
}

/* ── State tracking ────────────────────────────────────────────────── */

static uint16_t s_last_x = 0;
static uint16_t s_last_y = 0;
static uint8_t  s_last_buttons = 0;
static uint32_t s_screen_w = 1920;
static uint32_t s_screen_h = 1080;

/* Human-paced by default; see hid_type_profile_t. */
static hid_type_profile_t s_type = {
    .hold_ms       = 15,
    .gap_ms        = 35,
    .jitter_ms     = 25,
    .word_pause_ms = 120,
};

/* Cheap jitter source. Quality is irrelevant — this only de-correlates the
 * inter-key gaps so the rhythm is not a perfect metronome. A plain LCG avoids
 * pulling in another component just for typing. */
static uint32_t s_jitter_state = 0;

static uint32_t next_jitter(void)
{
    if (s_jitter_state == 0) {
        s_jitter_state = (uint32_t)xTaskGetTickCount() | 1u;
    }
    s_jitter_state = s_jitter_state * 1103515245u + 12345u;
    return (s_jitter_state >> 16) & 0xFFFF;
}

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x303A,
    .idProduct          = 0x4001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* ── Init ──────────────────────────────────────────────────────────── */

esp_err_t hid_device_init(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    /* Use second USB port (OTG1) to avoid conflict with MS2109 on OTG0 */
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;

    /* A per-device serial, derived from the chip's base MAC. The descriptor
     * used to be the fixed string "0001", which identified the model but not
     * the unit — so a host log could say "a Victrl was here" and nothing more.
     * With this, an operator or an auditor can tell which physical device
     * drove which machine. It is readable before any driver binds, so it is
     * available to the target even if the target runs nothing of ours. */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "VIC-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "Cannot read base MAC; using placeholder serial");
    }
    s_usb_strings[3] = s_serial;

    /* Print exactly what a host will read back. This matters because Windows
     * Device Manager names a HID device after the class driver ("HID Keyboard
     * Device"), not after the product string — the product string only shows
     * up as the device's "Bus reported device description". Having all three
     * in the boot log makes "the descriptor did not take effect" a comparison
     * of two concrete strings instead of a guess about which node to look at. */
    ESP_LOGI(TAG, "USB identity: manufacturer=\"%s\", product=\"%s\", serial=\"%s\"",
             s_usb_strings[1], s_usb_strings[2], s_usb_strings[3]);

    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_config_desc;
    tusb_cfg.descriptor.string = s_usb_strings;
    tusb_cfg.descriptor.string_count = sizeof(s_usb_strings) / sizeof(s_usb_strings[0]);

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "HID device initialized (keyboard + absolute mouse) on port 1");
    return ESP_OK;
}

void hid_device_set_screen_size(uint32_t w, uint32_t h)
{
    s_screen_w = w > 0 ? w : 1920;
    s_screen_h = h > 0 ? h : 1080;
}

void hid_device_set_type_profile(const hid_type_profile_t *profile)
{
    if (!profile) return;
    s_type = *profile;
    if (s_type.hold_ms == 0) s_type.hold_ms = 15;
    ESP_LOGI(TAG, "Typing profile: hold=%lums gap=%lums jitter=%lums word_pause=%lums",
             s_type.hold_ms, s_type.gap_ms, s_type.jitter_ms, s_type.word_pause_ms);
}

const hid_type_profile_t *hid_device_get_type_profile(void)
{
    return &s_type;
}

static uint16_t pixel_to_abs(uint32_t pixel, uint32_t screen_size)
{
    if (pixel > screen_size) pixel = screen_size;
    return (uint16_t)((pixel * HID_ABS_MAX) / screen_size);
}

/* ── Mouse operations ──────────────────────────────────────────────── */

void hid_mouse_move_abs(uint32_t pixel_x, uint32_t pixel_y,
                         uint32_t screen_w, uint32_t screen_h)
{
    if (!tud_mounted()) return;

    s_last_x = pixel_to_abs(pixel_x, screen_w);
    s_last_y = pixel_to_abs(pixel_y, screen_h);

    /* Requested pixel vs transmitted absolute value. Together with the cursor
     * position reported in the status line this makes the whole mapping chain
     * (model coords -> pixels -> HID units -> actual pointer) verifiable, which
     * is how to tell "the model aimed badly" apart from "the mapping is off". */
    ESP_LOGD(TAG, "move px(%lu,%lu) -> abs(%u,%u) [screen %lux%lu]",
             pixel_x, pixel_y, s_last_x, s_last_y, screen_w, screen_h);

    hid_send_abs_mouse_report(s_last_buttons, s_last_x, s_last_y);
}

void hid_mouse_click(const char *button)
{
    if (!tud_mounted()) {
        ESP_LOGW(TAG, "click skipped: not mounted");
        return;
    }

    uint8_t btn = 0;
    if (!button || strcmp(button, "left") == 0) btn = HID_MOUSE_BTN_LEFT;
    else if (strcmp(button, "right") == 0) btn = HID_MOUSE_BTN_RIGHT;
    else if (strcmp(button, "middle") == 0) btn = HID_MOUSE_BTN_MIDDLE;
    else if (strcmp(button, "double_left") == 0) {
        hid_mouse_click("left");
        vTaskDelay(pdMS_TO_TICKS(50));
        hid_mouse_click("left");
        return;
    }

    ESP_LOGI(TAG, "CLICK btn=0x%02x abs(%u,%u) mounted=%d", btn, s_last_x, s_last_y, tud_mounted());
    s_last_buttons = btn;
    hid_send_abs_mouse_report(btn, s_last_x, s_last_y);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_last_buttons = 0;
    hid_send_abs_mouse_report(0, s_last_x, s_last_y);
}

void hid_mouse_down(const char *button)
{
    if (!tud_mounted()) return;

    if (!button || strcmp(button, "left") == 0) s_last_buttons |= HID_MOUSE_BTN_LEFT;
    else if (strcmp(button, "right") == 0) s_last_buttons |= HID_MOUSE_BTN_RIGHT;
    else if (strcmp(button, "middle") == 0) s_last_buttons |= HID_MOUSE_BTN_MIDDLE;

    hid_send_abs_mouse_report(s_last_buttons, s_last_x, s_last_y);
}

void hid_mouse_up(const char *button)
{
    if (!tud_mounted()) return;

    if (!button || strcmp(button, "left") == 0) s_last_buttons &= ~HID_MOUSE_BTN_LEFT;
    else if (strcmp(button, "right") == 0) s_last_buttons &= ~HID_MOUSE_BTN_RIGHT;
    else if (strcmp(button, "middle") == 0) s_last_buttons &= ~HID_MOUSE_BTN_MIDDLE;

    hid_send_abs_mouse_report(s_last_buttons, s_last_x, s_last_y);
}

void hid_mouse_scroll(int8_t delta_x, int8_t delta_y)
{
    if (!tud_mounted()) return;
    if (delta_x != 0) {
        /* Horizontal scroll is not in the report descriptor yet — see
         * hid_send_mouse_scroll_report(). Surface it instead of dropping it
         * silently so a model that keeps asking for it is visible in logs. */
        ESP_LOGW(TAG, "horizontal scroll (delta_x=%d) unsupported by descriptor",
                 delta_x);
    }
    /* Repeat the current position/buttons: they are absolute fields. */
    hid_send_mouse_scroll_report(s_last_buttons, s_last_x, s_last_y, delta_y);
}

/* ── Keyboard operations ───────────────────────────────────────────── */

void hid_key_press(const char *key_combo)
{
    if (!tud_hid_ready() || !key_combo) return;

    char combo[64];
    strncpy(combo, key_combo, sizeof(combo) - 1);
    combo[sizeof(combo) - 1] = '\0';

    uint8_t modifiers = 0;
    uint8_t keycodes[6] = {0};
    int key_idx = 0;

    /* strtok_r, not strtok: strtok keeps its cursor in a global, and the LCD
     * refresh task tokenises strings too. Two tasks in strtok() at once
     * corrupt each other's iteration. */
    char *saveptr = NULL;
    char *token = strtok_r(combo, "+", &saveptr);
    while (token && key_idx < 6) {
        while (*token == ' ') token++;

        uint8_t mod = hid_keymap_modifier(token);
        if (mod) {
            modifiers |= mod;
        } else {
            uint8_t code = hid_keymap_lookup(token);
            if (code && key_idx < 6) {
                keycodes[key_idx++] = code;
            }
        }
        token = strtok_r(NULL, "+", &saveptr);
    }

    uint8_t empty[6] = {0};
    /* 50ms hold: comfortably longer than any host's polling interval, so a
     * chord can never be missed. */
    hid_send_keyboard_report(modifiers, keycodes);
    vTaskDelay(pdMS_TO_TICKS(50));
    hid_send_keyboard_report(0, empty);
}

/*
 * ── Numeric-keypad digit path ────────────────────────────────────────
 *
 * Keypad usage codes (HID Usage Page 0x07): 0x59 = '1' ... 0x61 = '9',
 * 0x62 = '0'. Written as literals because TinyUSB's names for these have
 * changed between revisions, while the spec values have not.
 */
static uint8_t keypad_digit_code(char c)
{
    if (c == '0') return 0x62;
    if (c >= '1' && c <= '9') return (uint8_t)(0x58 + (c - '0'));
    return 0;
}

static bool s_digits_on_keypad = false;

void hid_type_set_digits_on_keypad(bool on)
{
    s_digits_on_keypad = on;
}

bool hid_ensure_num_lock(void)
{
    if (!s_led_seen) {
        /* Without the host's LED report the current state is unknown, and
         * "press Num Lock" would be a coin flip that can just as easily turn
         * it OFF. Refuse rather than guess. */
        ESP_LOGW(TAG, "Num Lock state unknown: no LED report from the host yet");
        return false;
    }
    if (s_led_state & HID_LED_NUM_LOCK) return true;

    ESP_LOGI(TAG, "Num Lock is off — turning it on");
    hid_key_press("numlock");

    /* Up to ~600ms for the host to echo the new LED state back. The report is
     * the feedback; without it this would be another unobservable toggle. */
    for (int i = 0; i < 12; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_led_state & HID_LED_NUM_LOCK) return true;
    }
    ESP_LOGW(TAG, "Num Lock did not turn on (host never reported it)");
    return false;
}

hid_type_result_t hid_type_string(const char *text)
{
    hid_type_result_t result = {0};
    if (!text) return result;
    if (!tud_mounted()) {
        ESP_LOGW(TAG, "type skipped: not mounted");
        return result;
    }

    size_t len = strlen(text);
    result.len = len;

    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        uint8_t code = 0;
        uint8_t mod = 0;

        if (c >= 'a' && c <= 'z') {
            code = HID_KEY_A + (c - 'a');
        } else if (c >= 'A' && c <= 'Z') {
            code = HID_KEY_A + (c - 'A');
            mod = MOD_SHIFT;
        } else if (c >= '0' && c <= '9') {
            /* The keypad path exists because a Chinese IME in composition mode
             * eats the MAIN number row as candidate selectors while usually
             * passing the keypad through. Never used unless Num Lock was
             * confirmed on — otherwise the keypad emits arrows. */
            uint8_t kp = s_digits_on_keypad ? keypad_digit_code(c) : 0;
            code = kp ? kp : (uint8_t)(HID_KEY_0 + (c - '0'));
        } else if (c == ' ') {
            code = HID_KEY_SPACE;
        } else if (c == '\n' || c == '\r') {
            code = HID_KEY_ENTER;
        } else if (c == '\t') {
            code = HID_KEY_TAB;
        } else {
            char sym[2] = {c, 0};
            code = hid_keymap_lookup(sym);
            if (!code) {
                /* Unmappable (typically non-ASCII/CJK) — skip it. */
                ESP_LOGD(TAG, "type: no keymap for 0x%02X, skipped",
                         (unsigned)c);
                result.skipped++;
                continue;
            }
            /* Shift depends on the CHARACTER, not the resolved keycode:
             * "-" and "_" are both HID_KEY_MINUS, so a keycode test would
             * make every plain symbol type as its shifted twin. */
            if (hid_keymap_char_needs_shift(sym)) mod = MOD_SHIFT;
        }

        if (!code) continue;

        uint8_t keycodes[6] = {code, 0};
        uint8_t empty[6] = {0};

        if (!hid_send_keyboard_report(mod, keycodes)) {
            /* The endpoint refused the report; retrying the same character is
             * the only safe recovery, and it must be reported loudly because
             * it means a character may be missing from the field. */
            ESP_LOGW(TAG, "type: key-down for '%c' (0x%02X) was not queued", c,
                     (unsigned)code);
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!hid_send_keyboard_report(mod, keycodes)) {
                ESP_LOGE(TAG, "type: giving up on '%c' — output will be short", c);
                continue;
            }
        }
        result.queued++;

        vTaskDelay(pdMS_TO_TICKS(s_type.hold_ms));
        if (!hid_send_keyboard_report(0, empty)) {
            ESP_LOGW(TAG, "type: key-up for '%c' was not queued — the host may "
                          "see a stuck key", c);
        }

        /* Human-like rhythm. Reliability, not speed: each keystroke needs the
         * host's keyboard driver and the focused control to process it before
         * the next one arrives, otherwise characters are simply lost. */
        uint32_t gap = s_type.gap_ms;
        if (s_type.jitter_ms) gap += next_jitter() % s_type.jitter_ms;
        if (c == ' ') gap += s_type.word_pause_ms;
        vTaskDelay(pdMS_TO_TICKS(gap));
    }

    if (result.skipped) {
        ESP_LOGW(TAG, "type: %u of %u characters had no key mapping",
                 (unsigned)result.skipped, (unsigned)len);
    }
    /* This line is the evidence for "device sent it, target swallowed it". */
    ESP_LOGI(TAG, "typed %u/%u chars (skipped %u): \"%s\"",
             (unsigned)result.queued, (unsigned)len,
             (unsigned)result.skipped, text);
    return result;
}

void hid_release_all(void)
{
    uint8_t empty[6] = {0};
    hid_send_keyboard_report(0, empty);
    s_last_buttons = 0;
    hid_send_abs_mouse_report(0, s_last_x, s_last_y);
}

void hid_mouse_get_state(uint16_t *abs_x, uint16_t *abs_y, uint8_t *buttons,
                          uint32_t *screen_w, uint32_t *screen_h)
{
    if (abs_x) *abs_x = s_last_x;
    if (abs_y) *abs_y = s_last_y;
    if (buttons) *buttons = s_last_buttons;
    if (screen_w) *screen_w = s_screen_w;
    if (screen_h) *screen_h = s_screen_h;
}
