"""mDNS auto-discovery + CLI smoke tests.

The mDNS path is hard to exercise from pytest without standing up a
fake _signalk-ws._tcp record. We instead force one discovery cycle
via the `sk-discover` CLI and confirm the device logs the attempt.

The CLI tests prove every command surface (boat / nmea-wifi / n2k /
board) is wired into net::dispatchCommand.
"""
import time


def test_sk_discover_emits_log(device, udp_logs):
    # Drain any backlog so wait_for matches a fresh line.
    udp_logs.drain()
    device.post_cmd("sk-discover")
    line = udp_logs.wait_for("[sk]", timeout_s=6)
    assert "mDNS" in line or "discover" in line


def test_boat_cli_dumps_snapshot(device, udp_logs):
    udp_logs.drain()
    device.post_cmd("boat snapshot")
    line = udp_logs.wait_for("[boat] snapshot", timeout_s=5)
    assert "snapshot" in line


def test_boat_priority_cli(device, udp_logs):
    udp_logs.drain()
    device.post_cmd("boat priority")
    line = udp_logs.wait_for("[boat] priority", timeout_s=5)
    assert "nmea2000" in line and "nmea-wifi" in line and "signalk" in line


def test_n2k_status_cli(device, udp_logs):
    udp_logs.drain()
    device.post_cmd("n2k status")
    line = udp_logs.wait_for("[n2k]", timeout_s=5)
    assert "compiled=" in line and "enabled=" in line


def test_nmea_wifi_status_cli(device, udp_logs):
    udp_logs.drain()
    device.post_cmd("nmea-wifi status")
    line = udp_logs.wait_for("[n0183w]", timeout_s=5)
    assert "proto=" in line and "port=" in line


def test_board_cli(device, udp_logs):
    udp_logs.drain()
    device.post_cmd("board")
    line = udp_logs.wait_for("[board] id=", timeout_s=5)
    assert "sunton_4848s040" in line
