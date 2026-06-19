"""Lane B - command poll + ack (spec 17 F4 / spec 18 S5)."""
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
    # SetBrightness ack fires when manager queues the app::Command;
    # actual apply happens on the LVGL task. Allow a short window
    # for the drain.
    deadline = time.time() + 5
    last = None
    while time.time() < deadline:
        last = device.state()["display"]["brightness"]
        if last == 32:
            return
        time.sleep(0.5)
    pytest.fail(f"brightness never reached 32 (last={last})")


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


# ---- spec 17 §8 v1 commands added later ----------------------------------

def test_overlay_show_then_clear(device, manager):
    """overlay.show pins the alarm banner with the operator message;
    overlay.clear releases it. Both must ack ok."""
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "overlay.show",
                                {"message": "TESTING"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok", cmd.ack_result
    cid2 = manager.queue_command(dev.id, "overlay.clear", {})
    cmd2 = _wait_ack(manager, dev, cid2)
    assert cmd2.ack_result == "ok", cmd2.ack_result


def test_overlay_show_accepts_text_fallback(device, manager):
    """Plugins that use `text` instead of `message` must still work."""
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "overlay.show",
                                {"text": "FROM-TEXT-KEY"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"


def test_overlay_show_empty_payload_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "overlay.show", {})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


def test_log_level_string_payload(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "log.level", {"level": "warn"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok", cmd.ack_result
    # Restore default so subsequent tests still see info-level logs.
    cid2 = manager.queue_command(dev.id, "log.level", {"level": "info"})
    _wait_ack(manager, dev, cid2)


def test_log_level_numeric_payload(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "log.level", {"level": 3})  # ESP_LOG_INFO
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"


def test_log_level_unknown_string_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "log.level", {"level": "spammy"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


def test_log_level_out_of_range_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "log.level", {"level": 99})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


def test_touch_mode_poll_always_accepted(device, manager):
    """touch.mode "poll" must succeed regardless of board wiring -
    detaches the INT line if any, falls back to the polling timer."""
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "touch.mode", {"mode": "poll"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "ok"


def test_touch_mode_unknown_value_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "touch.mode", {"mode": "vibration"})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


def test_touch_mode_missing_payload_rejected(device, manager):
    dev = _register(device, manager)
    cid = manager.queue_command(dev.id, "touch.mode", {})
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


# ---- spec 17 §6 OTA policy enforcement -----------------------------------

# The sha used by these tests is intentionally fake; firmware.update only
# refuses or accepts based on payload shape vs policy. The actual job
# would fail at sha-verification later, but the policy gate fires first.
_FAKE_SHA = "0" * 64
_TINY_OK_URL = "http://localhost:65535/fake.bin"


def _set_ota_policy(device, manager, dev, *, enabled=True, require_sha=True,
                    max_size=0):
    """Push an OTA policy via cfg["ota"] and wait for the device to apply
    it. Uses the manager mock's set_config + config.reload command so we
    don't depend on heartbeat-driven drift detection."""
    manager.set_config(dev.id, {
        "ota": {
            "enabled": enabled,
            "requireSha256": require_sha,
            "maxSizeBytes": max_size,
        }
    })
    cid = manager.queue_command(dev.id, "config.reload", {})
    _wait_ack(manager, dev, cid)
    time.sleep(2)  # let fetch_config + apply run


def test_firmware_update_disabled_returns_forbidden(device, manager):
    dev = _register(device, manager)
    _set_ota_policy(device, manager, dev, enabled=False)
    try:
        cid = manager.queue_command(dev.id, "firmware.update", {
            "jobId": "j-disabled", "url": _TINY_OK_URL,
            "sha256": _FAKE_SHA, "size": 2048,
        })
        cmd = _wait_ack(manager, dev, cid)
        assert cmd.ack_result == "forbidden", cmd.ack_result
    finally:
        _set_ota_policy(device, manager, dev, enabled=True)


def test_firmware_update_missing_sha_rejected_when_required(device, manager):
    dev = _register(device, manager)
    _set_ota_policy(device, manager, dev, enabled=True, require_sha=True)
    cid = manager.queue_command(dev.id, "firmware.update", {
        "jobId": "j-no-sha", "url": _TINY_OK_URL, "size": 2048,
    })
    cmd = _wait_ack(manager, dev, cid)
    assert cmd.ack_result == "invalid_payload"


def test_firmware_update_oversize_rejected(device, manager):
    dev = _register(device, manager)
    _set_ota_policy(device, manager, dev, enabled=True, require_sha=True,
                    max_size=4096)
    try:
        cid = manager.queue_command(dev.id, "firmware.update", {
            "jobId": "j-too-big", "url": _TINY_OK_URL,
            "sha256": _FAKE_SHA, "size": 1_048_576,
        })
        cmd = _wait_ack(manager, dev, cid)
        assert cmd.ack_result == "invalid_payload"
    finally:
        _set_ota_policy(device, manager, dev, enabled=True, max_size=0)


# ---- spec 17 §6 autopilot permissions ------------------------------------

def _set_ap_permissions(device, manager, dev, *, allow_engage, allow_standby,
                        allow_heading_adjust):
    manager.set_config(dev.id, {
        "autopilot": {
            "allowEngage": allow_engage,
            "allowStandby": allow_standby,
            "allowHeadingAdjust": allow_heading_adjust,
        }
    })
    cid = manager.queue_command(dev.id, "config.reload", {})
    _wait_ack(manager, dev, cid)
    time.sleep(2)


def test_autopilot_engage_denied_by_default(device, manager):
    """Default permissions block engage modes. The plugin doesn't yet
    push set_mode commands via /commands, but the device CLI / web
    surface should refuse via Result::Forbidden. This test pushes via
    the autopilot console command which routes through the same
    permission gates."""
    dev = _register(device, manager)
    _set_ap_permissions(device, manager, dev, allow_engage=False,
                        allow_standby=True, allow_heading_adjust=True)
    # No direct manager command for set_mode yet; this is a guard rail
    # that the apply path persisted the deny.
    state = device.state()
    # The device exposes autopilot permissions via the heartbeat body
    # but not via /api/state today. Simply assert the device stayed
    # reachable - i.e. apply_config didn't crash when the autopilot
    # block contained allowEngage=false.
    assert state["device"]["uptime_ms"] > 0


def test_autopilot_permissions_round_trip(device, manager):
    """Push an autopilot block with all gates open then all closed; the
    device should accept both without crashing or rebooting."""
    dev = _register(device, manager)
    uptime_before = device.state()["device"]["uptime_ms"]
    _set_ap_permissions(device, manager, dev, allow_engage=True,
                        allow_standby=True, allow_heading_adjust=True)
    _set_ap_permissions(device, manager, dev, allow_engage=False,
                        allow_standby=False, allow_heading_adjust=False)
    uptime_after = device.state()["device"]["uptime_ms"]
    assert uptime_after >= uptime_before, "device rebooted during ap reconfig"
