"""HID controller using Linux uinput via evdev."""

import logging
import time

from utils.exceptions import HidError

logger = logging.getLogger("victrl.hid")


class HidController:
    """Simulate keyboard and mouse via uinput."""

    # Key name to evdev key code mapping for common keys
    KEY_MAP = {
        "a": 30, "b": 48, "c": 46, "d": 32, "e": 18, "f": 33, "g": 34,
        "h": 35, "i": 23, "j": 36, "k": 37, "l": 38, "m": 50, "n": 49,
        "o": 24, "p": 25, "q": 16, "r": 19, "s": 31, "t": 20, "u": 22,
        "v": 47, "w": 17, "x": 45, "y": 21, "z": 44,
        "0": 11, "1": 2, "2": 3, "3": 4, "4": 5, "5": 6, "6": 7, "7": 8,
        "8": 9, "9": 10,
        "space": 57, "enter": 28, "tab": 15, "backspace": 14, "escape": 1,
        "esc": 1,  # alias for escape
        "left": 105, "right": 106, "up": 103, "down": 108,
        "shift": 42, "ctrl": 29, "alt": 56, "super": 125,
        "f1": 59, "f2": 60, "f3": 61, "f4": 62, "f5": 63, "f6": 64,
        "f7": 65, "f8": 66, "f9": 67, "f10": 68, "f11": 87, "f12": 88,
        "capslock": 58, "numlock": 69,
        "delete": 111, "insert": 110, "home": 102, "end": 107,
        "pageup": 104, "pagedown": 109,
        "printscreen": 99, "pause": 119, "scrolllock": 70,
        "minus": 12, "equal": 13, "leftbrace": 26, "rightbrace": 27,
        "semicolon": 39, "apostrophe": 40, "grave": 41, "backslash": 43,
        "comma": 51, "dot": 52, "slash": 53,
        "kp1": 79, "kp2": 80, "kp3": 81, "kp4": 75, "kp5": 76,
        "kp6": 77, "kp7": 71, "kp8": 72, "kp9": 73, "kp0": 82,
        "kpslash": 98, "kpasterisk": 55, "kpminus": 74, "kpplus": 78,
        "kpenter": 96, "kpperiod": 83, "kpdot": 83,
    }

    SHIFT_MAP = {
        "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
        "&": "7", "*": "8", "(": "9", ")": "0", "_": "-", "+": "=",
        "{": "[", "}": "]", "|": "\\", ":": ";", '"': "'", "<": ",",
        ">": ".", "?": "/", "~": "`",
    }

    def __init__(self, dry_run: bool = False):
        """Initialize HID controller.

        Args:
            dry_run: If True, log actions instead of executing them.
        """
        self.dry_run = dry_run
        self._pressed_buttons = set()
        self._ui = None
        self._screen_width = 1280
        self._screen_height = 720
        self._evdev = None
        self._ecodes = None
        self._uinput_available = False

        if not dry_run:
            self._init_uinput()
        else:
            logger.info("HID controller running in dry-run mode")

    def _init_uinput(self) -> None:
        """Initialize uinput device for keyboard and absolute mouse."""
        try:
            import evdev
            from evdev import ecodes as e

            self._evdev = evdev
            self._ecodes = e
            self._abs_max = 32767  # logical coordinate range, same as ESP32

            keys = [
                e.KEY_A, e.KEY_B, e.KEY_C, e.KEY_D, e.KEY_E, e.KEY_F,
                e.KEY_G, e.KEY_H, e.KEY_I, e.KEY_J, e.KEY_K, e.KEY_L,
                e.KEY_M, e.KEY_N, e.KEY_O, e.KEY_P, e.KEY_Q, e.KEY_R,
                e.KEY_S, e.KEY_T, e.KEY_U, e.KEY_V, e.KEY_W, e.KEY_X,
                e.KEY_Y, e.KEY_Z, e.KEY_0, e.KEY_1, e.KEY_2, e.KEY_3,
                e.KEY_4, e.KEY_5, e.KEY_6, e.KEY_7, e.KEY_8, e.KEY_9,
                e.KEY_SPACE, e.KEY_ENTER, e.KEY_TAB, e.KEY_BACKSPACE,
                e.KEY_ESC, e.KEY_LEFT, e.KEY_RIGHT, e.KEY_UP, e.KEY_DOWN,
                e.KEY_LEFTSHIFT, e.KEY_RIGHTSHIFT, e.KEY_LEFTCTRL,
                e.KEY_RIGHTCTRL, e.KEY_LEFTALT, e.KEY_RIGHTALT,
                e.KEY_LEFTMETA, e.KEY_RIGHTMETA, e.KEY_F1, e.KEY_F2,
                e.KEY_F3, e.KEY_F4, e.KEY_F5, e.KEY_F6, e.KEY_F7,
                e.KEY_F8, e.KEY_F9, e.KEY_F10, e.KEY_F11, e.KEY_F12,
                e.KEY_CAPSLOCK, e.KEY_NUMLOCK, e.KEY_MINUS, e.KEY_EQUAL,
                e.KEY_LEFTBRACE, e.KEY_RIGHTBRACE, e.KEY_SEMICOLON,
                e.KEY_APOSTROPHE, e.KEY_GRAVE, e.KEY_BACKSLASH,
                e.KEY_COMMA, e.KEY_DOT, e.KEY_SLASH,
                e.KEY_DELETE, e.KEY_INSERT, e.KEY_HOME, e.KEY_END,
                e.KEY_PAGEUP, e.KEY_PAGEDOWN, e.KEY_SCROLLLOCK,
                e.KEY_PAUSE, e.KEY_SYSRQ, e.KEY_KPSLASH, e.KEY_KPASTERISK,
                e.KEY_KPMINUS, e.KEY_KPPLUS, e.KEY_KPENTER,
                e.KEY_KP1, e.KEY_KP2, e.KEY_KP3, e.KEY_KP4, e.KEY_KP5,
                e.KEY_KP6, e.KEY_KP7, e.KEY_KP8, e.KEY_KP9, e.KEY_KP0,
                e.KEY_KPDOT,
            ]

            abs_info = evdev.AbsInfo(
                value=0, min=0, max=self._abs_max, fuzz=0, flat=0, resolution=0
            )

            self._ui = evdev.UInput(
                name="Victrl Virtual HID",
                events={
                    e.EV_KEY: keys,
                    e.EV_REL: [
                        e.REL_WHEEL,
                        e.REL_HWHEEL,
                    ],
                    e.EV_ABS: [
                        (e.ABS_X, abs_info),
                        (e.ABS_Y, abs_info),
                    ],
                },
            )

            self._uinput_available = True
            logger.info("uinput HID device created (absolute mouse)")

        except ImportError:
            logger.warning("evdev not installed, HID operations will be logged only")
            self.dry_run = True
        except PermissionError:
            logger.error("Permission denied for /dev/uinput. Run with sudo.")
            self.dry_run = True
        except Exception as e:
            logger.error(f"Failed to initialize uinput: {e}")
            self.dry_run = True

    def set_screen_size(self, width: int, height: int) -> None:
        """Set the target screen resolution for coordinate mapping."""
        self._screen_width = width
        self._screen_height = height

    def mouse_move_abs(self, x: int, y: int) -> None:
        """Move mouse to absolute pixel coordinates.

        Args:
            x: Target X coordinate in pixels (0 .. screen_width).
            y: Target Y coordinate in pixels (0 .. screen_height).
        """
        if self.dry_run:
            logger.info(f"[DRY] mouse_move_abs({x}, {y})")
            return
        try:
            if self._ui:
                # Map pixel → logical 0..32767 (matches ESP32 absolute range)
                abs_x = int(x * self._abs_max / self._screen_width)
                abs_y = int(y * self._abs_max / self._screen_height)
                self._ui.write(self._ecodes.EV_ABS, self._ecodes.ABS_X, abs_x)
                self._ui.write(self._ecodes.EV_ABS, self._ecodes.ABS_Y, abs_y)
                self._ui.syn()
        except Exception as e:
            logger.error(f"mouse_move_abs failed: {e}")

    def _btn_code(self, button: str):
        """Convert button name to evdev code."""
        if button == "left":
            return self._ecodes.BTN_LEFT
        elif button == "right":
            return self._ecodes.BTN_RIGHT
        elif button == "middle":
            return self._ecodes.BTN_MIDDLE
        raise HidError(f"Unknown button: {button}")

    def mouse_down(self, button: str = "left") -> None:
        """Press a mouse button.

        Args:
            button: "left", "right", or "middle".
        """
        self._pressed_buttons.add(button)
        if self.dry_run:
            logger.info(f"[DRY] mouse_down({button})")
            return
        try:
            if self._ui:
                self._ui.write(self._ecodes.EV_KEY, self._btn_code(button), 1)
                self._ui.syn()
        except Exception as e:
            logger.error(f"mouse_down failed: {e}")

    def mouse_up(self, button: str = "left") -> None:
        """Release a mouse button.

        Args:
            button: "left", "right", or "middle".
        """
        self._pressed_buttons.discard(button)
        if self.dry_run:
            logger.info(f"[DRY] mouse_up({button})")
            return
        try:
            if self._ui:
                self._ui.write(self._ecodes.EV_KEY, self._btn_code(button), 0)
                self._ui.syn()
        except Exception as e:
            logger.error(f"mouse_up failed: {e}")

    def mouse_click(self, button: str = "left", double: bool = False) -> None:
        """Click a mouse button.

        Args:
            button: "left", "right", "middle", or "double_left".
            double: If True, perform double-click.
        """
        if button == "double_left":
            button = "left"
            double = True

        self.mouse_down(button)
        time.sleep(0.05)
        self.mouse_up(button)

        if double:
            time.sleep(0.05)
            self.mouse_down(button)
            time.sleep(0.05)
            self.mouse_up(button)

    def mouse_scroll(self, delta_x: int = 0, delta_y: int = 0) -> None:
        """Scroll the mouse wheel.

        Args:
            delta_x: Horizontal scroll amount.
            delta_y: Vertical scroll amount.
        """
        if self.dry_run:
            logger.info(f"[DRY] mouse_scroll(dx={delta_x}, dy={delta_y})")
            return
        try:
            if self._ui:
                if delta_y:
                    self._ui.write(self._ecodes.EV_REL, self._ecodes.REL_WHEEL, delta_y)
                if delta_x:
                    self._ui.write(self._ecodes.EV_REL, self._ecodes.REL_HWHEEL, delta_x)
                self._ui.syn()
        except Exception as e:
            logger.error(f"mouse_scroll failed: {e}")

    def key_press(self, key_combo: str) -> None:
        """Press a key or key combination (e.g. "Ctrl+C", "Enter").

        Args:
            key_combo: Key name or combination joined by "+".
        """
        keys = [k.strip().lower() for k in key_combo.split("+")]
        modifiers = {"ctrl": self._ecodes.KEY_LEFTCTRL if self._ecodes else 29,
                     "shift": self._ecodes.KEY_LEFTSHIFT if self._ecodes else 42,
                     "alt": self._ecodes.KEY_LEFTALT if self._ecodes else 56,
                     "super": self._ecodes.KEY_LEFTMETA if self._ecodes else 125}

        if self.dry_run:
            logger.info(f"[DRY] key_press({key_combo})")
            return

        try:
            if not self._ui:
                return
            # Press modifiers
            pressed_mods = []
            for k in keys:
                if k in modifiers:
                    self._ui.write(self._ecodes.EV_KEY, modifiers[k], 1)
                    pressed_mods.append(k)
            self._ui.syn()

            # Press main key
            main_key = [k for k in keys if k not in modifiers]
            if main_key:
                key_name = main_key[0]
                if key_name in self.KEY_MAP:
                    self._ui.write(self._ecodes.EV_KEY, self.KEY_MAP[key_name], 1)
                    self._ui.syn()
                    time.sleep(0.02)
                    self._ui.write(self._ecodes.EV_KEY, self.KEY_MAP[key_name], 0)
                    self._ui.syn()

            # Release modifiers in reverse order
            for k in reversed(pressed_mods):
                self._ui.write(self._ecodes.EV_KEY, modifiers[k], 0)
                self._ui.syn()

        except Exception as e:
            logger.error(f"key_press failed: {e}")

    def type_string(self, text: str, delay_ms: float = 25.0) -> None:
        """Type a string of text.

        Args:
            text: The text to type.
            delay_ms: Delay between keystrokes in milliseconds.
        """
        if self.dry_run:
            logger.info(f"[DRY] type_string({repr(text)})")
            return

        delay = delay_ms / 1000.0
        try:
            if not self._ui:
                return
            for ch in text:
                needs_shift = ch in self.SHIFT_MAP
                if needs_shift:
                    base_ch = self.SHIFT_MAP[ch]
                    self._ui.write(self._ecodes.EV_KEY, self._ecodes.KEY_LEFTSHIFT, 1)
                    self._ui.syn()
                else:
                    base_ch = ch.lower() if ch.isalpha() else ch

                key_name = {
                    " ": "space", "\n": "enter", "\t": "tab",
                    "-": "minus", "=": "equal", "[": "leftbrace",
                    "]": "rightbrace", ";": "semicolon", "'": "apostrophe",
                    "`": "grave", "\\": "backslash", ",": "comma",
                    ".": "dot", "/": "slash",
                }.get(base_ch, base_ch)

                if key_name in self.KEY_MAP:
                    self._ui.write(self._ecodes.EV_KEY, self.KEY_MAP[key_name], 1)
                    self._ui.syn()
                    time.sleep(0.01)
                    self._ui.write(self._ecodes.EV_KEY, self.KEY_MAP[key_name], 0)
                    self._ui.syn()

                if needs_shift:
                    self._ui.write(self._ecodes.EV_KEY, self._ecodes.KEY_LEFTSHIFT, 0)
                    self._ui.syn()

                if delay:
                    time.sleep(delay)

        except Exception as e:
            logger.error(f"type_string failed: {e}")

    def release_all(self) -> None:
        """Release all currently pressed buttons."""
        if self.dry_run:
            logger.info("[DRY] release_all()")
            self._pressed_buttons.clear()
            return
        try:
            if self._ui:
                for button in list(self._pressed_buttons):
                    self.mouse_up(button)
                self._ui.syn()
        except Exception as e:
            logger.error(f"release_all failed: {e}")

    def close(self) -> None:
        """Release HID resources and close uinput device."""
        self.release_all()
        if self._ui:
            try:
                self._ui.close()
            except Exception:
                pass
            self._ui = None
