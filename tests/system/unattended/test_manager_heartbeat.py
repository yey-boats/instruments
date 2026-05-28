"""Lane B - heartbeat cadence + payload (spec 17 F2 / spec 18 S4)."""
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


def test_first_heartbeat_within_window(device, manager):
    dev = _register(device, manager)
    # Heartbeat interval = 30 s per discovery. Allow 35 s for first
    # one to land.
    deadline = time.time() + 35
    while time.time() < deadline:
        if dev.last_status:
            break
        time.sleep(0.5)
    assert dev.last_status, "no heartbeat received"


def test_heartbeat_payload_groups(device, manager):
    dev = _register(device, manager)
    deadline = time.time() + 35
    while time.time() < deadline:
        if dev.last_status:
            break
        time.sleep(0.5)
    status = dev.last_status
    for group in ("network", "sk", "ui", "touch", "memory", "firmware"):
        assert group in status, f"missing status.{group}"


@pytest.mark.xfail(reason="firmware F3 config drift detection not yet implemented",
                   strict=False)
def test_heartbeat_acks_config_drift(device, manager):
    dev = _register(device, manager)
    manager.set_config(dev.id, {"theme": "night", "brightness": 64})
    deadline = time.time() + 35
    saw_drift = False
    while time.time() < deadline:
        if dev.last_status:
            current = dev.last_status.get("config", {}).get("hash")
            if current and current != dev.config_hash:
                saw_drift = True
                break
        time.sleep(0.5)
    assert saw_drift, "device never reported a config-drift"
