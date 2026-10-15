#!/usr/bin/env python3
"""Synthetic tests for ps5-capture.py's wire format and pixel conversion."""

import importlib.util
import struct
import sys
import unittest
from pathlib import Path

TOOL = Path(__file__).with_name("ps5-capture.py")
SPEC = importlib.util.spec_from_file_location("ps5_capture", TOOL)
CAPTURE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CAPTURE
assert SPEC.loader is not None
SPEC.loader.exec_module(CAPTURE)


def blob(width, height, vk_format, color_space, words, *, sequence=7, token=0x8100070422000000):
    header = CAPTURE.HEADER.pack(
        CAPTURE.MAGIC, width, height, width * 4, height * 4,
        vk_format, color_space, token, sequence, 123456789,
    )
    return header + struct.pack(f"<{len(words)}I", *words)


class CaptureTests(unittest.TestCase):
    def test_header_and_sdr_bgra_decode(self):
        data = blob(1, 1, CAPTURE.VK_FORMAT_B8G8R8A8_UNORM, 0, [0x44332211])
        header, payload = CAPTURE.parse_capture(data)
        self.assertEqual(header.sequence, 7)
        self.assertEqual((header.source_width, header.source_height), (4, 4))
        self.assertEqual(bytes(payload), struct.pack("<I", 0x44332211))
        _, image, raw, summary = CAPTURE.convert_capture(data)
        self.assertIsNone(raw)
        self.assertEqual(image.getpixel((0, 0)), (0x33, 0x22, 0x11))
        self.assertFalse(summary["hdr"])
        self.assertGreater(summary["max_nits"], 0.0)

    def test_hdr_rgb10_codes_and_tone_map(self):
        word = 1023 | (512 << 10) | (0 << 20) | (3 << 30)
        data = blob(1, 1, CAPTURE.VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                    CAPTURE.VK_COLOR_SPACE_HDR10_ST2084_EXT, [word])
        _, image, raw, summary = CAPTURE.convert_capture(data)
        self.assertIsNotNone(raw)
        self.assertEqual(raw.getpixel((0, 0)), (255, 128, 0))
        self.assertEqual(image.size, (1, 1))
        means = summary["mean_nits"]
        self.assertAlmostEqual(means[0], 10000.0, places=3)
        self.assertGreater(means[1], 0.0)
        self.assertEqual(means[2], 0.0)
        self.assertAlmostEqual(summary["max_nits"], 10000.0, places=3)

    def test_hdr_bgr10_packed_order(self):
        word = (1023 << 20) | (17 << 10) | 5 | (3 << 30)
        self.assertEqual(
            CAPTURE.unpack_pixel(word, CAPTURE.VK_FORMAT_A2R10G10B10_UNORM_PACK32),
            (1023, 17, 5, 3, 1023),
        )

    def test_rejects_truncated_and_unsupported(self):
        with self.assertRaisesRegex(ValueError, "shorter"):
            CAPTURE.parse_capture(b"PS5")
        data = blob(1, 1, 999, 0, [0])
        with self.assertRaisesRegex(ValueError, "unsupported VkFormat"):
            CAPTURE.convert_capture(data)
        with self.assertRaisesRegex(ValueError, "expected"):
            CAPTURE.parse_capture(data[:-1])


if __name__ == "__main__":
    unittest.main()
