"""
Unit tests for COBS encoding/decoding and frame protocol.
Ensures compatibility with firmware protocol invariants:
- Payload: RGB565 Little-Endian
- Encoding: COBS
- Delimiter: trailing 0x00
"""

import os
import pytest

from led_matrix_controller.controller import (
    cobs_encode,
)
from led_matrix_controller.devices.simulator import (
    _cobs_decode,
    _decode_frame,
    FRAME_SIZE_RGB565,
)


class TestCOBSProtocol:
    """Test COBS encoding and decoding correctness."""

    @pytest.mark.parametrize(
        "raw, expected_cobs",
        [
            (b"", b"\x01"),
            (b"\x00", b"\x01\x01"),
            (b"\x00\x00", b"\x01\x01\x01"),
            (b"\x01\x02\x03", b"\x04\x01\x02\x03"),
            (b"\x11\x22\x00\x33", b"\x03\x11\x22\x02\x33"),
            (b"\x11\x00\x00\x22", b"\x02\x11\x01\x02\x22"),
            # 254 non-zero bytes (single full block followed by end code 0x01)
            (bytes(range(1, 255)), b"\xff" + bytes(range(1, 255)) + b"\x01"),
            # 255 non-zero bytes (block + 1 byte)
            (
                bytes((i % 254) + 1 for i in range(255)),
                b"\xff"
                + bytes((i % 254) + 1 for i in range(254))
                + b"\x02"
                + bytes([(254 % 254) + 1]),
            ),
        ],
    )
    def test_cobs_known_vectors(self, raw: bytes, expected_cobs: bytes):
        """Test against well-known standard COBS vectors."""
        encoded = cobs_encode(raw)
        assert encoded == expected_cobs
        decoded = _cobs_decode(encoded)
        assert decoded == raw

    def test_cobs_no_zero_bytes_in_encoded_data(self):
        """Verify that encoded COBS bytes NEVER contain 0x00."""
        test_patterns = [
            b"\x00" * 100,
            bytes(range(256)),
            os.urandom(1024),
            os.urandom(FRAME_SIZE_RGB565),
        ]
        for data in test_patterns:
            encoded = cobs_encode(data)
            assert b"\x00" not in encoded, "COBS payload must never contain 0x00"
            assert _cobs_decode(encoded) == data

    def test_frame_size_and_framing(self):
        """Verify full-frame (8192 bytes RGB565) framing with trailing 0x00."""
        # 128x32x2 = 8192 bytes
        dummy_rgb565 = os.urandom(FRAME_SIZE_RGB565)
        encoded = cobs_encode(dummy_rgb565)

        # Wire packet has trailing 0x00
        packet = encoded + b"\x00"

        assert packet[-1] == 0x00
        assert b"\x00" not in packet[:-1]

        # Simulator frame decoder should succeed
        decoded = _decode_frame(packet)
        assert decoded == dummy_rgb565
        assert len(decoded) == FRAME_SIZE_RGB565

    def test_frame_size_mismatch_rejected(self):
        """Verify that packet with invalid decoded size is rejected."""
        # Truncated payload (4000 bytes instead of 8192)
        short_data = os.urandom(4000)
        packet = cobs_encode(short_data) + b"\x00"

        with pytest.raises(ValueError, match="invalid frame size"):
            _decode_frame(packet)

    def test_truncated_cobs_packet_rejected(self):
        """Verify that malformed/truncated COBS data raises an error."""
        # Header specifies block length 10, but only 3 bytes provided
        bad_cobs = b"\x0a\x01\x02\x03"
        with pytest.raises(ValueError, match="block length overflow"):
            _cobs_decode(bad_cobs)

    def test_zero_inside_cobs_payload_rejected(self):
        """Verify that a 0x00 inside the COBS payload raises an error."""
        bad_cobs = b"\x03\x01\x00\x02"
        with pytest.raises(ValueError, match="zero byte in payload"):
            _cobs_decode(bad_cobs)
