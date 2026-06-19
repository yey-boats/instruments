"""Device web Basic Auth contract tests.

Set YEYBOATS_WEB_USERNAME and YEYBOATS_WEB_PASSWORD when the flashed device has
web auth enabled. The shared Device fixture uses those credentials for normal
requests; these tests additionally verify that unauthenticated browser/API
requests receive a Basic challenge.
"""
from __future__ import annotations

import pytest
import requests


def _require_enabled_auth(device):
    state = device.state()
    web_auth = state.get("webAuth") or {}
    if not web_auth.get("enabled"):
        pytest.skip("device web auth is not enabled")
    if not device.auth:
        pytest.skip("set YEYBOATS_WEB_USERNAME and YEYBOATS_WEB_PASSWORD")


def _assert_basic_challenge(response):
    assert response.status_code == 401
    assert "Basic" in response.headers.get("WWW-Authenticate", "")


def test_root_requires_basic_auth_when_enabled(device):
    _require_enabled_auth(device)

    unauthenticated = requests.get(f"{device.base}/", timeout=10)
    _assert_basic_challenge(unauthenticated)

    authenticated = requests.get(f"{device.base}/", timeout=10,
                                 auth=device.auth)
    assert authenticated.status_code == 200
    assert "text/html" in authenticated.headers.get("Content-Type", "")


def test_api_requires_basic_auth_when_enabled(device):
    _require_enabled_auth(device)

    unauthenticated = requests.get(f"{device.base}/api/state", timeout=10)
    _assert_basic_challenge(unauthenticated)

    authenticated = requests.get(f"{device.base}/api/state", timeout=10,
                                 auth=device.auth)
    assert authenticated.status_code == 200
    assert authenticated.json()["webAuth"]["enabled"] is True
