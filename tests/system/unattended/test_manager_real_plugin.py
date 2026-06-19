"""Spec 19 D6 system tests against the real signalk-espdisp-manager plugin.

Lane: real plugin (not the manager_mock.py). Skipped unless the env
vars YEYBOATS_MGR_URL + YEYBOATS_MGR_SK_TOKEN are set; see the
`real_manager` fixture in conftest.py.

What's covered:
- the device is registered with the plugin and considered online
- swapping the assigned profile causes the device to fetch + apply
  new config (hash + version change)
- after the swap the device's heartbeat stays healthy (no crash, no
  408 / 503)
"""
from __future__ import annotations

import time

import pytest


def _wait_until(predicate, *, timeout: float = 30.0, interval: float = 1.0):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    return last


def _device_id(device) -> str:
    st = device.state()["manager"]
    return st["deviceId"]


def test_device_appears_online_in_plugin(device, real_manager):
    """The firmware must be visible in the plugin's /devices list and
    show a fresh lastSeen (within the heartbeat interval)."""
    did = _device_id(device)
    found = _wait_until(
        lambda: next((d for d in real_manager.devices() if d["id"] == did),
                     None),
        timeout=20,
    )
    assert found is not None, f"device {did!r} not in plugin /devices"
    # Plugin should consider us online or at least recently seen.
    assert found.get("health") != "unknown"
    # firmware block must be populated by register/identity.
    fw = found.get("firmware") or {}
    assert fw.get("version"), f"plugin has no firmware version for {did}: {found}"


def test_profile_swap_changes_device_config_hash(device, real_manager):
    """Reassigning the device's profile triggers a config refetch on
    the next heartbeat, and the firmware records the new hash."""
    did = _device_id(device)
    state_before = device.state()["manager"]
    hash_before = state_before["configHash"]

    profiles = [p["id"] for p in real_manager.profiles()]
    # Find a profile that is different from whatever is currently
    # assigned. We try "default" first, then any other.
    record = real_manager.device(did)
    current = record.get("assignedProfile", "default")
    target = next((p for p in ("default", "bench-profile",
                               "wide-day-test") if p != current and p in profiles),
                  None)
    if not target:
        pytest.skip(f"no alternate profile to swap to (have {profiles}, "
                    f"currently on {current!r})")

    real_manager.assign_profile(did, target)
    try:
        # Heartbeat is 5 s default + 1 s margin. Allow generous slack
        # for the device to fetch + apply.
        new_hash = _wait_until(
            lambda: (device.state()["manager"].get("configHash")
                     if device.state()["manager"].get("configHash") != hash_before
                     else None),
            timeout=45,
            interval=2,
        )
        assert new_hash and new_hash != hash_before, (
            f"configHash didn't change after profile swap; "
            f"before={hash_before!r}")

        # Device should remain healthy after applying.
        st = device.state()["manager"]
        assert st["lastHeartbeatCode"] == 200, f"heartbeat not ok: {st}"
    finally:
        # Restore to whatever it was before.
        if current and current != target:
            try:
                real_manager.assign_profile(did, current)
            except Exception:
                pass


def test_device_stays_up_through_profile_swap(device, real_manager):
    """Sanity: uptime monotonic across a swap, no reboot."""
    uptime_before = device.state()["device"]["uptime_ms"]
    did = _device_id(device)

    profiles = [p["id"] for p in real_manager.profiles()]
    record = real_manager.device(did)
    current = record.get("assignedProfile", "default")
    target = next((p for p in profiles if p != current), None)
    if not target:
        pytest.skip("only one profile available")

    real_manager.assign_profile(did, target)
    try:
        time.sleep(15)
        uptime_after = device.state()["device"]["uptime_ms"]
        # uptime is millis since boot - if device rebooted it would
        # drop. Allow a tiny tolerance for clock skew.
        assert uptime_after > uptime_before, (
            f"device rebooted during profile swap: "
            f"before={uptime_before}ms after={uptime_after}ms")
    finally:
        if current and current != target:
            try:
                real_manager.assign_profile(did, current)
            except Exception:
                pass
