#include "hid_keymap.h"

#include <string.h>

static const key_entry_t s_keymap[] = {
    {.name = "a", .code = HID_KEY_A}, {.name = "b", .code = HID_KEY_B},
    {.name = "c", .code = HID_KEY_C}, {.name = "d", .code = HID_KEY_D},
    {.name = "e", .code = HID_KEY_E}, {.name = "f", .code = HID_KEY_F},
    {.name = "g", .code = HID_KEY_G}, {.name = "h", .code = HID_KEY_H},
    {.name = "i", .code = HID_KEY_I}, {.name = "j", .code = HID_KEY_J},
    {.name = "k", .code = HID_KEY_K}, {.name = "l", .code = HID_KEY_L},
    {.name = "m", .code = HID_KEY_M}, {.name = "n", .code = HID_KEY_N},
    {.name = "o", .code = HID_KEY_O}, {.name = "p", .code = HID_KEY_P},
    {.name = "q", .code = HID_KEY_Q}, {.name = "r", .code = HID_KEY_R},
    {.name = "s", .code = HID_KEY_S}, {.name = "t", .code = HID_KEY_T},
    {.name = "u", .code = HID_KEY_U}, {.name = "v", .code = HID_KEY_V},
    {.name = "w", .code = HID_KEY_W}, {.name = "x", .code = HID_KEY_X},
    {.name = "y", .code = HID_KEY_Y}, {.name = "z", .code = HID_KEY_Z},
    {.name = "0", .code = HID_KEY_0}, {.name = "1", .code = HID_KEY_1},
    {.name = "2", .code = HID_KEY_2}, {.name = "3", .code = HID_KEY_3},
    {.name = "4", .code = HID_KEY_4}, {.name = "5", .code = HID_KEY_5},
    {.name = "6", .code = HID_KEY_6}, {.name = "7", .code = HID_KEY_7},
    {.name = "8", .code = HID_KEY_8}, {.name = "9", .code = HID_KEY_9},
    {.name = "f1", .code = HID_KEY_F1}, {.name = "f2", .code = HID_KEY_F2},
    {.name = "f3", .code = HID_KEY_F3}, {.name = "f4", .code = HID_KEY_F4},
    {.name = "f5", .code = HID_KEY_F5}, {.name = "f6", .code = HID_KEY_F6},
    {.name = "f7", .code = HID_KEY_F7}, {.name = "f8", .code = HID_KEY_F8},
    {.name = "f9", .code = HID_KEY_F9}, {.name = "f10", .code = HID_KEY_F10},
    {.name = "f11", .code = HID_KEY_F11}, {.name = "f12", .code = HID_KEY_F12},
    {.name = "enter", .code = HID_KEY_ENTER},
    {.name = "return", .code = HID_KEY_ENTER},
    {.name = "escape", .code = HID_KEY_ESCAPE},
    {.name = "esc", .code = HID_KEY_ESCAPE},
    {.name = "backspace", .code = HID_KEY_BACKSPACE},
    {.name = "bsp", .code = HID_KEY_BACKSPACE},
    {.name = "tab", .code = HID_KEY_TAB},
    {.name = "space", .code = HID_KEY_SPACE},
    {.name = " ", .code = HID_KEY_SPACE},
    {.name = "-", .code = HID_KEY_MINUS},
    {.name = "=", .code = HID_KEY_EQUAL},
    {.name = "[", .code = HID_KEY_BRACKET_LEFT},
    {.name = "]", .code = HID_KEY_BRACKET_RIGHT},
    {.name = "\\", .code = HID_KEY_BACKSLASH},
    {.name = ";", .code = HID_KEY_SEMICOLON},
    {.name = "'", .code = HID_KEY_APOSTROPHE},
    {.name = "`", .code = HID_KEY_GRAVE},
    {.name = ",", .code = HID_KEY_COMMA},
    {.name = ".", .code = HID_KEY_PERIOD},
    {.name = "/", .code = HID_KEY_SLASH},
    {.name = "capslock", .code = HID_KEY_CAPS_LOCK},
    {.name = "printscreen", .code = HID_KEY_PRINT_SCREEN},
    {.name = "scrolllock", .code = HID_KEY_SCROLL_LOCK},
    {.name = "pause", .code = HID_KEY_PAUSE},
    {.name = "insert", .code = HID_KEY_INSERT},
    {.name = "ins", .code = HID_KEY_INSERT},
    {.name = "home", .code = HID_KEY_HOME},
    {.name = "pageup", .code = HID_KEY_PAGE_UP},
    {.name = "pgup", .code = HID_KEY_PAGE_UP},
    {.name = "delete", .code = HID_KEY_DELETE},
    {.name = "del", .code = HID_KEY_DELETE},
    {.name = "end", .code = HID_KEY_END},
    {.name = "pagedown", .code = HID_KEY_PAGE_DOWN},
    {.name = "pgdn", .code = HID_KEY_PAGE_DOWN},
    {.name = "rightarrow", .code = HID_KEY_ARROW_RIGHT},
    {.name = "right", .code = HID_KEY_ARROW_RIGHT},
    {.name = "leftarrow", .code = HID_KEY_ARROW_LEFT},
    {.name = "left", .code = HID_KEY_ARROW_LEFT},
    {.name = "downarrow", .code = HID_KEY_ARROW_DOWN},
    {.name = "down", .code = HID_KEY_ARROW_DOWN},
    {.name = "uparrow", .code = HID_KEY_ARROW_UP},
    {.name = "up", .code = HID_KEY_ARROW_UP},
    {.name = "numlock", .code = HID_KEY_NUM_LOCK},
    {.name = "kp_slash", .code = HID_KEY_KEYPAD_DIVIDE},
    {.name = "kp_asterisk", .code = HID_KEY_KEYPAD_MULTIPLY},
    {.name = "kp_minus", .code = HID_KEY_KEYPAD_SUBTRACT},
    {.name = "kp_plus", .code = HID_KEY_KEYPAD_ADD},
    {.name = "kp_enter", .code = HID_KEY_KEYPAD_ENTER},
    {.name = "kp_1", .code = HID_KEY_KEYPAD_1},
    {.name = "kp_2", .code = HID_KEY_KEYPAD_2},
    {.name = "kp_3", .code = HID_KEY_KEYPAD_3},
    {.name = "kp_4", .code = HID_KEY_KEYPAD_4},
    {.name = "kp_5", .code = HID_KEY_KEYPAD_5},
    {.name = "kp_6", .code = HID_KEY_KEYPAD_6},
    {.name = "kp_7", .code = HID_KEY_KEYPAD_7},
    {.name = "kp_8", .code = HID_KEY_KEYPAD_8},
    {.name = "kp_9", .code = HID_KEY_KEYPAD_9},
    {.name = "kp_0", .code = HID_KEY_KEYPAD_0},
    {.name = "kp_dot", .code = HID_KEY_KEYPAD_DECIMAL},
    {.name = "application", .code = HID_KEY_APPLICATION},
    {.name = "power", .code = HID_KEY_POWER},
    {.name = "menu", .code = HID_KEY_MENU},
    {.name = NULL, .code = 0},
};

