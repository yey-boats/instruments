"""NMEA2000 hardware tests.

Most builds ship with -DENABLE_NMEA2000 *not* set, so the worker is a
sleeping no-op and the only thing we can verify is the status
reporting. The decode test xfails when no transceiver is present.
"""
import os
import time

import pytest


def test_n2k_status_reachable(device):
    b = device.boat()
    s = b["sources"]["nmea2000"]
    assert "compiled_in" in s and "enabled" in s
    assert "frames_rx" in s and "pgns_decoded" in s


def test_n2k_status_disabled_by_default(device):
    b = device.boat()
    s = b["sources"]["nmea2000"]
    # Default config: disabled until user runs `n2k enable`.
    assert s["enabled"] is False


@pytest.mark.xfail(
    reason="NMEA2000 transceiver/wiring not validated on this board",
    strict=False,
)
def test_n2k_decoded_pgns_when_enabled(device):
    """Documents the smoke test that runs once a transceiver is attached.

    Requires:
      - firmware built with -DENABLE_NMEA2000
      - SN65HVD230 (or equivalent) on the configured rx_pin / tx_pin
      - a live NMEA2000 bus emitting at least one of the supported PGNs
        within 5 s

    Marked xfail so it documents the expectation without blocking CI.
    Remove the marker once hardware is wired up.
    """
    b = device.boat()
    s = b["sources"]["nmea2000"]
    if not s["compiled_in"]:
        pytest.xfail("firmware not built with -DENABLE_NMEA2000")
    device.post_cmd("n2k enable")
    time.sleep(5.0)
    s2 = device.boat()["sources"]["nmea2000"]
    assert s2["frames_rx"] > 0, "no CAN frames received - check wiring/baud"
    assert s2["pgns_decoded"] > 0, "frames came in but no PGNs decoded"
