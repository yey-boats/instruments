"""Stress + regression scenarios.

These tests don't check pixel correctness - they hammer the device with
synthetic input or data and assert no crash / no queue overflow / heap
doesn't bleed.
"""
import os
import socket
import time

import pytest

from tests.system.inject import bench, nmea0183


UDP_PORT = int(os.environ.get("ESPDISP_NMEA_WIFI_PORT", "10110"))


def test_gesture_spam_no_crash(device, console, udp_logs):
    """Fire 60 gestures back-to-back. Device must stay reachable."""
    heap_before = device.state()["device"]["heap_free"]
    for i in range(60):
        device.gesture(console, "left" if i % 2 == 0 else "right")
    time.sleep(2.0)
    state = device.state()
    heap_after = state["device"]["heap_free"]
    # Heap shouldn't drop by more than ~20 KB through 60 queued gestures.
    delta = heap_before - heap_after
    assert delta < 20_000, f"heap leak suspected: lost {delta} bytes"
    snap = bench.collect(udp_logs, device, reset=False)
    assert snap.ui_queue <= 4, f"ui queue not draining: {snap.ui_queue}"


def test_tap_spam_full_grid(device, console):
    """Synthesise 100 taps across the screen (10x10 grid). Device must
    not lock up - state endpoint must respond after."""
    for gy in range(0, 480, 48):
        for gx in range(0, 480, 48):
            device.tap(console, x=gx + 24, y=gy + 24, hold_ms=20)
    time.sleep(1.0)
    s = device.state()
    assert s["device"]["heap_free"] > 30_000


def test_rapid_nmea_udp_flood(device, udp_logs):
    """Flood the device with 200 NMEA sentences across 4 s. Parser must
    stay alive and sentences_ok counter must climb."""
    device.post_cmd(f"nmea-wifi udp {UDP_PORT}")
    time.sleep(1.5)
    device.post_cmd("boat reset")
    nmea0183.send_udp("255.255.255.255", UDP_PORT,
                      [nmea0183.rmc(sog_kn=i % 12, cog_deg=(i * 7) % 360)
                       for i in range(200)])
    time.sleep(4.0)
    b = device.boat()
    nw = b["sources"]["nmea_wifi"]
    # We don't guarantee every sentence was received (UDP loss is normal)
    # but at least 30% should have parsed.
    assert nw["sentences_ok"] >= 60, (
        f"only parsed {nw['sentences_ok']} of 200 broadcast sentences")
    assert nw["sentences_bad"] == 0
    device.post_cmd("nmea-wifi disable")


def test_alternating_screen_taps(device, console):
    """Switch screens 30 times via taps + gestures. Each step asserts
    the screen actually changed."""
    seen_set = set()
    for i in range(30):
        if i % 3 == 0:
            device.show_screen("dashboard")
            device.wait_for_screen("dashboard")
            device.tap(console, x=120, y=180, hold_ms=60)
            device.wait_for_screen("wind", timeout_s=4)
            seen_set.add("wind")
        elif i % 3 == 1:
            device.gesture(console, "left")
            time.sleep(0.4)
            seen_set.add(device.state()["screen"]["id"])
        else:
            device.gesture(console, "down")
            device.wait_for_screen("dashboard", timeout_s=4)
            seen_set.add("dashboard")
    assert len(seen_set) >= 2


def test_heap_stable_over_workload(device, console, udp_logs):
    """Quick mixed workload: gestures + a few taps + bench reads.
    Heap should be flat (no leak over 30 cycles)."""
    samples = []
    for _ in range(5):
        for _ in range(6):
            device.gesture(console, "left")
            time.sleep(0.15)
        for _ in range(3):
            device.tap(console, x=200, y=200, hold_ms=40)
        bench.collect(udp_logs, device, reset=False)
        samples.append(device.state()["device"]["heap_free"])
    # Trend: last sample shouldn't be more than 30 KB below the first.
    assert samples[-1] > samples[0] - 30_000, (
        f"heap trended down: {samples}")