/* Shifted symbol → base key */
typedef struct { const char *sym; uint8_t base; } shift_entry_t;
static const shift_entry_t s_shift_map[] = {
    {.sym = "!", .base = HID_KEY_1}, {.sym = "@", .base = HID_KEY_2},
    {.sym = "#", .base = HID_KEY_3}, {.sym = "$", .base = HID_KEY_4},
    {.sym = "%", .base = HID_KEY_5}, {.sym = "^", .base = HID_KEY_6},
    {.sym = "&", .base = HID_KEY_7}, {.sym = "*", .base = HID_KEY_8},
    {.sym = "(", .base = HID_KEY_9}, {.sym = ")", .base = HID_KEY_0},
    {.sym = "_", .base = HID_KEY_MINUS}, {.sym = "+", .base = HID_KEY_EQUAL},
    {.sym = "{", .base = HID_KEY_BRACKET_LEFT},
    {.sym = "}", .base = HID_KEY_BRACKET_RIGHT},
    {.sym = "|", .base = HID_KEY_BACKSLASH},
    {.sym = ":", .base = HID_KEY_SEMICOLON},
    {.sym = "\"", .base = HID_KEY_APOSTROPHE},
    {.sym = "~", .base = HID_KEY_GRAVE},
    {.sym = "<", .base = HID_KEY_COMMA},
    {.sym = ">", .base = HID_KEY_PERIOD},
    {.sym = "?", .base = HID_KEY_SLASH},
    {.sym = NULL, .base = 0},
};

uint8_t hid_keymap_lookup(const char *name)
{
    if (!name) return 0;
    for (const key_entry_t *e = s_keymap; e->name; e++) {
        if (strcasecmp(name, e->name) == 0) return e->code;
    }
    for (const shift_entry_t *s = s_shift_map; s->sym; s++) {
        if (strcmp(name, s->sym) == 0) return s->base;
    }
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z')
        return HID_KEY_A + (name[0] - 'a');
    if (strlen(name) == 1 && name[0] >= '0' && name[0] <= '9')
        return HID_KEY_0 + (name[0] - '0');
    return 0;
}

uint8_t hid_keymap_modifier(const char *name)
{
    if (!name) return 0;
    if (strcasecmp(name, "ctrl") == 0) return MOD_CTRL;
    if (strcasecmp(name, "shift") == 0) return MOD_SHIFT;
    if (strcasecmp(name, "alt") == 0) return MOD_ALT;
    if (strcasecmp(name, "gui") == 0 || strcasecmp(name, "win") == 0) return MOD_GUI;
    return 0;
}

/*
 * Shift requirement is a property of the CHARACTER, not of the resolved key
 * code. Resolving first and then testing the code is wrong because each
 * unshifted symbol shares its code with a shifted one ("-" / "_" both map to
 * HID_KEY_MINUS, "=" / "+" to HID_KEY_EQUAL, and so on), which made every
 * plain symbol get typed as its shifted form.
 */
int hid_keymap_char_needs_shift(const char *sym)
{
    if (!sym || !sym[0]) return 0;
    for (const shift_entry_t *s = s_shift_map; s->sym; s++) {
        if (strcmp(sym, s->sym) == 0) return 1;
    }
    return 0;
}
