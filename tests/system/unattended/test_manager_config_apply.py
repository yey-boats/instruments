"""Lane B - central config fetch + apply (spec 17 F3 / spec 18 S4)."""
import os
import time

import pytest

ENABLED = os.environ.get("ESPDISP_MANAGER_CONTRACT") == "1"
pytestmark = pytest.mark.skipif(
    not ENABLED, reason="ESPDISP_MANAGER_CONTRACT=1 not set")


def _register(device, manager):
    device.post_cmd(f"manager-register {manager.base_url}")
    deadline = time.time() + 10
    while time.time() < deadline:
        if manager.devices:
            return next(iter(manager.devices.values()))
        time.sleep(0.5)
    pytest.fail("device never registered")


def test_brightness_applied_from_profile(device, manager):
    dev = _register(device, manager)
    manager.set_config(dev.id, {
        "ui": {"brightness": 96, "theme": "day"},
    })
    deadline = time.time() + 40
    while time.time() < deadline:
        st = device.state()
        if st["display"]["brightness"] == 96 and st["ui"]["theme"] == "day":
            return
        time.sleep(1)
    pytest.fail("device never adopted pushed profile")


def test_sk_host_applied_from_profile(device, manager):
    dev = _register(device, manager)
    manager.set_config(dev.id, {
        "signalk": {"host": "10.10.10.10", "port": 3000},
    })
    # device should NOT reboot continuously; it should pick up the new
    # host either on next heartbeat-driven apply or after a controlled
    # restart. Allow 60 s.
    deadline = time.time() + 60
    while time.time() < deadline:
        sk = device.state()["sk"]
        if sk["host"] == "10.10.10.10":
            return
        time.sleep(2)
    pytest.fail("sk.host not adopted")


def test_invalid_config_falls_back(device, manager):
    dev = _register(device, manager)
    manager.set_config(dev.id, {
        "ui": {"brightness": -1, "theme": "neon"},   # invalid
    })
    time.sleep(40)
    st = device.state()
    # Brightness must stay in 0..255; theme must remain valid
    assert 0 <= st["display"]["brightness"] <= 255
    assert st["ui"]["theme"] in ("day", "night", "auto")
