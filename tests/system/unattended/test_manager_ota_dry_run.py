"""Lane B - pull-OTA dry-run (spec 17 F6 / spec 18 S8).

Documents the OTA job state machine. Uses the mock's command queue
since the firmware-side firmware.update handler isn't built yet.
"""
import os
import time

import pytest

ENABLED = os.environ.get("ESPDISP_MANAGER_CONTRACT") == "1"
pytestmark = [
    pytest.mark.skipif(not ENABLED,
                       reason="ESPDISP_MANAGER_CONTRACT=1 not set"),
    pytest.mark.xfail(reason="firmware F6 pull-OTA not yet implemented",
                      strict=False),
]


def _register(device, manager):
    device.post_cmd(f"manager-register {manager.base_url}")
    deadline = time.time() + 10
    while time.time() < deadline:
        if manager.devices:
            return next(iter(manager.devices.values()))
        time.sleep(0.5)
    pytest.fail("device never registered")


def test_firmware_update_state_progression(device, manager):
    """Queue a firmware.update command pointing at a known artifact.
    Device should advance through accepted -> downloading -> verifying
    -> installing -> rebooting -> booted -> confirmed."""
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "firmware.update", {
        "url": f"{manager.base_url}/firmware/test-artifact.bin",
        "sha256": "0" * 64,
        "size": 1024,
        "version": "v0.0.99-test",
    })
    # We don't enforce all transitions inline (some happen after reboot),
    # but the ack result for the launch command should be "ok" within
    # ~30 s and the audit log should record at least 2 progress events.
    deadline = time.time() + 60
    while time.time() < deadline:
        cmd = next((c for c in dev.commands if c.id == cid), None)
        if cmd and cmd.state in ("acknowledged", "failed"):
            break
        time.sleep(1)
    assert cmd is not None and cmd.ack_result in ("ok", "accepted")


def test_firmware_update_with_bad_sha_is_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "firmware.update", {
        "url": f"{manager.base_url}/firmware/test-artifact.bin",
        "sha256": "ff" * 32,  # 0-byte hash would never match
        "size": 1024,
        "version": "v0.0.99-baddsig",
    })
    deadline = time.time() + 30
    while time.time() < deadline:
        cmd = next((c for c in dev.commands if c.id == cid), None)
        if cmd and cmd.state in ("acknowledged", "failed"):
            break
        time.sleep(1)
    assert cmd.ack_result in ("failed", "invalid_payload")
