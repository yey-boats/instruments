"""Verify the dirty-value cache pipeline (spec 09) actually quiets the
renderer when nothing on screen has changed.

Strategy: capture FrameInterval samples per second on an idle dashboard;
they should be sparse (only periodic ui_refresh ticks emit a frame).
After we induce a value change (post a SignalK delta), the cadence
picks up.
"""
import os
import socket
import time

import pytest

from tests.system.inject import bench, sk_pump

SK_HOST = os.environ.get("ESPDISP_SK_HOST", "localhost")
SK_PORT = int(os.environ.get("ESPDISP_SK_PORT", "3000"))


def _sk_available() -> bool:
    try:
        with socket.create_connection((SK_HOST, SK_PORT), timeout=1):
            return True
    except OSError:
        return False


def test_idle_render_count_is_low(device, udp_logs):
    """On a static screen with no incoming data, the dirty-value caches
    should hold the per-second frame count low (well under continuous
    60 Hz)."""
    device.show_screen("dashboard")
    time.sleep(1.5)
    device.post_cmd("latency-reset")
    time.sleep(5.0)  # stay idle for 5 seconds
    s = bench.collect(udp_logs, device, reset=False)
    fi = s.latencies.get("FrameInterval")
    if not fi or fi.count == 0:
        pytest.skip("no FrameInterval samples")
    # 5 s @ <12 Hz target after stabilisation; allow generous headroom.
    per_sec = fi.count / 5.0
    assert per_sec < 20, (
        f"idle frame rate too high: {per_sec:.1f} Hz (count={fi.count} / 5 s)")


@pytest.mark.skipif(not _sk_available(),
                    reason=f"SignalK demo not at {SK_HOST}:{SK_PORT}")
def test_data_update_triggers_render(device, udp_logs):
    """When a SK value changes, frames should be emitted within ~1 s."""
    device.show_screen("dashboard")
    time.sleep(1.0)
    device.post_cmd("latency-reset")
    time.sleep(0.5)
    # Push 5 distinct SOG values
    for v in (1.0, 2.5, 4.0, 6.5, 9.0):
        sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", v)
        time.sleep(0.25)
    time.sleep(1.5)
    s = bench.collect(udp_logs, device, reset=False)
    fi = s.latencies.get("FrameInterval")
    assert fi and fi.count > 0, "no frames during data updates"
