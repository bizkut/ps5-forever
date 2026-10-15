#!/usr/bin/env python3
"""Request, download, and convert a PS5 RADV VideoOut frame capture."""

from __future__ import annotations

import argparse
import os
import posixpath
import re
import struct
import time
from dataclasses import dataclass
from ftplib import FTP, error_perm, error_reply
from io import BytesIO
from pathlib import Path

from PIL import Image

HOST = os.environ.get("PS5_HOST", "10.0.1.41")
FTP_PORT = int(os.environ.get("FTP_PORT", "2121"))
REMOTE_DIR = "/data/homebrew/PPSA99030/wine"
REQUEST_PATH = f"{REMOTE_DIR}/radv-capture.req"
CAPTURE_RE = re.compile(r"^radv-capture-(\d+)\.bin$")
MAGIC = b"PS5CAP1\0"
HEADER = struct.Struct("<8s6I3Q")

VK_FORMAT_B8G8R8A8_UNORM = 44
VK_FORMAT_B8G8R8A8_SRGB = 50
VK_FORMAT_A2R10G10B10_UNORM_PACK32 = 58
VK_FORMAT_A2B10G10R10_UNORM_PACK32 = 64
VK_COLOR_SPACE_HDR10_ST2084_EXT = 1000104008


@dataclass(frozen=True)
class CaptureHeader:
    width: int
    height: int
    source_width: int
    source_height: int
    format: int
    color_space: int
    pixel_format: int
    sequence: int
    timestamp_ns: int


def parse_capture(data: bytes) -> tuple[CaptureHeader, memoryview]:
    if len(data) < HEADER.size:
        raise ValueError(f"capture is shorter than its {HEADER.size}-byte header")
    fields = HEADER.unpack_from(data)
    if fields[0] != MAGIC:
        raise ValueError(f"bad capture magic {fields[0]!r}")
    header = CaptureHeader(*fields[1:])
    if not (header.width and header.height and header.source_width and header.source_height):
        raise ValueError("capture dimensions must be nonzero")
    if header.width > header.source_width or header.height > header.source_height:
        raise ValueError("capture dimensions exceed source dimensions")
    expected = HEADER.size + header.width * header.height * 4
    if expected != len(data):
        raise ValueError(f"capture has {len(data)} bytes, expected {expected}")
    return header, memoryview(data)[HEADER.size:]


def unpack_pixel(word: int, vk_format: int) -> tuple[int, int, int, int, int]:
    """Return integer R, G, B, A and the component maximum."""
    if vk_format in (VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB):
        return (word >> 16) & 0xFF, (word >> 8) & 0xFF, word & 0xFF, (word >> 24) & 0xFF, 255
    if vk_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return word & 0x3FF, (word >> 10) & 0x3FF, (word >> 20) & 0x3FF, (word >> 30) & 3, 1023
    if vk_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return (word >> 20) & 0x3FF, (word >> 10) & 0x3FF, word & 0x3FF, (word >> 30) & 3, 1023
    raise ValueError(f"unsupported VkFormat {vk_format}")


def _srgb_to_linear(value: float) -> float:
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def _linear_to_srgb_byte(value: float) -> int:
    value = min(1.0, max(0.0, value))
    encoded = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return min(255, max(0, round(encoded * 255.0)))


def _pq_nits(code: int) -> float:
    # SMPTE ST 2084, normalized code value to absolute luminance in nits.
    m1 = 2610.0 / 16384.0
    m2 = 2523.0 / 32.0
    c1 = 3424.0 / 4096.0
    c2 = 2413.0 / 128.0
    c3 = 2392.0 / 128.0
    value = code / 1023.0
    power = value ** (1.0 / m2)
    numerator = max(power - c1, 0.0)
    denominator = c2 - c3 * power
    return 10000.0 * (numerator / denominator) ** (1.0 / m1) if denominator > 0.0 else 0.0


