"""Unit coverage for the system-test discovery helper."""
from __future__ import annotations

from tests.system.discovery import (
    DiscoveredDevice,
    dedupe_devices,
    device_from_announcement,
    parse_device_spec,
    split_device_specs,
)


def test_split_device_specs_accepts_commas_spaces_and_newlines():
    assert split_device_specs("a.local, 192.168.1.20\nb.local") == [
        "a.local",
        "192.168.1.20",
        "b.local",
    ]


def test_parse_device_spec_accepts_host_port_and_urls():
    plain = parse_device_spec("espdisp.local")
    assert plain.host == "espdisp.local"
    assert plain.port == 80

    with_port = parse_device_spec("192.168.1.20:8080")
    assert with_port.host == "192.168.1.20"
    assert with_port.port == 8080

    url = parse_device_spec("http://espdisp-wide.local:8080/")
    assert url.host == "espdisp-wide.local"
    assert url.port == 8080


def test_dedupe_keeps_first_matching_device():
    devices = dedupe_devices([
        DiscoveredDevice(host="192.168.1.20", device_id="espdisp-a",
                         source="explicit"),
        DiscoveredDevice(host="192.168.1.20", device_id="espdisp-a",
                         source="mdns"),
    ])
    assert len(devices) == 1
    assert devices[0].source == "explicit"


def test_udp_announcement_maps_to_discovered_device():
    device = device_from_announcement({
        "protocol": "espdisp.device.announce.v1",
        "deviceId": "espdisp-udp",
        "address": "192.168.1.50",
        "port": 80,
        "path": "/",
        "authRequired": True,
        "device": {"id": "espdisp-udp", "board": "native_fake"},
        "firmware": {"name": "espdisp", "version": "0.5.0"},
        "display": {"width": 480, "height": 480},
    }, "192.168.1.50")
    assert device is not None
    assert device.device_id == "espdisp-udp"
    assert device.source == "udp"
    assert device.auth_required is True


def test_non_espdisp_udp_announcement_is_ignored():
    assert device_from_announcement({"protocol": "other"}, "192.168.1.50") is None
