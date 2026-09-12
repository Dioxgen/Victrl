"""UVC video capture module.

Primary: OpenCV V4L2 backend (battle-tested buffer management).
Fallback: direct Python V4L2 ioctl/mmap (no OpenCV dependency).
Last resort: ffmpeg JPEG snapshot.
"""

import glob
import hashlib
import logging
import subprocess
import time
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image

from utils.exceptions import UvcError

logger = logging.getLogger("victrl.uvc")

_FRAME_TMP = Path("/tmp/victrl_frame.jpg")

# Number of frames to discard after opening the device (MS2109 warmup)
_WARMUP_FRAMES = 8

# ── Optional OpenCV import ─────────────────────────────────────────────────
_CV2 = None
try:
    import cv2
    _CV2 = cv2
except ImportError:
    pass

# ── Optional direct V4L2 import ────────────────────────────────────────────
_V4L2 = None
try:
    from core.v4l2_direct import V4l2Direct, _yuyv_to_rgb
    _V4L2 = V4l2Direct
    _YUYV_TO_RGB = _yuyv_to_rgb
except ImportError:
    pass


class UvcCapture:
    """Capture frames from a UVC device.

    Priority: OpenCV > direct V4L2 > ffmpeg.
    """

    def __init__(self, device: str = "/dev/video0", width: int = 1920, height: int = 1080):
        self.device = device
        self.width = width
        self.height = height
        self._actual_width = width
        self._actual_height = height
        self._cap: "cv2.VideoCapture | None" = None
        self._v4l2 = None
        self._backend = None  # "opencv" | "v4l2" | "ffmpeg"
        logger.info(f"UvcCapture initialized: device={device}, target={width}x{height}")

    # ── Backend selection ──────────────────────────────────────────────────
    def _init_backend(self) -> bool:
        """Auto-select the best available capture backend. Returns True on success."""
        if self._backend is not None:
            return self._backend is not None

        if _CV2 is not None:
            if self._try_opencv():
                self._backend = "opencv"
                logger.info("Using OpenCV capture backend")
                return True
            logger.warning("OpenCV backend failed to initialize, trying direct V4L2")

        if _V4L2 is not None:
            if self._try_v4l2():
                self._backend = "v4l2"
                logger.info("Using direct V4L2 capture backend")
                return True
            logger.warning("Direct V4L2 backend failed, trying ffmpeg")

        self._backend = "ffmpeg"
        logger.info("Using ffmpeg capture backend")
        return True

    def _try_opencv(self) -> bool:
        cap = cv2.VideoCapture(self.device)
        if not cap.isOpened():
            logger.warning(f"OpenCV: failed to open {self.device}")
            return False
        # Set MJPEG and target resolution
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)

        # Warmup: discard first N frames — MS2109 needs several frames
        # after stream start before the video pipeline stabilizes
        warmup_ok = False
        for i in range(_WARMUP_FRAMES):
            ok, frame = cap.read()
            if ok and frame is not None:
                warmup_ok = True
                logger.debug(f"OpenCV warmup frame {i+1}/{_WARMUP_FRAMES}: "
                            f"shape={frame.shape}, mean={frame.mean():.1f}, "
                            f"std={frame.std():.1f}")
            else:
                logger.warning(f"OpenCV warmup frame {i+1}/{_WARMUP_FRAMES}: FAILED (ok={ok})")
        if not warmup_ok:
            logger.error(f"OpenCV: all {_WARMUP_FRAMES} warmup frames failed")
            cap.release()
            return False
        self._cap = cap
        self._last_mean = None
        self._frame_count = 0
        logger.info(f"OpenCV backend ready: {self.device} @ {self.width}x{self.height} MJPEG "
                    f"({_WARMUP_FRAMES} warmup frames discarded)")
        return True

    def _try_v4l2(self) -> bool:
        v4l2 = V4l2Direct(self.device, self.width, self.height)
        if not v4l2.setup():
            return False
        self._v4l2 = v4l2
        return True

    # ── Frame capture ──────────────────────────────────────────────────────
    @staticmethod
    def _wake_ms2109() -> None:
        """Disable autosuspend on MacroSilicon MS2109 devices.

        These devices suspend after 2s of idle and resume with a color-bar
        test pattern instead of live video. Writing 'on' to power/control
        prevents the suspend entirely.

        Only writes if the current value is not already 'on', to avoid
        unnecessary sysfs writes that might disrupt the video stream.
        """
        for path in glob.glob("/sys/devices/*/usb[0-9]*/[0-9]*-[0-9]*/idVendor"):
            try:
                with open(path) as f:
                    vendor = f.read().strip()
                if vendor != "534d":
                    continue
                prod_path = path.replace("idVendor", "idProduct")
                with open(prod_path) as f:
                    product = f.read().strip()
                if product != "2109":
                    continue
                ctrl_path = path.replace("idVendor", "power/control")
                with open(ctrl_path) as f:
                    current = f.read().strip()
                if current != "on":
                    with open(ctrl_path, "w") as f:
                        f.write("on")
                    logger.info(f"MS2109 power/control: '{current}' -> 'on'")
                return
            except (OSError, PermissionError):
                pass

    def grab_frame(self, retries: int = 3) -> Image.Image:
        """Capture a single RGB frame.

        Re-initializes the VideoCapture on every call to guarantee a fresh
        stream. The MS2109 stops delivering new frames after a few seconds
        of idle (e.g. during 30s API calls), so re-using a persistent
        VideoCapture returns stale buffers. Closing and re-opening forces
        the UVC driver to restart the isochronous transfer.

        Args:
            retries: Number of retry attempts on failure.

        Returns:
            PIL Image in RGB mode.

        Raises:
            UvcError: If all retries fail.
        """
        t0 = time.perf_counter()
        self._wake_ms2109()

        # Release previous instance — MS2109 stops streaming during long
        # idle gaps; a fresh open guarantees current frames.
        if self._cap is not None:
            self._cap.release()
            self._cap = None
            logger.debug("Released previous OpenCV capture")

        # Force re-initialization of the backend
        saved_backend = self._backend
        self._backend = None
        if not self._init_backend():
            self._backend = saved_backend  # restore on failure
            raise UvcError("Failed to re-initialize capture backend")

        if self._backend == "opencv":
            img = self._grab_opencv(retries)
        elif self._backend == "v4l2":
            img = self._grab_v4l2(retries)
        else:
            img = self._grab_ffmpeg(retries)

        elapsed_ms = (time.perf_counter() - t0) * 1000
        logger.debug(f"grab_frame total: {elapsed_ms:.0f}ms")
        return img

    def _grab_opencv(self, retries: int) -> Image.Image:
        last_error = None
        for attempt in range(retries):
            try:
                ok, frame = self._cap.read()
                if not ok or frame is None:
                    last_error = "OpenCV read returned no frame"
                    logger.warning(f"Capture attempt {attempt + 1}/{retries}: {last_error}")
                    time.sleep(0.3)
                    continue

                self._frame_count += 1
                frame_mean = float(frame.mean())
                frame_std = float(frame.std())
                frame_hash = hashlib.md5(frame.tobytes()).hexdigest()[:8]

                # Compare with previous frame
                delta_str = ""
                if self._last_mean is not None:
                    delta = abs(frame_mean - self._last_mean)
                    delta_str = f", Δmean={delta:.2f}"
                self._last_mean = frame_mean

                logger.info(
                    f"Captured frame #{self._frame_count}: "
                    f"shape={frame.shape}, mean={frame_mean:.1f}, std={frame_std:.1f}, "
                    f"hash={frame_hash}{delta_str}"
                )

                # frame is BGR numpy array, convert to PIL RGB
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                img = Image.fromarray(rgb)
                w, h = img.size
                self._actual_width = w
                self._actual_height = h

                if (w, h) != (self.width, self.height):
                    img = img.resize((self.width, self.height), Image.BILINEAR)

                return img

            except Exception as e:
                last_error = str(e)
                logger.warning(f"Capture attempt {attempt + 1}/{retries}: {e}")
                time.sleep(0.3)

        raise UvcError(f"Failed to capture frame after {retries} attempts: {last_error}")

    def _grab_v4l2(self, retries: int) -> Image.Image:
        last_error = None
        for attempt in range(retries):
            try:
                raw = self._v4l2.grab_frame()
                if raw is None:
                    last_error = "V4L2 returned no data"
                    logger.warning(f"Capture attempt {attempt + 1}/{retries}: {last_error}")
                    time.sleep(0.3)
                    continue

                if self._v4l2._pixelformat == 0x47504A4D:  # MJPEG
                    img = Image.open(BytesIO(raw))
                    img.load()
                else:  # YUYV
                    img = _YUYV_TO_RGB(raw, self.width, self.height)

                w, h = img.size
                self._actual_width = w
                self._actual_height = h

                if (w, h) != (self.width, self.height):
                    img = img.resize((self.width, self.height), Image.BILINEAR)

                return img.convert("RGB")

            except Exception as e:
                last_error = str(e)
                logger.warning(f"Capture attempt {attempt + 1}/{retries}: {e}")
                time.sleep(0.4)

        raise UvcError(f"Failed to capture frame after {retries} attempts: {last_error}")

    def _grab_ffmpeg(self, retries: int) -> Image.Image:
        last_error = None
        for attempt in range(retries):
            try:
                _FRAME_TMP.unlink(missing_ok=True)

                cmd = [
                    "ffmpeg", "-y",
                    "-f", "v4l2",
                    "-i", self.device,
                    "-vframes", "1",
                    str(_FRAME_TMP),
                ]

                proc = subprocess.run(cmd, capture_output=True, timeout=8)

                if proc.returncode != 0:
                    stderr = proc.stderr.decode(errors="replace")[-300:]
                    raise UvcError(f"ffmpeg rc={proc.returncode}: {stderr}")

                if not _FRAME_TMP.exists() or _FRAME_TMP.stat().st_size == 0:
                    raise UvcError("No frame data — is HDMI signal connected?")

                img = Image.open(_FRAME_TMP)
                img.load()
                w, h = img.size
                self._actual_width = w
                self._actual_height = h

                if (w, h) != (self.width, self.height):
                    img = img.resize((self.width, self.height), Image.BILINEAR)

                return img.convert("RGB")

            except UvcError as e:
                last_error = str(e)
                logger.warning(f"Capture attempt {attempt + 1}/{retries}: {last_error}")
            except Exception as e:
                last_error = str(e)
                logger.warning(f"Capture attempt {attempt + 1}/{retries}: {e}")
            time.sleep(0.6)

        raise UvcError(f"Failed to capture frame after {retries} attempts: {last_error}")

    # ── Helpers ────────────────────────────────────────────────────────────
    def get_actual_resolution(self) -> tuple:
        """Return the actual native resolution of the last captured frame."""
        return self._actual_width, self._actual_height

    def diagnose(self) -> str:
        """Return diagnostic info for the capture device."""
        lines = []
        if _CV2 is not None:
            lines.append("OpenCV: available")
        else:
            lines.append("OpenCV: not installed")
        if _V4L2 is not None:
            try:
                lines.extend(V4l2Direct.diagnose(self.device))
            except Exception as e:
                lines.append(f"V4L2 diagnostic error: {e}")
        return "\n".join(lines)

    def release(self) -> None:
        """Clean up resources."""
        if self._cap is not None:
            try:
                self._cap.release()
            except Exception:
                pass
            self._cap = None
        if self._v4l2 is not None:
            self._v4l2.close()
            self._v4l2 = None
        _FRAME_TMP.unlink(missing_ok=True)
