"""NMEA0183 TCP-client mode: device connects out to a TCP server we
host on the test runner. Verifies the same parser path under a
different transport.

Requires the runner's LAN IP to be reachable from the device. The
test auto-detects it via a UDP-trick socket; override with
ESPDISP_TEST_HOST_IP if you're on a multi-homed machine.
"""
import os
import socket
import time

import pytest

from tests.system.inject import nmea0183


def _runner_ip(device_host: str) -> str:
    override = os.environ.get("ESPDISP_TEST_HOST_IP")
    if override:
        return override
    # Pick the local interface that would be used to reach the device.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        try:
            s.connect((device_host, 1))
        except OSError:
            s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


TCP_PORT = 20183


@pytest.fixture(autouse=True)
def _reset(device):
    device.post_cmd("boat reset")
    time.sleep(0.3)


def test_tcp_client_ingests(device):
    runner_ip = _runner_ip(device.host)
    sents = [nmea0183.rmc(sog_kn=2.0, cog_deg=270.0)] * 30
    feed = nmea0183.TcpFeed(TCP_PORT, sents, hold_open_s=4.0)
    feed.start()
    try:
        device.post_cmd(f"nmea-wifi tcp {runner_ip} {TCP_PORT}")
        # Allow time for the worker to (re)connect + receive sentences.
        f = device.wait_for_field("sog_mps", "nmea-wifi", timeout_s=12)
        # 2 kn = 1.029 m/s
        assert abs(f["value"] - 1.029) < 0.05
    finally:
        feed.stop()
        device.post_cmd("nmea-wifi disable")
