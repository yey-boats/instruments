"""Future firmware contract tests for the SignalK espdisp-manager plugin.

These tests are opt-in because the current firmware does not yet implement the
manager client. Enable them when working on that firmware support:

    ESPDISP_MANAGER_CONTRACT=1 \
    ESPDISP_HOST=<device-ip> \
    SIGNALK_URL=http://localhost:3000 \
    pytest tests/system/unattended/test_espdisp_manager_contract.py
"""
from __future__ import annotations

import json
import os
import time
from urllib.parse import urlparse

import pytest
import requests


pytestmark = pytest.mark.skipif(
    os.environ.get("ESPDISP_MANAGER_CONTRACT") != "1",
    reason="ESPDISP_MANAGER_CONTRACT=1 not set; future manager contract test is opt-in",
)


def _signalk_url() -> str:
    return os.environ.get("SIGNALK_URL", "http://localhost:3000").rstrip("/")


def _signalk_login() -> str:
    url = _signalk_url()
    username = os.environ.get("SIGNALK_USERNAME", "admin")
    password = os.environ.get("SIGNALK_PASSWORD", "admin")
    r = requests.post(
        f"{url}/signalk/v1/auth/login",
        json={"username": username, "password": password},
        timeout=10,
    )
    r.raise_for_status()
    return r.json()["token"]


def _sk_headers(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def _manager_headers(token: str) -> dict[str, str]:
    return {
        "Authorization": f"Bearer {token}",
        "X-EspDisp-Authorization": "Bearer espdisp-dev",
    }


def _wait_for_manager_state(device, timeout_s: float = 20.0) -> dict:
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        last = device.state().get("manager")
        if last and last.get("registered") and last.get("lastHeartbeatOk"):
            return last
        time.sleep(0.5)
    pytest.fail(f"manager state did not become healthy; last={last!r}")


def test_firmware_registers_with_espdisp_manager(device, console):
    token = _signalk_login()
    signalk = urlparse(_signalk_url())
    host = signalk.hostname or "localhost"
    port = signalk.port or 3000

    discovery = requests.get(
        f"{_signalk_url()}/plugins/espdisp-manager/.well-known/espdisp-management",
        headers=_sk_headers(token),
        timeout=10,
    )
    discovery.raise_for_status()
    assert discovery.json()["protocol"] == "espdisp.management.v1"

    # Future firmware commands. These intentionally define the firmware-side
    # contract that will be implemented next.
    device.cmd_via_console(console, f"manager-sk-token {token}", wait_for="[mgr] sk_token saved", timeout_s=5)
    device.cmd_via_console(console, "manager-token espdisp-dev", wait_for="[mgr] token saved", timeout_s=5)
    device.cmd_via_console(
        console,
        f"manager-register {host} {port}",
        wait_for="[mgr] registered",
        timeout_s=15,
    )

    state = _wait_for_manager_state(device)
    device_id = state["deviceId"]
    assert device_id.startswith("espdisp-")

    devices = requests.get(
        f"{_signalk_url()}/plugins/espdisp-manager/devices",
        headers=_sk_headers(token),
        timeout=10,
    )
    devices.raise_for_status()
    assert device_id in {d["id"] for d in devices.json()["devices"]}


def test_firmware_fetches_config_and_executes_commands(device, console):
    token = _signalk_login()
    manager_state = _wait_for_manager_state(device)
    device_id = manager_state["deviceId"]

    config = requests.get(
        f"{_signalk_url()}/plugins/espdisp-manager/devices/{device_id}/config",
        headers=_manager_headers(token),
        timeout=10,
    )
    config.raise_for_status()
    assert config.json()["hash"] == manager_state["configHash"]

    command = requests.post(
        f"{_signalk_url()}/plugins/espdisp-manager/devices/{device_id}/command",
        headers=_sk_headers(token),
        json={"type": "screen.set", "payload": {"screen": "dashboard"}},
        timeout=10,
    )
    command.raise_for_status()
    command_id = command.json()["id"]

    device.wait_for_screen("dashboard", timeout_s=15)

    deadline = time.time() + 15
    last = None
    while time.time() < deadline:
        r = requests.get(
            f"{_signalk_url()}/plugins/espdisp-manager/devices/{device_id}/commands/{command_id}",
            headers=_sk_headers(token),
            timeout=10,
        )
        r.raise_for_status()
        last = r.json()
        if last["status"] == "acknowledged":
            break
        time.sleep(0.5)

    assert last and last["status"] == "acknowledged", json.dumps(last, indent=2)
