"""Touch + gesture injection via BLE NUS or USB serial.

Injection is deliberately NOT carried over HTTP. The `console` fixture
opens whichever transport is configured (ESPDISP_SERIAL_PORT preferred,
falls back to ESPDISP_BLE_NAME) and stays connected for the session.

Two injection levels:

  Level 1 - touchscreen manager
    touch <x> <y> <0|1>        raw snapshot write
    tap <x> <y> [hold_ms]      synthesised press+hold+release
    swipe <x0> <y0> <x1> <y1>  intermediate samples + detect_swipe_release

  Level 2 - action queue
    gesture <left|right|up|down>  bypass touch, post ShowScreen directly
"""
import time

import pytest


# --- Security: HTTP must reject injection words ----------------------

@pytest.mark.parametrize("line", [
    "tap 120 180",
    "swipe 400 240 80 240",
    "gesture left",
    "touch 100 100 1",
])
def test_http_rejects_injection_words(device, line):
    """Tests that /api/cmd actively refuses these to keep the IP path
    safe. This test does NOT require the console fixture - it's the
    one place HTTP touches the injection surface (only to refuse it).
    """
    import requests
    auth_kw = {"auth": device.auth} if device.auth else {}
    r = requests.post(f"{device.base}/api/cmd",
                      data=line, timeout=5,
                      headers={"Content-Type": "text/plain"},
                      **auth_kw)
    assert r.status_code == 403, f"expected 403 for {line!r}, got {r.status_code} {r.text!r}"


# --- Level 2: action-queue injection ---------------------------------

def _ensure_dashboard(device):
    device.show_screen("dashboard")
    device.wait_for_screen("dashboard")


def test_gesture_left_goes_next(device, console):
    _ensure_dashboard(device)
    before = device.state()["screen"]["index"]
    device.gesture(console, "left")
    time.sleep(0.6)
    after = device.state()["screen"]["index"]
    assert after != before


def test_gesture_up_opens_settings(device, console):
    _ensure_dashboard(device)
    device.gesture(console, "up")
    device.wait_for_screen("settings", timeout_s=4)


def test_gesture_down_returns_to_dashboard(device, console):
    device.show_screen("settings")
    device.wait_for_screen("settings")
    device.gesture(console, "down")
    device.wait_for_screen("dashboard", timeout_s=4)


def test_gesture_unknown_logs_failure(device, console):
    console.send("gesture sideways")
    line = console.wait_for("[test] gesture ok=", timeout_s=3)
    assert "ok=0" in line


# --- Level 1: low-level synthesised swipe ----------------------------

def test_swipe_left_navigates(device, console):
    _ensure_dashboard(device)
    before = device.state()["screen"]["index"]
    device.swipe(console, x0=400, y0=240, x1=80, y1=240, dur_ms=300)
    time.sleep(0.8)
    after = device.state()["screen"]["index"]
    assert after != before


def test_swipe_up_opens_settings(device, console):
    _ensure_dashboard(device)
    device.swipe(console, x0=240, y0=420, x1=240, y1=80, dur_ms=300)
    device.wait_for_screen("settings", timeout_s=4)


def test_swipe_too_short_does_nothing(device, console):
    device.show_screen("settings")
    device.wait_for_screen("settings")
    device.swipe(console, x0=240, y0=240, x1=240, y1=240 - 30, dur_ms=200)
    time.sleep(0.6)
    assert device.state()["screen"]["id"] == "settings"


# --- Level 1: raw tap into the touch snapshot ------------------------

def test_tap_on_dashboard_tile(device, console):
    _ensure_dashboard(device)
    device.tap(console, x=120, y=180, hold_ms=80)
    device.wait_for_screen("wind", timeout_s=4)


def test_tap_outside_any_tile_is_inert(device, console):
    _ensure_dashboard(device)
    before = device.state()["screen"]["id"]
    device.tap(console, x=10, y=10, hold_ms=80)
    time.sleep(0.6)
    assert device.state()["screen"]["id"] == before


def test_raw_touch_press_release(device, console):
    device.touch(console, x=240, y=240, pressed=True)
    device.touch(console, x=0, y=0, pressed=False)
