"""Spec 20 system tests: dashboard config import/export + /api/security.

These hit the device's web surface only (no plugin dependency); they
require ESPDISP_HOST. The dashboard config endpoints are aliases for
the existing layout document, so the round-trip uses a minimal valid
layout we know the parser accepts.
"""
from __future__ import annotations

import json
import time

import pytest
import requests


def _get(device, path, **kw):
    r = requests.get(f"{device.base}{path}", timeout=10, **kw)
    r.raise_for_status()
    return r


def _put(device, path, body, *, content_type="application/json"):
    return requests.put(f"{device.base}{path}", data=body, timeout=10,
                        headers={"Content-Type": content_type})


MINIMAL_LAYOUT = {
    "version": 1,
    "screens": [{
        "id": "smoke",
        "title": "Smoke",
        "tiles": [
            {"row": 0, "col": 0, "rowSpan": 1, "colSpan": 1,
             "metric": "sog", "format": "kn"},
        ],
    }],
}


def test_security_endpoint_documents_access_model(device):
    """Spec 20 §Access Security: /api/security reports the active
    access model + writable endpoints + the secrets-echoed=false
    contract."""
    r = _get(device, "/api/security")
    body = r.json()
    assert "web" in body and "ble" in body and "signalk" in body, body
    web = body["web"]
    # The contract from the spec is explicit about these three.
    assert web["secrets_echoed"] is False, "/api/security must promise no secrets"
    assert isinstance(web.get("write_endpoints"), list)
    assert "/api/dashboard/config.json" in web["write_endpoints"]
    assert "/api/layout" in web["write_endpoints"]
    assert web["touch_injection_over_http"] is False
    # BLE and SignalK sub-objects must at least declare an auth mode.
    assert "auth" in body["ble"]
    assert "manager_auth" in body["signalk"]


def test_security_response_contains_no_secrets(device):
    """Belt-and-suspenders: the /api/security blob must not echo any
    WiFi PSK / SignalK bearer / device token, even if the firmware
    later sprouts new fields."""
    raw = _get(device, "/api/security").text.lower()
    # Tokens are typically 100+ chars; password fields tend to use
    # these substring names. Catch any obvious leak before it ships.
    for needle in ("password", "psk", "secret", "bearer ey",
                   "device-token-", "espdisp-dev"):
        assert needle not in raw, f"secret-shaped substring {needle!r} in /api/security"


def test_dashboard_config_json_round_trip(device):
    """PUT /api/dashboard/config.json should accept the same shape
    GET returns. Restore the prior config on the way out so the bench
    isn't left in a smoke-test state."""
    before = _get(device, "/api/dashboard/config.json").text
    try:
        r = _put(device, "/api/dashboard/config.json",
                 json.dumps(MINIMAL_LAYOUT))
        assert r.status_code in (200, 202), f"{r.status_code} {r.text}"
        # The apply is queued; give the UI task a beat to drain.
        time.sleep(2)
        after = _get(device, "/api/dashboard/config.json").json()
        screens = after.get("screens") or after.get("layout", {}).get("screens")
        assert screens, f"no screens in echoed config: {after}"
        ids = [s.get("id") for s in screens]
        assert "smoke" in ids, f"smoke screen missing after round-trip: {ids}"
    finally:
        if before.strip().startswith("{"):
            _put(device, "/api/dashboard/config.json", before)


def test_dashboard_config_yaml_endpoint_is_alias(device):
    """The .yaml endpoint serves the same document under a different
    content type. JSON is a subset of YAML 1.2, so the body should
    parse as JSON too."""
    r = _get(device, "/api/dashboard/config.yaml")
    assert r.status_code == 200
    assert "yaml" in r.headers.get("Content-Type", "").lower()
    # JSON-compatible YAML means the body parses as JSON.
    json.loads(r.text)


def test_dashboard_config_rejects_garbage(device):
    """A malformed PUT body must not crash the device; expect a 4xx
    and the device should still be reachable after."""
    r = _put(device, "/api/dashboard/config.json", "not valid json {[}")
    assert 400 <= r.status_code < 500, f"expected 4xx, got {r.status_code}"
    # Liveness check.
    _get(device, "/api/state")
