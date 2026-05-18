"""System/hardware detection utilities."""

import glob
import logging
import os

logger = logging.getLogger("victrl.utils")


def check_uinput() -> bool:
    """Check if /dev/uinput is available.

    Returns:
        True if uinput exists, False otherwise.
    """
    if os.path.exists("/dev/uinput"):
        return True
    logger.error("/dev/uinput not found. Run: sudo modprobe uinput")
    return False


def find_uvc_devices() -> list:
    """Scan /dev/video* for devices with video capture capability.

    Returns:
        List of device paths that support V4L2_CAP_VIDEO_CAPTURE.
    """
    devices = []
    for path in sorted(glob.glob("/dev/video*")):
        try:
            with open(path, "rb") as f:
                # Basic existence check; full capability query would need ioctl
                devices.append(path)
        except (PermissionError, OSError):
            continue
    return devices


def get_cpu_temperature() -> float:
    """Read CPU temperature (for RK3566 or similar boards).

    Returns:
        Temperature in Celsius, or -1 if unavailable.
    """
    temp_paths = [
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/hwmon/hwmon0/temp1_input",
    ]
    for p in temp_paths:
        try:
            with open(p) as f:
                val = int(f.read().strip())
                return val / 1000.0 if val > 1000 else float(val)
        except (FileNotFoundError, ValueError):
            continue
    return -1.0
