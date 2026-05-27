"""Lane B - command poll + ack (spec 17 F4 / spec 18 S5)."""
import os
import time

import pytest

ENABLED = os.environ.get("ESPDISP_MANAGER_CONTRACT") == "1"
pytestmark = [
    pytest.mark.skipif(not ENABLED,
                       reason="ESPDISP_MANAGER_CONTRACT=1 not set"),
    pytest.mark.xfail(reason="firmware F4 command-poll not yet implemented",
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


def _wait_ack(manager, dev, cid, timeout_s=20):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        cmd = next((c for c in dev.commands if c.id == cid), None)
        if cmd and cmd.state in ("acknowledged", "failed"):
            return cmd
        time.sleep(0.5)
    pytest.fail(f"command {cid} never acked")


def test_screen_set_command(device, manager):
    dev = _register(device, manager)
    device.show_screen("dashboard")
    cid = manager.queue_command(dev.id, "screen.set", {"id": "wind"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"
    assert device.state()["screen"]["id"] == "wind"


def test_brightness_set_command(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "brightness.set", {"value": 32})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"
    assert device.state()["display"]["brightness"] == 32


def test_beep_command(device, manager):
    """beep is a no-op on boards without a beeper but must still ack ok."""
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "beep", {"duration_ms": 50})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"


def test_unknown_command_is_unsupported(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "definitely.not.a.thing", {})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "unsupported_command"


def test_audit_log_records_command_lifecycle(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "screen.set", {"id": "depth"})
    _wait_ack(manager, dev, cid)
    events = [e for e in manager.audit if e.get("cid") == cid]
    types = {e["event"] for e in events}
    assert {"command.created", "command.ack"}.issubset(types)
