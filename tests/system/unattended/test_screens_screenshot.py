"""Iterate every registered screen, switch to it, snapshot it. The
artifacts dir then contains a baseline for visual review.

This test does NOT assert pixel content - that's brittle and varies
between firmware revisions. It only asserts the BMP came back well-
formed (header + non-zero payload). Use the artifacts in code review.
"""
import struct
import time


def _validate_bmp(path) -> None:
    data = path.read_bytes()
    assert len(data) > 1024, f"{path}: too small ({len(data)} bytes)"
    assert data[:2] == b"BM", f"{path}: bad magic {data[:2]!r}"
    file_size = struct.unpack_from("<I", data, 2)[0]
    assert file_size == len(data), f"{path}: header size {file_size} != actual {len(data)}"


def test_every_screen_snapshots(device, artifacts):
    screens = device.screens()
    for s in screens:
        sid = s["id"]
        device.show_screen(sid)
        time.sleep(1.2)
        path = device.screenshot(f"screen_{sid}")
        _validate_bmp(path)
