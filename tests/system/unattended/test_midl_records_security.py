"""MIDL records push API + /api/security (docs/midl/midl-only.md).

These hit the device's web surface only (no plugin dependency); they
require YEYBOATS_HOST. The display renders MIDL records only: the legacy
dashboard-config / layout endpoints are gone, records are written with
PUT /api/midl/records/<id> (web Basic auth -> source "local").
"""
from __future__ import annotations

import copy
import json

import requests


def _get(device, path, **kw):
    kw.setdefault("auth", device.auth)
    r = requests.get(f"{device.base}{path}", timeout=10, **kw)
    r.raise_for_status()
    return r


def _req(device, method, path, body=None):
    auth_kw = {"auth": device.auth} if device.auth else {}
    return requests.request(method, f"{device.base}{path}", data=body, timeout=10,
                            headers={"Content-Type": "application/json"}, **auth_kw)


def test_security_endpoint_documents_access_model(device):
    """/api/security reports the active access model + writable endpoints
    + the secrets-echoed=false contract."""
    body = _get(device, "/api/security").json()
    assert "web" in body and "ble" in body and "signalk" in body, body
    web = body["web"]
    assert web["secrets_echoed"] is False, "/api/security must promise no secrets"
    writes = web.get("write_endpoints")
    assert isinstance(writes, list)
    assert "/api/midl/records/<id>" in writes
    assert "/api/midl/index" in writes
    for gone in ("/api/layout", "/api/dashboard/config.json", "/api/dashboard/config.yaml"):
        assert gone not in writes, f"retired endpoint {gone} still listed"
    assert web["touch_injection_over_http"] is False
    assert "auth" in body["ble"]
    assert "manager_auth" in body["signalk"]


def test_security_response_contains_no_secrets(device):
    """The /api/security blob must not echo any WiFi PSK / SignalK bearer /
    device token, even if the firmware later sprouts new fields."""
    raw = _get(device, "/api/security").text.lower()
    for needle in ("password", "psk", "secret", "bearer ey",
                   "device-token-", "espdisp-dev"):
        assert needle not in raw, f"secret-shaped substring {needle!r} in /api/security"


def test_legacy_layout_endpoints_are_gone(device):
    auth_kw = {"auth": device.auth} if device.auth else {}
    for path in ("/api/layout", "/api/dashboard/config.json", "/api/dashboard/config.yaml"):
        r = requests.get(f"{device.base}{path}", timeout=10, **auth_kw)
        assert r.status_code == 404, f"{path} -> {r.status_code}"


def test_embedded_records_listed(device):
    body = _get(device, "/api/midl/records").json()
    ids = [r["id"] for r in body["records"]]
    assert body["active"] in ids
    for r in body["records"]:
        assert r["hash"].startswith("sha256:"), r
        assert r["screens"], r


def test_local_record_round_trip(device):
    """PUT a local record (a copy of an embedded doc under a new screen id),
    read it back, then DELETE it so the bench is left unchanged."""
    body = _get(device, "/api/midl/records").json()
    src_id = body["records"][0]["id"]
    rec = _get(device, f"/api/midl/records/{src_id}").json()
    doc = copy.deepcopy(rec["doc"])
    doc["screens"] = doc["screens"][:1]
    doc["screens"][0]["id"] = "smoke"
    try:
        r = _req(device, "PUT", "/api/midl/records/smoke",
                 json.dumps({"title": "Smoke", "doc": doc}))
        assert r.status_code == 200, f"{r.status_code} {r.text}"
        out = r.json()
        assert out["ok"] and out["source"] == "local" and out["screens"] == ["smoke"], out
        got = _get(device, "/api/midl/records/smoke").json()
        assert got["source"] == "local" and got["doc"]["screens"][0]["id"] == "smoke"
        # A second record claiming the same screen id is a 409.
        r2 = _req(device, "PUT", "/api/midl/records/smoke2",
                  json.dumps({"title": "Smoke 2", "doc": doc}))
        assert r2.status_code == 409, f"{r2.status_code} {r2.text}"
        assert r2.json()["error"] == "screen_id_conflict"
    finally:
        _req(device, "DELETE", "/api/midl/records/smoke")
    r = requests.get(f"{device.base}/api/midl/records/smoke", timeout=10,
                     **({"auth": device.auth} if device.auth else {}))
    assert r.status_code == 404


def test_midl_record_rejects_garbage(device):
    """A malformed PUT body must not crash the device; expect a 4xx and the
    device should still be reachable after."""
    r = _req(device, "PUT", "/api/midl/records/smoke", "not valid json {[}")
    assert 400 <= r.status_code < 500, f"expected 4xx, got {r.status_code}"
    _get(device, "/api/state")
