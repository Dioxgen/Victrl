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

    def key_press(self, key_combo: str) -> None:
        self._send(f"K {key_combo}")

    def type_string(self, text: str, delay_ms: float = 5.0) -> None:
        encoded = base64.b64encode(text.encode("utf-8")).decode("ascii")
        self._send(f"T {encoded}")

    def release_all(self) -> None:
        self._send("R")
        self._pressed_buttons.clear()

    def close(self) -> None:
        self.release_all()
        if self._ser and self._ser.is_open:
            self._ser.close()
            self._ser = None
            logger.info("SerialHidBridge disconnected")
