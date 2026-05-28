"""Smoke test: device is reachable, /api/state is well-formed, free
heap is plausible, current screen advances on `screen <id>` command.
"""
import pytest


def test_state_endpoint(device):
    s = device.state()
    assert "device" in s and "wifi" in s and "sk" in s and "screen" in s
    # Threshold relaxed from 50 KB to 30 KB now that the manager
    # subsystem (F1-F6), beeper, autopilot, and device_identity all
    # ship in the default build. The device still has ~7 MB PSRAM and
    # the LVGL renderer lives there; internal heap >= 30 KB is healthy.
    assert s["device"]["heap_free"] > 30_000, f"heap suspiciously low: {s['device']['heap_free']}"
    assert s["wifi"]["state"] in ("sta", "ap"), s["wifi"]["state"]


def test_screen_listing(device):
    screens = device.screens()
    assert len(screens) >= 5, f"expected several screens, got {len(screens)}"
    ids = [s["id"] for s in screens]
    for must_have in ("dashboard", "wind", "depth"):
        assert must_have in ids, f"missing screen {must_have!r}"


def test_screen_switch(device, artifacts):
    device.show_screen("dashboard")
    # Eventual consistency - the show is queued for the UI task.
    import time
    time.sleep(1.5)
    s = device.state()
    assert s["screen"]["id"] == "dashboard"
    device.screenshot("boot_health_dashboard")


def test_boat_endpoint(device):
    b = device.boat()
    assert "priority" in b and "fields" in b and "sources" in b
    # Default priority chain
    order = b["priority"]["order"]
    assert order[0] == "nmea2000"
    assert order[1] == "nmea-wifi"
    assert order[2] == "signalk"
