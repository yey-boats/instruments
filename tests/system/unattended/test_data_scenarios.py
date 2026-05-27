"""Data scenarios: no data, single source, conflicting sources,
stale-then-recovery. Asserts /api/boat reflects the right source per
field and that the dashboard screenshot for each scenario lands cleanly.

The console fixture is NOT required for these tests - all data is
pushed over network paths (SignalK WebSocket, NMEA0183 UDP).
"""
import os
import socket
import time

import pytest

from tests.system.inject import nmea0183, sk_pump

SK_HOST = os.environ.get("ESPDISP_SK_HOST", "localhost")
SK_PORT = int(os.environ.get("ESPDISP_SK_PORT", "3000"))
UDP_PORT = int(os.environ.get("ESPDISP_NMEA_WIFI_PORT", "10110"))


def _sk_available() -> bool:
    try:
        with socket.create_connection((SK_HOST, SK_PORT), timeout=1):
            return True
    except OSError:
        return False


SK_REQUIRED = pytest.mark.skipif(
    not _sk_available(), reason=f"SignalK demo not at {SK_HOST}:{SK_PORT}")


def _fake_boat_running() -> bool:
    """Heuristic: is fake_boat.py actively publishing nav data right now?"""
    import urllib.request, json
    try:
        with urllib.request.urlopen(
                f"http://{SK_HOST}:{SK_PORT}/signalk/v1/api/vessels/self/"
                "navigation/speedOverGround/value", timeout=2) as r:
            v = json.loads(r.read())
            return isinstance(v, (int, float))
    except Exception:
        return False


CLEAN_SK_REQUIRED = pytest.mark.skipif(
    _fake_boat_running(),
    reason="fake_boat.py is publishing - stop it first (kill -HUP $(pgrep fake_boat))")


@pytest.fixture(autouse=True)
def _reset(device):
    device.post_cmd("boat reset")
    time.sleep(0.3)


def _udp(*sents):
    nmea0183.send_udp("255.255.255.255", UDP_PORT, sents)


def _ensure_nmea_wifi_enabled(device):
    device.post_cmd(f"nmea-wifi udp {UDP_PORT}")
    time.sleep(1.5)


# --- Scenario A: no data --------------------------------------------

@CLEAN_SK_REQUIRED
def test_no_data_screen_shows_placeholders(device):
    device.show_screen("dashboard")
    time.sleep(1.0)
    b = device.boat()
    for f in ("sog_mps", "depth_m", "awa_rad", "heading_true_rad"):
        assert b["fields"][f].get("source", "none") == "none"
    device.screenshot("scenario_no_data_dashboard")


# --- Scenario B: SignalK only ---------------------------------------

@SK_REQUIRED
def test_signalk_only_drives_fields(device):
    for path, val in [
        ("navigation.speedOverGround", 4.2),
        ("environment.depth.belowTransducer", 9.1),
        ("environment.wind.angleApparent", 0.785),
        ("environment.wind.speedApparent", 5.0),
    ]:
        sk_pump.send(SK_HOST, SK_PORT, path, val)
    time.sleep(2.0)
    b = device.boat()
    for f in ("sog_mps", "depth_m", "awa_rad", "aws_mps"):
        assert b["fields"][f].get("source") == "signalk", (f, b["fields"][f])
    device.screenshot("scenario_signalk_only_dashboard")


# --- Scenario C: conflicting sources --------------------------------

@SK_REQUIRED
def test_conflicting_sources_higher_priority_wins(device):
    _ensure_nmea_wifi_enabled(device)
    # SK pushes 4 kn = 2.05 m/s
    sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", 2.05)
    # NMEA-WiFi (higher priority) pushes 10 kn = 5.14 m/s
    for _ in range(5):
        _udp(nmea0183.rmc(sog_kn=10.0, cog_deg=90.0))
        time.sleep(0.2)
    f = device.wait_for_field("sog_mps", "nmea-wifi", timeout_s=6)
    assert abs(f["value"] - 5.14) < 0.1, f
    device.screenshot("scenario_conflict_nmea_wins")


# --- Scenario D: stale higher-priority -> SK reclaims ---------------

@SK_REQUIRED
def test_higher_priority_stale_then_signalk_reclaims(device):
    _ensure_nmea_wifi_enabled(device)
    device.post_cmd("boat timeout wifi 1500")
    sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", 2.05)
    for _ in range(5):
        _udp(nmea0183.rmc(sog_kn=12.0, cog_deg=180.0))
        time.sleep(0.2)
    device.wait_for_field("sog_mps", "nmea-wifi", timeout_s=6)
    # Stop NMEA injection; keep SK fresh
    deadline = time.time() + 8.0
    reclaimed = False
    while time.time() < deadline:
        sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", 2.05)
        time.sleep(0.5)
        f = device.boat()["fields"]["sog_mps"]
        if f["source"] == "signalk":
            reclaimed = True
            break
    device.post_cmd("boat timeout wifi 3000")
    assert reclaimed, "SignalK never reclaimed sog_mps"


# --- Scenario E: data drops out -> field goes stale -----------------

@SK_REQUIRED
@CLEAN_SK_REQUIRED
def test_field_marked_stale_after_timeout(device):
    sk_pump.send(SK_HOST, SK_PORT, "environment.depth.belowTransducer", 5.0)
    f = device.wait_for_field("depth_m", "signalk", timeout_s=6)
    assert f["fresh"] is True
    # Wait past the signalk default timeout (10 s) without further updates.
    # Speed it up by tightening the SK timeout to 1.5 s.
    device.post_cmd("boat timeout sk 1500")
    time.sleep(2.5)
    f2 = device.boat()["fields"]["depth_m"]
    assert f2["fresh"] is False, f"expected stale, got {f2}"
    device.post_cmd("boat timeout sk 10000")


# --- Scenario F: mixed - SK provides depth, NMEA provides wind -----

@SK_REQUIRED
def test_per_field_routing_independent(device):
    _ensure_nmea_wifi_enabled(device)
    sk_pump.send(SK_HOST, SK_PORT, "environment.depth.belowTransducer", 7.7)
    for _ in range(5):
        _udp(nmea0183.mwv_apparent(awa_deg=60.0, aws_kn=12.0))
        time.sleep(0.2)
    device.wait_for_field("depth_m", "signalk", timeout_s=6)
    device.wait_for_field("awa_rad", "nmea-wifi", timeout_s=6)
    b = device.boat()
    assert b["fields"]["depth_m"]["source"] == "signalk"
    assert b["fields"]["awa_rad"]["source"] == "nmea-wifi"
