"""Unit tests for tools/listen_rtp.py. Run with: python -m unittest discover -s tests"""

import importlib.util
import pathlib
import unittest

_TOOL = pathlib.Path(__file__).resolve().parent.parent / "tools" / "listen_rtp.py"
_spec = importlib.util.spec_from_file_location("listen_rtp", _TOOL)
listen_rtp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(listen_rtp)
alaw_to_linear = listen_rtp.alaw_to_linear


class AlawDecodeTest(unittest.TestCase):
    def test_known_values(self):
        # Silence, smallest steps and full scale, matching the C encoder's tests.
        self.assertEqual(alaw_to_linear(0xD5), 8)
        self.assertEqual(alaw_to_linear(0x55), -8)
        self.assertEqual(alaw_to_linear(0xAA), 32256)
        self.assertEqual(alaw_to_linear(0x2A), -32256)
        self.assertEqual(alaw_to_linear(0xC5), 264)  # segment 1, first step
        self.assertEqual(alaw_to_linear(0xF5), 528)  # segment 2, first step

    def test_sign_bit_mirrors(self):
        for code in range(256):
            self.assertEqual(alaw_to_linear(code ^ 0x80), -alaw_to_linear(code), hex(code))

    def test_all_codes_distinct_and_in_range(self):
        values = [alaw_to_linear(code) for code in range(256)]
        self.assertEqual(len(set(values)), 256)
        self.assertTrue(all(-32768 <= v <= 32767 for v in values))

    def test_positive_codes_increase(self):
        # Positive codes in order of magnitude: XOR 0xD5 gives 0x00..0x7F.
        values = [alaw_to_linear(m ^ 0xD5) for m in range(128)]
        self.assertEqual(values, sorted(values))
        self.assertEqual(len(set(values)), 128)


class PayloadDecodeTest(unittest.TestCase):
    def test_l16_is_big_endian_signed(self):
        payload = bytes([0x00, 0x01, 0xFF, 0xFF, 0x80, 0x00, 0x7F, 0xFF])
        self.assertEqual(listen_rtp.decode_payload(96, payload, 48000), [1, -1, -32768, 32767])

    def test_l16_odd_byte_is_dropped(self):
        self.assertEqual(listen_rtp.decode_payload(96, bytes([0x00, 0x02, 0x05])), [2])

    def test_pcma(self):
        self.assertEqual(listen_rtp.decode_payload(8, bytes([0xD5, 0x55])), [8, -8])

    def test_formats(self):
        self.assertEqual(listen_rtp.payload_format(8, 48000), ("PCMA", 8000))
        self.assertEqual(listen_rtp.payload_format(96, 16000), ("L16", 16000))
        self.assertEqual(listen_rtp.payload_format(11, 48000), ("L16", 44100))
        self.assertIsNone(listen_rtp.payload_format(0, 48000))
        self.assertEqual(listen_rtp.decode_payload(0, bytes([1, 2])), [])


if __name__ == "__main__":
    unittest.main()
