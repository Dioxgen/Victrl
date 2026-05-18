"""Direct V4L2 frame capture using Python ioctl/mmap — no ffmpeg dependency.

This module bypasses ffmpeg's VIDIOC_G_INPUT ioctl which some capture cards
(MacroSilicon MS2109, etc.) don't implement.
"""

import fcntl
import logging
import mmap
import os
import struct
import ctypes
from io import BytesIO
from typing import Tuple

from PIL import Image

logger = logging.getLogger("victrl.v4l2")

# ── V4L2 constants ─────────────────────────────────────────────────────────
_V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
_V4L2_MEMORY_MMAP = 1
_V4L2_FIELD_NONE = 1
_V4L2_PIX_FMT_MJPEG = 0x47504A4D  # 'MJPG' LE
_V4L2_PIX_FMT_YUYV = 0x56595559   # 'YUYV' LE

# ── ioctl helper (x86_64 Linux) ────────────────────────────────────────────
_IOC_NONE = 0
_IOC_WRITE = 1
_IOC_READ = 2

def _ioc(dir_, type_, nr, size):
    return (dir_ << 30) | (size << 16) | (ord(type_) << 8) | nr

_VIDIOC_QUERYCAP  = _ioc(_IOC_READ, 'V', 0, 104)
_VIDIOC_ENUM_FMT  = _ioc(_IOC_READ | _IOC_WRITE, 'V', 2, 208)
_VIDIOC_S_FMT     = _ioc(_IOC_READ | _IOC_WRITE, 'V', 5, 208)
_VIDIOC_G_FMT     = _ioc(_IOC_READ | _IOC_WRITE, 'V', 4, 208)
_VIDIOC_REQBUFS   = _ioc(_IOC_READ | _IOC_WRITE, 'V', 8, 20)
_VIDIOC_QUERYBUF  = _ioc(_IOC_READ | _IOC_WRITE, 'V', 9, 88)
_VIDIOC_QBUF      = _ioc(_IOC_READ | _IOC_WRITE, 'V', 15, 88)
_VIDIOC_DQBUF     = _ioc(_IOC_READ | _IOC_WRITE, 'V', 17, 88)
_VIDIOC_STREAMON  = _ioc(_IOC_WRITE, 'V', 18, 4)
_VIDIOC_STREAMOFF = _ioc(_IOC_WRITE, 'V', 19, 4)

# ── struct formats (x86_64: LP64) ──────────────────────────────────────────
# struct v4l2_capability: 16*8 + 32 + 32 + 32 + 4*32 = 128+32+32+32+128=352...
# Actually let's be precise. Linux kernel definition:
# __u8 driver[16], card[32], bus_info[32]; __u32 version; __u32 capabilities; __u32 device_caps; __u32 reserved[3];
# = 16 + 32 + 32 + 4 + 4 + 4 + 12 = 104 bytes
_FMT_CAP = "16s32s32sIII3I"  # 104 bytes

# struct v4l2_format: __u32 type; union { struct v4l2_pix_format pix; ... }
# struct v4l2_pix_format: width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv, flags, ycbcr_enc, hsv_enc, quantization, xfer_func
# __u32 width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv; __u32 flags; __u32 ycbcr_enc; __u32 hsv_enc; __u32 quantization; __u32 xfer_func
# = 13 * 4 = 52 bytes for pix + 4 bytes type = 56... no wait
# The union is the largest member. pix is 52 bytes. type is 4 bytes, then padding to align union.
# Actually format is: type (4) + padding (4) + pix (52) = 60?
# Let me check: sizeof(struct v4l2_format) on x86_64 = 208 (from kernel headers, it's padded to include both pix and other format types)
# The biggest union member is probably v4l2_sdr_format or v4l2_meta_format.
# Let me use a generous approach: just pack type + pix fields directly

