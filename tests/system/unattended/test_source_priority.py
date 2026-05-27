"""Source priority transitions:

  1. SignalK alone -> field.source = signalk
  2. Add NMEA-WiFi feed for the same field -> source flips to nmea-wifi
  3. Stop NMEA-WiFi feed and wait past nmea_wifi_ms -> source falls
     back to signalk

Skips automatically if the SignalK demo server isn't reachable.
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


pytestmark = pytest.mark.skipif(
    not _sk_available(), reason=f"SignalK demo not reachable at {SK_HOST}:{SK_PORT}",
)


@pytest.fixture(autouse=True)
def _setup(device):
    device.post_cmd("boat reset")
    device.post_cmd(f"nmea-wifi udp {UDP_PORT}")
    # Tighten the nmea-wifi freshness window so the fallback test is fast.
    device.post_cmd("boat timeout wifi 1500")
    time.sleep(2.0)
    yield
    device.post_cmd("nmea-wifi disable")
    device.post_cmd("boat timeout wifi 3000")


def _udp(*sents: str) -> None:
    nmea0183.send_udp("255.255.255.255", UDP_PORT, sents)


def test_signalk_then_nmea_wins_then_falls_back(device):
    # Phase 1: only SignalK
    sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", 3.0)
    device.wait_for_field("sog_mps", "signalk", timeout_s=8)

    # Phase 2: layer NMEA-WiFi - higher priority, should take over
    for _ in range(5):
        _udp(nmea0183.rmc(sog_kn=10.0, cog_deg=90.0))
        time.sleep(0.2)
    f = device.wait_for_field("sog_mps", "nmea-wifi", timeout_s=6)
    assert abs(f["value"] - 5.144) < 0.1  # 10 kn

    # Phase 3: stop NMEA, wait past freshness -> SignalK reclaims
    # We keep SK fresh by re-publishing it during the wait.
    deadline = time.time() + 6.0
    while time.time() < deadline:
        sk_pump.send(SK_HOST, SK_PORT, "navigation.speedOverGround", 3.0)
        b = device.boat()
        f = b["fields"]["sog_mps"]
        if f["source"] == "signalk":
            assert abs(f["value"] - 3.0) < 0.1
            return
        time.sleep(1.0)
    pytest.fail(f"SignalK never reclaimed sog_mps; final={f}")
