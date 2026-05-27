"""NMEA0183 UDP injection -> /api/boat with source=nmea-wifi.

Enables the device's UDP listener, fires sentences from the host, and
asserts the parsed values land with the right source. UDP path is
preferred over TCP for tests because the device polls UDP without
needing a routable host IP.
"""
import os
import time

import pytest

from tests.system.inject import nmea0183

UDP_PORT = int(os.environ.get("ESPDISP_NMEA_WIFI_PORT", "10110"))


@pytest.fixture(scope="module", autouse=True)
def _enable_udp(device):
    device.post_cmd(f"nmea-wifi udp {UDP_PORT}")
    time.sleep(2.0)  # let the worker bind
    yield
    device.post_cmd("nmea-wifi disable")


@pytest.fixture(autouse=True)
def _reset(device):
    device.post_cmd("boat reset")
    time.sleep(0.3)


def _broadcast(*sents: str) -> None:
    # UDP broadcast to the device. Tests run on the same LAN; use the
    # global broadcast address so we don't need to know the subnet.
    nmea0183.send_udp("255.255.255.255", UDP_PORT, sents)


def test_rmc_sets_sog_cog(device):
    for _ in range(6):
        _broadcast(nmea0183.rmc(sog_kn=5.5, cog_deg=120.0))
        time.sleep(0.2)
    f = device.wait_for_field("sog_mps", "nmea-wifi", timeout_s=6)
    # 5.5 kn = 2.83 m/s
    assert abs(f["value"] - 2.83) < 0.05
    f2 = device.wait_for_field("cog_true_rad", "nmea-wifi", timeout_s=6)
    # 120 deg = 2.094 rad
    assert abs(f2["value"] - 2.094) < 0.02


def test_dpt_sets_depth(device):
    for _ in range(6):
        _broadcast(nmea0183.dpt(8.4))
        time.sleep(0.2)
    f = device.wait_for_field("depth_m", "nmea-wifi", timeout_s=6)
    assert abs(f["value"] - 8.4) < 0.1


def test_vhw_sets_heading_and_stw(device):
    for _ in range(6):
        _broadcast(nmea0183.vhw(heading_deg=87.0, stw_kn=4.1))
        time.sleep(0.2)
    f_hdg = device.wait_for_field("heading_true_rad", "nmea-wifi", timeout_s=6)
    assert abs(f_hdg["value"] - 1.518) < 0.02
    f_stw = device.wait_for_field("stw_mps", "nmea-wifi", timeout_s=6)
    # 4.1 kn = 2.11 m/s
    assert abs(f_stw["value"] - 2.11) < 0.05


def test_mwv_sets_apparent_wind(device):
    for _ in range(6):
        _broadcast(nmea0183.mwv_apparent(awa_deg=30.0, aws_kn=10.0))
        time.sleep(0.2)
    f = device.wait_for_field("awa_rad", "nmea-wifi", timeout_s=6)
    assert abs(f["value"] - 0.524) < 0.02
    device.show_screen("wind")
    time.sleep(1.0)
    device.screenshot("nmea_wifi_wind")


def test_bad_checksum_ignored(device):
    bad = "$IIVHW,090.0,T,090.0,M,5.0,N,,*FF\r\n"
    for _ in range(6):
        nmea0183.send_udp("255.255.255.255", UDP_PORT, [bad])
        time.sleep(0.1)
    b = device.boat()
    assert b["fields"].get("heading_true_rad", {}).get("source") != "nmea-wifi"
