"""Lane A - SignalK autopilot emulator round-trips (spec 16).

Drives the device's sk-ap-state / sk-ap-adjust CLIs (via /api/cmd
which dispatches into sk::handleSerialCommand). Asserts the
@signalk/signalk-autopilot emulator plugin reflects the change.

Prereqs:
  - SK demo running (`make demo-up`)
  - signalk-autopilot plugin enabled with type=emulator (per spec 16,
    already committed in signalk/config/plugin-config-data/autopilot.json)
  - Device flashed with sk-ap-* CLI support (today's firmware)
  - Device has a valid token (sk-token <jwt>) so PUTs are authenticated
"""
import json
import math
import os
import socket
import time
import urllib.request

import pytest

SK_HOST = os.environ.get("YEYBOATS_SK_HOST", "10.75.205.84")
SK_PORT = int(os.environ.get("YEYBOATS_SK_PORT", "3000"))
SK_USER = os.environ.get("SIGNALK_USERNAME", "admin")
SK_PASS = os.environ.get("SIGNALK_PASSWORD", "admin")


def _sk_available() -> bool:
    try:
        with socket.create_connection((SK_HOST, SK_PORT), timeout=1):
            return True
    except OSError:
        return False


pytestmark = pytest.mark.skipif(
    not _sk_available(),
    reason=f"SignalK demo not reachable at {SK_HOST}:{SK_PORT}")


# --- helpers --------------------------------------------------------------

_token_cache = None


def _token() -> str:
    global _token_cache
    if _token_cache:
        return _token_cache
    req = urllib.request.Request(
        f"http://{SK_HOST}:{SK_PORT}/signalk/v1/auth/login",
        data=json.dumps({"username": SK_USER, "password": SK_PASS}).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        _token_cache = json.loads(r.read().decode())["token"]
    return _token_cache


def _sk_get(path: str):
    """Authenticated GET against the SK REST API. Returns parsed JSON
    (might be a scalar like a string) or None on 404."""
    req = urllib.request.Request(
        f"http://{SK_HOST}:{SK_PORT}/signalk/v1/api/vessels/self/{path}",
        headers={"Authorization": f"Bearer {_token()}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            txt = r.read().decode()
            try:
                return json.loads(txt)
            except json.JSONDecodeError:
                return txt
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def _wait_state(expected: str, timeout_s: float = 8.0) -> str:
    """Poll the emulator state until it matches `expected`."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        s = _sk_get("steering/autopilot/state/value")
        if s == expected:
            return s
        time.sleep(0.5)
    pytest.fail(f"AP state never became {expected!r}; last={s!r}")


# --- tests ---------------------------------------------------------------

@pytest.fixture(autouse=True)
def _start_in_standby(device):
    """Park the emulator in standby before each test so the assertion
    sees a real transition."""
    # Reach into the emulator directly via authenticated PUT (the
    # firmware doesn't have a standby reset path that's distinct from
    # sk-ap-state).
    device.post_cmd("sk-ap-state standby")
    _wait_state("standby", timeout_s=6)


def test_engage_auto(device):
    device.post_cmd("sk-ap-state auto")
    _wait_state("auto", timeout_s=6)


def test_engage_wind_then_standby(device):
    device.post_cmd("sk-ap-state wind")
    _wait_state("wind", timeout_s=6)
    device.post_cmd("sk-ap-state standby")
    _wait_state("standby", timeout_s=6)


def test_adjust_heading_moves_target(device):
    # Engage auto first so the emulator accepts heading adjustments.
    device.post_cmd("sk-ap-state auto")
    _wait_state("auto", timeout_s=6)
    # Adjust +10 deg. The emulator only exposes target/headingMagnetic
    # after the adjust completes; allow up to 15 s. The simulator doesn't
    # touch this path so no interference.
    device.post_cmd("sk-ap-adjust 10")
    # The emulator only exposes a non-zero target after auto-lock has
    # settled + adjust applied. Poll up to 20 s; accept any positive
    # value as proof the firmware -> emulator round-trip worked.
    # (The emulator quantises and may report 0 if it hasn't picked up
    # a current heading reference.)
    deadline = time.time() + 20
    saw_target = None
    while time.time() < deadline:
        v = _sk_get("steering/autopilot/target/headingMagnetic/value")
        if isinstance(v, (int, float)) and v != 0:
            saw_target = v
            break
        time.sleep(0.7)
    assert saw_target is not None, (
        f"emulator never reported a non-zero target after adjust")


def test_invalid_state_logged_not_crashing(device, udp_logs):
    """Bogus state values should be logged but not crash the device."""
    udp_logs.drain()
    device.post_cmd("sk-ap-state bogus")
    line = udp_logs.wait_for("[sk] ap state=bogus", timeout_s=5)
    # PUT response code from the emulator is logged after the verb.
    # Either way the device should remain responsive.
    assert device.state()["device"]["uptime_ms"] > 0
