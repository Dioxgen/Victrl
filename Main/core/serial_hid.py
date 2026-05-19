"""Serial HID bridge — sends HID commands to ESP32 BLE HID over UART."""

import base64
import logging
import time

import serial

from utils.exceptions import HidError

logger = logging.getLogger("victrl.serial_hid")

# Serial protocol (one command per line, '\n' delimited):
#   M <x> <y>         mouse move absolute (pixels)
#   D <button>        mouse down (left/right/middle)
#   U <button>        mouse up
#   C <button>        mouse click (left/right/middle/double_left)
#   S <dx> <dy>       scroll
#   K <combo>         key press (e.g. "ctrl+c", "enter")
#   T <b64text>       type string (base64 encoded)
#   R                 release all
#   W <ms>            wait milliseconds


class SerialHidBridge:
    """Sends HID actions to ESP32 over serial UART. Identical API to HidController."""

    def __init__(self, port: str = "/dev/ttyUSB0", baudrate: int = 115200,
                 dry_run: bool = False):
        self.port = port
        self.baudrate = baudrate
        self.dry_run = dry_run
        self._ser: serial.Serial | None = None
        self._pressed_buttons: set[str] = set()
        self._screen_width = 1280
        self._screen_height = 720

        if not dry_run:
            self._connect()
        else:
            logger.info("SerialHidBridge running in dry-run mode")

    def _connect(self) -> None:
        """Open serial connection to ESP32."""
        try:
            self._ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=1.0,
                write_timeout=1.0,
            )
            time.sleep(0.1)
            self._ser.reset_input_buffer()
            logger.info(f"SerialHidBridge connected: {self.port} @ {self.baudrate}")
        except serial.SerialException as e:
            logger.error(f"Failed to open serial port {self.port}: {e}")
            self.dry_run = True
        except ImportError:
            logger.warning("pyserial not installed, falling back to dry-run")
            self.dry_run = True

    def _send(self, cmd: str) -> None:
        """Send a command over serial."""
        if self.dry_run or self._ser is None:
            logger.info(f"[ESP32-DRY] {cmd}")
            return
        try:
            self._ser.write(f"{cmd}\n".encode("ascii"))
            self._ser.flush()
        except serial.SerialException as e:
            logger.error(f"Serial write failed: {e}")
            # ESP32 likely disconnected — fall back to dry-run so agent
            # doesn't keep trying to write to a dead port.
            self.dry_run = True
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None
            logger.critical("SerialHidBridge disconnected — all subsequent HID commands will be NO-OP")

    def _wait_ack(self) -> bool:
        """Read one line of acknowledgement from ESP32 (non-blocking best-effort)."""
        if self.dry_run or self._ser is None:
            return True
        try:
            if self._ser.in_waiting:
                line = self._ser.readline().decode("ascii", errors="replace").strip()
                logger.debug(f"ESP32: {line}")
                return line.startswith("OK")
        except serial.SerialException:
            pass
        return True  # don't block on ack timeout

    def set_screen_size(self, width: int, height: int) -> None:
        self._screen_width = width
        self._screen_height = height
        self._send(f"Z {width} {height}")
        time.sleep(0.02)  # let ESP32 process before next command

    def mouse_move_abs(self, x: int, y: int) -> None:
        self._send(f"M {x} {y}")

    def mouse_down(self, button: str = "left") -> None:
        self._pressed_buttons.add(button)
        self._send(f"D {button}")

    def mouse_up(self, button: str = "left") -> None:
        self._pressed_buttons.discard(button)
        self._send(f"U {button}")

    def mouse_click(self, button: str = "left", double: bool = False) -> None:
        if button == "double_left":
            button = "left"
            double = True
        self._send(f"C {button}")
        if double:
            time.sleep(0.05)
            self._send(f"C {button}")

    def mouse_scroll(self, delta_x: int = 0, delta_y: int = 0) -> None:
        self._send(f"S {delta_x} {delta_y}")

    # Special keys that the ESP32 K command handles unreliably due to a
    # PROGMEM lookup bug for multi-character key names. For these we use
    # the T (type) command with the corresponding ASCII control character
    # which the ESP32 firmware handles correctly.
    _K_TO_CHAR = {
        "enter":     "\n",
        "tab":       "\t",
        "backspace": "\b",
        "escape":    "\x1b",
        "space":     " ",
    }

    def key_press(self, key_combo: str) -> None:
        # Route named special keys through the T (type) command to avoid
        # ESP32 firmware PROGMEM lookup bug for multi-char key names.
        key_lower = key_combo.lower()
        if key_lower in self._K_TO_CHAR:
            encoded = base64.b64encode(
                self._K_TO_CHAR[key_lower].encode("utf-8")
            ).decode("ascii")
            self._send(f"T {encoded}")
            return
        self._send(f"K {key_combo}")

    # US QWERTY shift mapping — which unshifted key + shift = the desired char
    _SHIFT_MAP = {
        '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
        '*': '8', '(': '9', ')': '0', '_': '-', '+': '=', '{': '[', '}': ']',
        '|': '\\', ':': ';', '"': "'", '<': ',', '>': '.', '?': '/', '~': '`',
    }

    def type_string(self, text: str, delay_ms: float = 25.0) -> None:
        # Send each character individually via K command. The ESP32 T command's
        # base64 path drops characters at BLE speeds (5-15ms per char) and the
        # firmware's asciiToHid() doesn't map shifted symbols.  Sending from
        # Python with explicit shift handling and 25ms spacing is reliable.
        import time as _time
        for ch in text:
            if ch == '\n':
                self._send("K enter")
            elif ch == '\t':
                self._send("K tab")
            elif ch == ' ':
                encoded = base64.b64encode(b' ').decode("ascii")
                self._send(f"T {encoded}")
            elif 'A' <= ch <= 'Z':
                self._send(f"K shift+{ch.lower()}")
            elif ch in self._SHIFT_MAP:
                self._send(f"K shift+{self._SHIFT_MAP[ch]}")
            else:
                self._send(f"K {ch}")
            _time.sleep(delay_ms / 1000.0)

    def release_all(self) -> None:
        self._send("R")
        self._pressed_buttons.clear()

    def close(self) -> None:
        self.release_all()
        if self._ser and self._ser.is_open:
            self._ser.close()
            self._ser = None
            logger.info("SerialHidBridge disconnected")