# struct v4l2_pix_format (packed for ioctl):
_FMT_PIX = "8I4xIIIII"  # width,height,pixelformat,field,bytesperline,sizeimage,colorspace,priv(8) + 4 pad + flags,ycbcr_enc,hsv_enc,quantization,xfer_func(5)

# struct v4l2_requestbuffers: count, type, memory, reserved[2]
_FMT_REQBUFS = "IIII4x"  # 20 bytes

# struct v4l2_buffer: index, type, bytesused, flags, field, memory, m.offset, length, ...timestamp..., ...
# Actually this is complex. Let me define it precisely for 64-bit:
# __u32 index, type, bytesused, flags, field; struct timeval timestamp; struct v4l2_timecode timecode; __u32 sequence, memory; union { __u32 offset; unsigned long userptr; }; __u32 length; __u32 reserved2; __u32 reserved;
# timeval: 8+8=16, timecode: 4*4=16
# Total: 5*4 + 16 + 16 + 2*4 + 8 + 4 + 4 + 4 = 20+16+16+8+8+12 = 80?
# Let me use 88 which is known for 64-bit
_FMT_BUF = "5I4x4I4x4IQQ4I"  # Hmm this is getting complex

# Let me simplify: just use binary buffers of the right size
_SZ_CAP = 104
_SZ_FMT = 208
_SZ_REQBUFS = 20
_SZ_BUF = 88

# ── YUYV→RGB conversion (fast approximate, no cv2 needed) ─────────────────
def _yuyv_to_rgb(yuyv_data: bytes, width: int, height: int) -> Image.Image:
    """Convert YUYV422 raw data to RGB PIL Image."""
    img = Image.frombytes("YCbCr", (width, height), yuyv_data, "raw", "YUYV")
    return img.convert("RGB")


