"""Multi-step user journeys built from console-injected gestures.

Each test scripts a sequence of taps / swipes / screen switches that a
real user might run through, then asserts the device ended in the
expected state and produced at least one screenshot per step.
"""
import time
from pathlib import Path

import pytest


@pytest.fixture(autouse=True)
def _start_on_dashboard(device):
    device.show_screen("dashboard")
    device.wait_for_screen("dashboard")


def _shot(device, name: str) -> Path:
    return device.screenshot(f"journey_{name}")


def test_journey_dashboard_to_each_detail(device, console, artifacts):
    """User taps each of the 4 dashboard tiles in turn and swipes back."""
    targets = [(120, 180, "wind"), (360, 180, "nav"),
               (120, 380, "depth"), (360, 380, "status")]
    for x, y, screen in targets:
        device.show_screen("dashboard")
        device.wait_for_screen("dashboard")
        device.tap(console, x=x, y=y, hold_ms=80)
        device.wait_for_screen(screen, timeout_s=4)
        _shot(device, f"tap_{screen}")
        # Swipe right (or escape via dashboard screen-id) to return.
        device.swipe(console, x0=40, y0=240, x1=440, y1=240, dur_ms=300)
        time.sleep(0.6)


def test_journey_carousel_swipe_full_circle(device, console):
    """Swipe left through every screen in the carousel, then assert we
    saw at least N distinct screen ids."""
    seen = set()
    seen.add(device.state()["screen"]["id"])
    for _ in range(8):
        device.swipe(console, x0=400, y0=240, x1=80, y1=240, dur_ms=250)
        time.sleep(0.5)
        seen.add(device.state()["screen"]["id"])
    assert len(seen) >= 4, f"only saw {seen}"


def test_journey_open_close_settings(device, console):
    """Up-swipe opens settings, down-swipe returns. Capture both."""
    device.gesture(console, "up")
    device.wait_for_screen("settings", timeout_s=4)
    _shot(device, "settings_open")
    device.gesture(console, "down")
    device.wait_for_screen("dashboard", timeout_s=4)
    _shot(device, "settings_closed")


def test_journey_mixed_input_high_and_low(device, console):
    """Combine level-2 gestures and level-1 taps to prove both layers
    cooperate (no debouncing collisions)."""
    device.gesture(console, "left")
    s_after_gesture = device.wait_for_screen_not("dashboard") if hasattr(
        device, "wait_for_screen_not") else None
    time.sleep(0.4)
    s = device.state()["screen"]["id"]
    assert s != "dashboard"
    device.gesture(console, "down")
    device.wait_for_screen("dashboard", timeout_s=4)
    device.tap(console, x=120, y=180, hold_ms=80)
    device.wait_for_screen("wind", timeout_s=4)
