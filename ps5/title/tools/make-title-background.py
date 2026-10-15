#!/usr/bin/env python3
# The title's home-screen background, sce_sys/pic0.dds, drawn from nothing but
# arithmetic and a fixed random seed, so its origin is this script. ps5/title/
# build.sh installs it as both sce_sys/pic0.dds and pic1.dds.
#
# Adapted from ps5-forever ps5/tools/make-title-background.py (commit 6cf23f2d):
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: MIT
# Changes: writes ps5/title/sce_sys/pic0.dds only; the option that fetched
# Battle.net's artwork is removed (this tree holds no Blizzard artwork).
#
#   python3 ps5/title/tools/make-title-background.py            write the file
#   python3 ps5/title/tools/make-title-background.py --png DIR  also write the picture, and
#                                                               what the DDS decodes to, as PNG
#
# The picture: a dark navy-to-violet gradient, a red-magenta glow rising from
# the bottom centre, and particles scattered through the glow, thinning
# upwards. The files: 3840x2160, DDS with a DX10 header, BC7_UNORM, one level,
# straight alpha, the header PS5_VulkanTemplate's title-packaging.md names as
# shown by the console (flags 0xA1007, depth 1, mip count 1, miscFlags2 1).
# BC7 mode 6 (one subset, RGBA 7.7.7.7 endpoints with a p-bit, 4-bit indices):
# each block's endpoints span its colours along their principal axis, and
# each pixel takes the nearest of the 16 levels. Alpha is 255 throughout.
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT = 3840, 2160
PS5 = Path(__file__).resolve().parent.parent
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int32)


def picture() -> np.ndarray:
    rng = np.random.default_rng(20261002)
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH].astype(np.float32)
    u, v = x / (WIDTH - 1), y / (HEIGHT - 1)
    # The gradient: navy at the top to a deep violet at the bottom
    top, bottom = np.array([0.020, 0.024, 0.055]), np.array([0.075, 0.030, 0.110])
    rgb = top * (1 - v[..., None]) + bottom * v[..., None]
    # The glow: an ellipse at the bottom centre, red at its heart, magenta and
    # violet further out
    d = np.sqrt(((u - 0.5) * 1.6) ** 2 + ((v - 1.08) * 1.15) ** 2)
    heart = np.exp(-(d / 0.28) ** 2)[..., None]
    halo = np.exp(-(d / 0.62) ** 2)[..., None]
    rgb += halo * np.array([0.16, 0.03, 0.24]) + heart * np.array([0.78, 0.15, 0.10])
    # A vignette towards the sides
    rgb *= (1 - 0.55 * np.abs(u - 0.5) ** 1.6)[..., None]

    # The particles, drawn on a layer: most near the glow, fewer higher up
    layer = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    count = 9000
    px = rng.normal(0.5, 0.17, count)
    py = 1 - np.abs(rng.normal(0, 0.42, count))
    keep = (px > 0) & (px < 1) & (py > 0)
    for cx, cy in zip(px[keep], py[keep]):
        r = rng.uniform(1.0, 3.2) * (1.6 if rng.random() < 0.08 else 1.0) * (0.6 + 0.6 * cy)
        warm = np.clip(cy * 1.2 - 0.25 + rng.normal(0, 0.15), 0, 1)
        colour = np.array([0.55, 0.35, 0.95]) * (1 - warm) + np.array([1.0, 0.25, 0.35]) * warm
        alpha = rng.uniform(0.25, 0.85) * (0.35 + 0.65 * cy)
        x0, y0 = cx * WIDTH, cy * HEIGHT
        draw.ellipse([x0 - r, y0 - r, x0 + r, y0 + r],
                     fill=tuple(int(c * 255) for c in colour) + (int(alpha * 255),))
    soft = layer.filter(ImageFilter.GaussianBlur(1.2))
    dots = np.asarray(soft, dtype=np.float32) / 255
    rgb = rgb * (1 - dots[..., 3:]) + dots[..., :3] * dots[..., 3:] + dots[..., :3] * dots[..., 3:] * 0.15
    # A faint grain, so the gradient does not band
    rgb += rng.normal(0, 0.004, rgb.shape)
    return np.clip(np.round(rgb * 255), 0, 255).astype(np.uint8)