def convert_capture(data: bytes) -> tuple[CaptureHeader, Image.Image, Image.Image | None, dict[str, object]]:
    header, payload = parse_capture(data)
    hdr = header.color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT
    if hdr and header.format not in (
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
    ):
        raise ValueError(f"HDR capture uses unsupported VkFormat {header.format}")

    count = header.width * header.height
    rgb = bytearray(count * 3)
    raw_rgb = bytearray(count * 3) if hdr else None
    sums = [0.0, 0.0, 0.0]
    maximum = 0.0
    pq_lut = [_pq_nits(code) for code in range(1024)] if hdr else None

    for index, (word,) in enumerate(struct.iter_unpack("<I", payload)):
        red, green, blue, _alpha, component_max = unpack_pixel(word, header.format)
        if hdr:
            assert pq_lut is not None and raw_rgb is not None
            r_nits, g_nits, b_nits = pq_lut[red], pq_lut[green], pq_lut[blue]
            raw_rgb[index * 3:index * 3 + 3] = bytes((
                round(red * 255 / component_max),
                round(green * 255 / component_max),
                round(blue * 255 / component_max),
            ))
            # Linear BT.2020 to linear BT.709, then clip at 203-nit reference white.
            r709 = 1.660491 * r_nits - 0.587641 * g_nits - 0.072850 * b_nits
            g709 = -0.124550 * r_nits + 1.132900 * g_nits - 0.008349 * b_nits
            b709 = -0.018151 * r_nits - 0.100579 * g_nits + 1.118730 * b_nits
            rgb[index * 3:index * 3 + 3] = bytes((
                _linear_to_srgb_byte(r709 / 203.0),
                _linear_to_srgb_byte(g709 / 203.0),
                _linear_to_srgb_byte(b709 / 203.0),
            ))
        else:
            rgb[index * 3:index * 3 + 3] = bytes((red, green, blue))
            r_nits = _srgb_to_linear(red / component_max) * 203.0
            g_nits = _srgb_to_linear(green / component_max) * 203.0
            b_nits = _srgb_to_linear(blue / component_max) * 203.0
        sums[0] += r_nits
        sums[1] += g_nits
        sums[2] += b_nits
        maximum = max(maximum, r_nits, g_nits, b_nits)

    image = Image.frombytes("RGB", (header.width, header.height), bytes(rgb))
    raw_image = Image.frombytes("RGB", (header.width, header.height), bytes(raw_rgb)) if raw_rgb else None
    summary = {
        "mean_nits": tuple(value / count for value in sums),
        "max_nits": maximum,
        "hdr": hdr,
    }
    return header, image, raw_image, summary


def _delete(ftp: FTP, path: str) -> None:
    try:
        ftp.delete(path)
    except error_reply as error:
        if not str(error).startswith("226"):
            raise
    except error_perm as error:
        if not str(error).startswith("550"):
            raise


def _capture_paths(ftp: FTP) -> dict[int, str]:
    lines: list[str] = []
    try:
        # The console's ftpsrv rejects NLST (502); LIST is what deploy-wine.py uses.
        ftp.retrlines(f"LIST {REMOTE_DIR}", lines.append)
    except error_perm as error:
        if str(error).startswith("550"):
            return {}
        raise
    captures: dict[int, str] = {}
    for line in lines:
        base = posixpath.basename(line.split(None, 8)[-1].rstrip("/"))
        match = CAPTURE_RE.fullmatch(base)
        if match:
            captures[int(match.group(1))] = f"{REMOTE_DIR}/{base}"
    return captures


def request_capture(scale: int, timeout: float = 15.0, host: str = HOST, port: int = FTP_PORT) -> bytes:
    ftp = FTP()
    ftp.connect(host, port, timeout=max(30.0, timeout + 5.0))
    ftp.login()
    try:
        # Capture files are disposable and sequence numbers restart with the title.
        for path in _capture_paths(ftp).values():
            _delete(ftp, path)
        _delete(ftp, REQUEST_PATH)
        ftp.storbinary(f"STOR {REQUEST_PATH}", BytesIO(f"scale={scale}\n".encode()))

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            captures = _capture_paths(ftp)
            if captures:
                sequence = max(captures)
                path = captures[sequence]
                output = BytesIO()
                ftp.retrbinary(f"RETR {path}", output.write, blocksize=1 << 16)
                data = output.getvalue()
                parse_capture(data)
                _delete(ftp, path)
                return data
            time.sleep(0.25)
        _delete(ftp, REQUEST_PATH)
        raise TimeoutError(f"no RADV capture appeared within {timeout:g} seconds")
    finally:
        try:
            ftp.quit()
        except (OSError, EOFError, error_perm, error_reply):
            ftp.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=int, choices=(2, 4, 8), default=4,
                        help="point-sampling divisor (default: 4, producing 960x540)")
    parser.add_argument("--out", type=Path, required=True, help="destination PNG")
    parser.add_argument("--host", default=HOST, help=f"PS5 FTP host (default: {HOST})")
    parser.add_argument("--port", type=int, default=FTP_PORT, help=f"PS5 FTP port (default: {FTP_PORT})")
    parser.add_argument("--timeout", type=float, default=15.0, help="capture timeout in seconds")
    args = parser.parse_args()

    data = request_capture(args.scale, args.timeout, args.host, args.port)
    header, image, raw_image, summary = convert_capture(data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Keep the exact packed pixels: 8-bit PNGs lose 10-bit precision.
    args.out.with_suffix(".bin").write_bytes(data)
    image.save(args.out, format="PNG")
    raw_path = None
    if raw_image is not None:
        raw_path = args.out.with_name(f"{args.out.stem}-raw.png")
        raw_image.save(raw_path, format="PNG")

    means = summary["mean_nits"]
    assert isinstance(means, tuple)
    print(f"capture seq={header.sequence} {header.width}x{header.height} "
          f"source={header.source_width}x{header.source_height} format={header.format} "
          f"color_space={header.color_space} token=0x{header.pixel_format:016x}")
    print(f"mean_nits R={means[0]:.3f} G={means[1]:.3f} B={means[2]:.3f} "
          f"max={float(summary['max_nits']):.3f}")
    print(f"wrote {args.out}")
    if raw_path:
        print(f"wrote {raw_path} (raw 10-bit codes shown as sRGB)")


if __name__ == "__main__":
    main()
