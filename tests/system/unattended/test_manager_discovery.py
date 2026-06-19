"""Lane B - manager discovery + registration handshake (spec 17 F2 / spec 18 S2).

These are xfail until the firmware grows the manager subsystem. The
mock side is fully implemented so the tests serve as the executable
contract for both sides.
"""
import os
import time

import pytest

ENABLED = os.environ.get("YEYBOATS_MANAGER_CONTRACT") == "1"
pytestmark = pytest.mark.skipif(
    not ENABLED, reason="YEYBOATS_MANAGER_CONTRACT=1 not set; future contract test")


def test_manual_register(device, manager):
    """Device receives `manager-register http://host:port` via BLE/serial
    console, then registers within ~5 s. Mock should see the request
    and return a token; firmware should persist it."""
    device.post_cmd(f"manager-register {manager.base_url}")
    deadline = time.time() + 10
    while time.time() < deadline:
        if manager.devices:
            break
        time.sleep(0.5)
    assert manager.devices, "no device ever registered"
    (dev,) = manager.devices.values()
    assert dev.identity.get("board_id") == "sunton_4848s040"
    assert len(dev.token) >= 16


@pytest.mark.xfail(reason="firmware F2 (mDNS discovery of _espdisp-mgmt._tcp) not yet implemented",
                   strict=False)
def test_mdns_discovery(device, manager):
    """Mock advertises mDNS; device should discover automatically when
    no manager endpoint is saved."""
    # Plugin-mock mDNS advertisement is a TODO - this test documents
    # the expected device behaviour even before the advertisement.
    device.post_cmd("manager-discover")
    time.sleep(8)
    assert manager.devices, "device should have discovered via mDNS"


def test_register_identity_payload(device, manager):
    device.post_cmd(f"manager-register {manager.base_url}")
    deadline = time.time() + 10
    while time.time() < deadline:
        if manager.devices:
            break
        time.sleep(0.5)
    (dev,) = list(manager.devices.values())
    for required in ("deviceId", "board_id", "chip", "firmware_version"):
        assert required in dev.identity, f"missing {required}"
    caps = dev.capabilities
    assert "touch" in caps and "ble_config" in caps
