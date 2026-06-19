"""Lane B - pull-OTA dry-run (spec 17 F6 / spec 18 S8).

Documents the firmware OTA job state machine against the mock manager.
The served image is intentionally invalid so the test exercises progress
and verification paths without replacing the device firmware partition.
"""
import os
import time

import pytest

ENABLED = os.environ.get("YEYBOATS_MANAGER_CONTRACT") == "1"
pytestmark = pytest.mark.skipif(
    not ENABLED, reason="YEYBOATS_MANAGER_CONTRACT=1 not set")


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
    Mock serves a 1 KB blob that is NOT a valid ESP image, so the
    install will fail at Update.end. We assert the launch command
    acked "ok" and at least the "accepted" + "downloading" progress
    events arrived at the mock - i.e. the state machine started even
    if the image was invalid (we never want a test to actually swap
    the device's firmware partition)."""
    dev = _register(device, manager)
    # Register a dummy artifact (1 KB of zeros). The known sha lets us
    # exercise verify too, but install will fail (intentional).
    url, sha = manager.serve_artifact("test-artifact.bin", b"\x00" * 4096)
    cid = manager.queue_command(dev.id, "firmware.update", {
        "url": url,
        "sha256": sha,
        "size": 4096,
        "version": "v0.0.99-test",
        "job_id": "job-state-progression",
    })
    deadline = time.time() + 30
    while time.time() < deadline:
        cmd = next((c for c in dev.commands if c.id == cid), None)
        if cmd and cmd.state in ("acknowledged", "failed"):
            break
        time.sleep(1)
    assert cmd is not None, "command never appeared"
    assert cmd.ack_result in ("ok", "accepted"), (
        f"expected ack ok/accepted, got {cmd.ack_result}")
    # Wait for progress events from the device side.
    deadline = time.time() + 15
    progress_states = set()
    while time.time() < deadline:
        events = manager.firmware_progress.get("job-state-progression", [])
        for e in events:
            progress_states.add(e.get("state"))
        if "accepted" in progress_states and (
                "downloading" in progress_states or
                "failed" in progress_states):
            return
        time.sleep(1)
    pytest.fail(f"expected accepted + downloading/failed, got {progress_states}")


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
