"""UVC video capture module.

Primary: OpenCV V4L2 backend (battle-tested buffer management).
Fallback: direct Python V4L2 ioctl/mmap (no OpenCV dependency).
Last resort: ffmpeg JPEG snapshot.
"""

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
            return False
        # Set MJPEG and target resolution
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
        # Verify by reading one frame
        ok, _ = cap.read()
        if not ok:
            cap.release()
            return False
        self._cap = cap
        return True

    def _try_v4l2(self) -> bool:
        v4l2 = V4l2Direct(self.device, self.width, self.height)
        if not v4l2.setup():
            return False
        self._v4l2 = v4l2
        return True

    # ── Frame capture ──────────────────────────────────────────────────────
    def grab_frame(self, retries: int = 3) -> Image.Image:
        """Capture a single RGB frame.

        Args:
            retries: Number of retry attempts on failure.

        Returns:
            PIL Image in RGB mode.

        Raises:
            UvcError: If all retries fail.
        """
        self._init_backend()

        if self._backend == "opencv":
            return self._grab_opencv(retries)
        elif self._backend == "v4l2":
            return self._grab_v4l2(retries)
        else:
            return self._grab_ffmpeg(retries)

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