class V4l2Direct:
    """Minimal V4L2 capture using only Python stdlib ioctl/mmap."""

    def __init__(self, device: str = "/dev/video1", width: int = 1920, height: int = 1080):
        self.device = device
        self.width = width
        self.height = height
        self._fd = -1
        self._buffers: list[tuple[int, mmap.mmap]] = []
        self._streaming = False
        self._pixelformat = _V4L2_PIX_FMT_MJPEG

    # ── low-level ioctl helpers ────────────────────────────────────────────
    def _ioctl(self, request, arg=0):
        return fcntl.ioctl(self._fd, request, arg)

    def _ioctl_read(self, request, size):
        buf = bytes(size)
        result = fcntl.ioctl(self._fd, request, buf)
        return bytes(result)  # py3 returns bytes

    def _ioctl_write(self, request, data):
        return fcntl.ioctl(self._fd, request, data)

    # ── open / close ───────────────────────────────────────────────────────
    def open(self) -> bool:
        """Open the V4L2 device. Returns True on success."""
        try:
            self._fd = os.open(self.device, os.O_RDWR | os.O_NONBLOCK)
        except OSError as e:
            logger.error(f"Cannot open {self.device}: {e}")
            return False

        # Check capabilities
        try:
            cap = self._ioctl_read(_VIDIOC_QUERYCAP, _SZ_CAP)
            driver, card, bus_info, version, capabilities, device_caps, r0, r1, r2 = \
                struct.unpack(_FMT_CAP, cap)
            drv_str = driver.rstrip(b'\x00').decode(errors='replace')
            card_str = card.rstrip(b'\x00').decode(errors='replace')
            has_streaming = bool(device_caps & 0x00004000)  # V4L2_CAP_STREAMING
            logger.info(f"V4L2 device: {drv_str} / {card_str}, streaming={has_streaming}")
            if not has_streaming:
                logger.warning("Device lacks streaming capability — capture may fail")
        except OSError as e:
            logger.warning(f"VIDIOC_QUERYCAP failed: {e}")

        return True

    def close(self):
        """Close device and free buffers."""
        self._stop_streaming()
        self._free_buffers()
        if self._fd >= 0:
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = -1

    # ── format negotiation ─────────────────────────────────────────────────
    def _set_format(self) -> bool:
        """Try to set MJPEG format, falling back to YUYV."""
        # Try MJPEG first
        for pixfmt in (_V4L2_PIX_FMT_MJPEG, _V4L2_PIX_FMT_YUYV):
            try:
                # Build v4l2_format as 208-byte buffer
                buf = bytearray(_SZ_FMT)
                # type at offset 0
                struct.pack_into("I", buf, 0, _V4L2_BUF_TYPE_VIDEO_CAPTURE)
                # pix.width, pix.height at offsets 4, 8
                struct.pack_into("II", buf, 4, self.width, self.height)
                # pix.pixelformat at offset 12
                struct.pack_into("I", buf, 12, pixfmt)
                # pix.field at offset 16
                struct.pack_into("I", buf, 16, _V4L2_FIELD_NONE)

                result = self._ioctl_write(_VIDIOC_S_FMT, bytes(buf))
                result = bytes(result)

                # Parse result
                rtype = struct.unpack_from("I", result, 0)[0]
                rwidth = struct.unpack_from("I", result, 4)[0]
                rheight = struct.unpack_from("I", result, 8)[0]
                rpixfmt = struct.unpack_from("I", result, 12)[0]
                rsize = struct.unpack_from("I", result, 20)[0]  # sizeimage

                fmt_name = struct.pack("I", rpixfmt).decode(errors='replace')
                logger.info(f"Format set: {rwidth}x{rheight} {fmt_name}, sizeimage={rsize}")

                if rwidth == 0 or rheight == 0:
                    logger.error(f"Device returned zero resolution — no HDMI signal?")
                    return False

                self._pixelformat = rpixfmt
                self.width = rwidth
                self.height = rheight
                self._sizeimage = rsize
                return True

            except OSError as e:
                logger.warning(f"VIDIOC_S_FMT failed for {pixfmt:#x}: {e}")

        return False

    # ── buffer management ──────────────────────────────────────────────────
    def _request_buffers(self, count: int = 4) -> bool:
        buf = struct.pack(_FMT_REQBUFS, count, _V4L2_BUF_TYPE_VIDEO_CAPTURE, _V4L2_MEMORY_MMAP, 0)
        try:
            result = self._ioctl_write(_VIDIOC_REQBUFS, buf)
            nbufs = struct.unpack("I", bytes(result)[:4])[0]
            logger.info(f"Requested {count} buffers, got {nbufs}")
            return nbufs > 0
        except OSError as e:
            logger.error(f"VIDIOC_REQBUFS failed: {e}")
            return False

    def _map_buffers(self) -> bool:
        for i in range(4):  # we requested 4
            try:
                # Build v4l2_buffer query
                qbuf = bytearray(_SZ_BUF)
                struct.pack_into("I", qbuf, 0, i)       # index
                struct.pack_into("I", qbuf, 4, _V4L2_BUF_TYPE_VIDEO_CAPTURE)  # type
                struct.pack_into("I", qbuf, 16, _V4L2_MEMORY_MMAP)  # memory

                result = self._ioctl_write(_VIDIOC_QUERYBUF, bytes(qbuf))
                result = bytes(result)

                # m.offset is at offset 40 (after: index,type,bytesused,flags,field + timestamp(16) + timecode(16) + sequence,memory)
                # Let me compute: index(4) + type(4) + bytesused(4) + flags(4) + field(4) = 20
                # timestamp: tv_sec(8) + tv_usec(8) = 16 → offset 20
                # timecode: 4*4 = 16 → offset 36
                # sequence(4) + memory(4) = 8 → offset 52
                # m.offset: 4 bytes at offset 52... wait, on 64-bit, union m includes unsigned long which is 8 bytes
                # So m.offset is at offset 52 but needs 8 bytes for userptr
                # Let me re-examine:
                # struct v4l2_buffer on 64-bit:
                # index(I), type(I), bytesused(I), flags(I), field(I) = 20
                # struct timeval { long tv_sec(8), long tv_usec(8) } = 16 → offset 20
                # struct v4l2_timecode { I,I,I,I } = 16 → offset 36
                # sequence(I), memory(I) = 8 → offset 52
                # union { __u32 offset; unsigned long userptr; __u32 planes; int fd; } = 8 → offset 60
                # length(I) = 4 → offset 68
                # reserved2(I), reserved(I) = 8 → offset 72
                # Total: 80? But known value is 88, so there must be padding
                # Let me use known offsets: m.offset is at 0x38 = 56, length at 0x40 = 64

                offset = struct.unpack_from("I", result, 56)[0]  # m.offset (lower 32 bits)
                length = struct.unpack_from("I", result, 64)[0]

                if length == 0:
                    logger.warning(f"Buffer {i}: zero length, skipping")
                    continue

                mm = mmap.mmap(self._fd, length, offset=offset, prot=mmap.PROT_READ | mmap.PROT_WRITE)
                self._buffers.append((i, mm))
                logger.debug(f"Buffer {i}: offset={offset}, length={length}")

            except OSError as e:
                logger.warning(f"Buffer {i} map failed: {e}")
                break

        return len(self._buffers) > 0

    def _queue_all_buffers(self):
        for idx, _ in self._buffers:
            qbuf = bytearray(_SZ_BUF)
            struct.pack_into("I", qbuf, 0, idx)
            struct.pack_into("I", qbuf, 4, _V4L2_BUF_TYPE_VIDEO_CAPTURE)
            struct.pack_into("I", qbuf, 16, _V4L2_MEMORY_MMAP)
            try:
                self._ioctl_write(_VIDIOC_QBUF, bytes(qbuf))
            except OSError as e:
                logger.warning(f"QBUF {idx} failed: {e}")

    def _start_streaming(self):
        buf = struct.pack("I", _V4L2_BUF_TYPE_VIDEO_CAPTURE)
        self._ioctl_write(_VIDIOC_STREAMON, buf)
        self._streaming = True

    def _stop_streaming(self):
        if self._streaming and self._fd >= 0:
            try:
                buf = struct.pack("I", _V4L2_BUF_TYPE_VIDEO_CAPTURE)
                self._ioctl_write(_VIDIOC_STREAMOFF, buf)
            except OSError:
                pass
            self._streaming = False

    def _free_buffers(self):
        for _, mm in self._buffers:
            try:
                mm.close()
            except Exception:
                pass
        self._buffers.clear()

    # ── public API ─────────────────────────────────────────────────────────
    def setup(self) -> bool:
        """Initialize device, set format, allocate buffers. Returns True on success."""
        if not self.open():
            return False
        if not self._set_format():
            return False
        if not self._request_buffers(4):
            return False
        if not self._map_buffers():
            return False
        self._queue_all_buffers()
        self._start_streaming()
        return True

    def grab_frame(self, timeout_ms: int = 3000) -> bytes | None:
        """Dequeue one frame buffer and return its raw data.

        Returns raw bytes (MJPEG or YUYV depending on format), or None on timeout/error.
        """
        if not self._streaming:
            return None

        # Poll the fd for data
        import select
        try:
            ready, _, _ = select.select([self._fd], [], [], timeout_ms / 1000.0)
            if not ready:
                logger.warning("Frame timeout — no data from device")
                return None
        except OSError:
            return None

        # Dequeue buffer
        dqbuf = bytearray(_SZ_BUF)
        struct.pack_into("I", dqbuf, 4, _V4L2_BUF_TYPE_VIDEO_CAPTURE)
        struct.pack_into("I", dqbuf, 16, _V4L2_MEMORY_MMAP)

        try:
            result = self._ioctl_write(_VIDIOC_DQBUF, bytes(dqbuf))
            result = bytes(result)
        except OSError as e:
            logger.warning(f"DQBUF failed: {e}")
            return None

        idx = struct.unpack_from("I", result, 0)[0]
        bytesused = struct.unpack_from("I", result, 8)[0]
        flags = struct.unpack_from("I", result, 12)[0]

        if bytesused == 0:
            self._queue_buffer(idx)
            return None

        # Read from mmap buffer
        for bi, mm in self._buffers:
            if bi == idx:
                mm.seek(0)
                data = mm.read(bytesused)
                # Re-queue buffer
                self._queue_buffer(idx)
                return data

        self._queue_buffer(idx)
        return None

    def _queue_buffer(self, idx: int):
        qbuf = bytearray(_SZ_BUF)
        struct.pack_into("I", qbuf, 0, idx)
        struct.pack_into("I", qbuf, 4, _V4L2_BUF_TYPE_VIDEO_CAPTURE)
        struct.pack_into("I", qbuf, 16, _V4L2_MEMORY_MMAP)
        try:
            self._ioctl_write(_VIDIOC_QBUF, bytes(qbuf))
        except OSError as e:
            logger.warning(f"Re-QBUF {idx} failed: {e}")

    # ── diagnostic ─────────────────────────────────────────────────────────
    @staticmethod
    def diagnose(device: str = "/dev/video1") -> list[str]:
        """Return a list of diagnostic strings for the device."""
        lines = [f"V4L2 diagnostic for {device}:"]
        try:
            fd = os.open(device, os.O_RDWR | os.O_NONBLOCK)
        except OSError as e:
            lines.append(f"  Cannot open device: {e}")
            return lines

        has_video_cap = False
        has_streaming = False
        try:
            cap = fcntl.ioctl(fd, _VIDIOC_QUERYCAP, bytes(_SZ_CAP))
            cap = bytes(cap)
            driver, card, bus_info, version, capabilities, device_caps, *_ = \
                struct.unpack(_FMT_CAP, cap)
            drv = driver.rstrip(b'\x00').decode(errors='replace')
            crd = card.rstrip(b'\x00').decode(errors='replace')
            bus = bus_info.rstrip(b'\x00').decode(errors='replace')
            has_video_cap = bool(device_caps & 0x00000001)
            has_streaming = bool(device_caps & 0x00004000)
            lines.append(f"  Driver: {drv}")
            lines.append(f"  Card:   {crd}")
            lines.append(f"  Bus:    {bus}")
            lines.append(f"  Video capture: {has_video_cap}, Streaming: {has_streaming}")

            # Enumerate formats
            fmt_count = 0
            for i in range(8):
                fmtbuf = bytearray(_SZ_FMT)
                struct.pack_into("I", fmtbuf, 0, i)
                struct.pack_into("I", fmtbuf, 4, _V4L2_BUF_TYPE_VIDEO_CAPTURE)
                try:
                    result = fcntl.ioctl(fd, _VIDIOC_ENUM_FMT, bytes(fmtbuf))
                    result = bytes(result)
                    pixfmt = struct.unpack_from("I", result, 8)[0]
                    if pixfmt == 0:
                        break
                    fmt_str = struct.pack("I", pixfmt).decode(errors='replace')
                    desc = result[12:44].rstrip(b'\x00').decode(errors='replace')
                    lines.append(f"  Format [{i}]: {fmt_str} — {desc}")
                    fmt_count += 1
                except OSError:
                    break

            if fmt_count == 0 and has_video_cap:
                lines.append("  No video formats enumerated — HDMI signal may be absent")
            elif fmt_count == 0 and not has_video_cap:
                lines.append("  Device lacks video-capture capability (metadata-only mode)")
                lines.append("  This usually means the capture card is not receiving HDMI input.")
                lines.append("  Check: (1) HDMI cable connected, (2) source device powered on,")
                lines.append("         (3) source is outputting a supported resolution.")
        finally:
            os.close(fd)

        return lines