def encode_chunk(pixels: np.ndarray):
    """pixels (n, 16, 3), 0..255: mode 6 endpoints (n, 2, 3) as 7-bit values, indices (n, 16)."""
    p = pixels.astype(np.float64)
    mean = p.mean(axis=1, keepdims=True)
    centred = p - mean
    cov = np.einsum("nki,nkj->nij", centred, centred)
    axis = np.ones((p.shape[0], 3)) / np.sqrt(3)
    for _ in range(8):
        axis = np.einsum("nij,nj->ni", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), 1 / np.sqrt(3))
    t = np.einsum("nki,ni->nk", centred, axis)
    lo = mean[:, 0] + t.min(axis=1, keepdims=True) * axis
    hi = mean[:, 0] + t.max(axis=1, keepdims=True) * axis
    ends7 = np.clip(np.round((np.stack([lo, hi], axis=1) - 1) / 2), 0, 127).astype(np.int32)
    ends = ends7 * 2 + 1  # the p-bit, 1
    levels = ((64 - WEIGHTS)[None, :, None] * ends[:, 0:1] + WEIGHTS[None, :, None] * ends[:, 1:2] + 32) >> 6
    error = ((pixels[:, :, None, :].astype(np.int32) - levels[:, None, :, :]) ** 2).sum(axis=3)
    indices = error.argmin(axis=2).astype(np.int32)
    # The anchor (pixel 0) keeps 3 bits: its index must be below 8
    swap = indices[:, 0] >= 8
    ends7[swap] = ends7[swap][:, ::-1]
    indices[swap] = 15 - indices[swap]
    return ends7, indices


def blocks_of(image: np.ndarray) -> np.ndarray:
    by, bx = HEIGHT // 4, WIDTH // 4
    return image.reshape(by, 4, bx, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)


def bc7(image: np.ndarray):
    """The BC7 blocks, and what they decode to (for the preview)."""
    blocks = blocks_of(image)
    out = np.empty(blocks.shape[0] * 2, dtype="<u8")
    decoded = np.empty_like(blocks)
    for start in range(0, blocks.shape[0], 32768):
        ends7, indices = encode_chunk(blocks[start:start + 32768])
        n = ends7.shape[0]
        ends = ends7 * 2 + 1
        w = WEIGHTS[indices][..., None]
        decoded[start:start + n] = ((64 - w) * ends[:, None, 0] + w * ends[:, None, 1] + 32) >> 6
        lo = np.full(n, 1 << 6, dtype=np.uint64)  # mode 6: bit 6
        hi = np.zeros(n, dtype=np.uint64)
        alpha = np.full(n, 127, dtype=np.int32)
        fields = [(f, 7) for f in (ends7[:, 0, 0], ends7[:, 1, 0], ends7[:, 0, 1], ends7[:, 1, 1],
                                    ends7[:, 0, 2], ends7[:, 1, 2], alpha, alpha)]
        fields += [(np.ones(n, np.int32), 1)] * 2 + [(indices[:, 0], 3)]
        fields += [(indices[:, i], 4) for i in range(1, 16)]
        pos = 7
        for value, width in fields:  # no field crosses bit 64 (P0 is bit 63, P1 bit 64)
            value = value.astype(np.uint64)
            if pos < 64:
                lo |= value << np.uint64(pos)
            else:
                hi |= value << np.uint64(pos - 64)
            pos += width
        assert pos == 128
        out[2 * start:2 * (start + n):2], out[2 * start + 1:2 * (start + n):2] = lo, hi
    by, bx = HEIGHT // 4, WIDTH // 4
    preview = decoded.reshape(by, bx, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(HEIGHT, WIDTH, 3)
    return out.tobytes(), preview.astype(np.uint8)


def dds_header() -> bytes:
    """DDS with a DX10 header: BC7_UNORM (98), 2D, one level, straight alpha."""
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, 0xA1007, HEIGHT, WIDTH, WIDTH * HEIGHT, 1, 1)
    header += b"\0" * 44
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 98, 3, 0, 1, 1)
    assert len(header) == 148
    return header



def main() -> int:
    png_dir = None
    if len(sys.argv) == 3 and sys.argv[1] == "--png":
        png_dir = Path(sys.argv[2])
    elif len(sys.argv) != 1:
        print("usage: make-title-background.py [--png DIR]", file=sys.stderr)
        return 2
    image = picture()
    blocks, decoded = bc7(image)
    data = dds_header() + blocks
    (PS5 / "sce_sys" / "pic0.dds").write_bytes(data)
    print(f"sce_sys/pic0.dds: {len(data)} bytes")
    error = image.astype(np.float64) - decoded
    print(f"BC7 against the picture: PSNR {10 * np.log10(255 ** 2 / np.mean(error ** 2)):.1f} dB")
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)
        Image.fromarray(image).save(png_dir / "background.png")
        Image.fromarray(decoded).save(png_dir / "background-bc7.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
