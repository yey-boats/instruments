"""Discovery helpers for espdisp system tests.

The primary path is mDNS service discovery for `_yeyboats._tcp.local.` as
advertised by the firmware. Explicit hosts remain supported for lab setups and
CI, and optional CIDR probing covers networks where mDNS multicast is blocked.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import os
import socket
import time
from dataclasses import asdict, dataclass
from typing import Iterable
from urllib.parse import urlparse

import requests


@dataclass(frozen=True)
class DiscoveredDevice:
    host: str
    port: int = 80
    source: str = "explicit"
    device_id: str | None = None
    name: str | None = None
    address: str | None = None
    path: str = "/"
    auth_required: bool = False

    @property
    def base_url(self) -> str:
        suffix = "" if self.port == 80 else f":{self.port}"
        return f"http://{self.host}{suffix}"

    @property
    def pytest_id(self) -> str:
        label = self.device_id or self.name or self.host
        return label.replace("/", "_")


def split_device_specs(value: str | None) -> list[str]:
    if not value:
        return []
    normalized = value.replace(",", " ").replace("\n", " ")
    return [part.strip() for part in normalized.split(" ") if part.strip()]


def parse_device_spec(spec: str) -> DiscoveredDevice:
    raw = spec.strip()
    if not raw:
        raise ValueError("empty device spec")
    parsed = urlparse(raw if "://" in raw else f"http://{raw}")
    host = parsed.hostname
    if not host:
        raise ValueError(f"invalid device spec: {spec!r}")
    return DiscoveredDevice(
        host=host,
        port=parsed.port or 80,
        source="explicit",
        path=parsed.path or "/",
    )


def explicit_devices(specs: Iterable[str]) -> list[DiscoveredDevice]:
    return [parse_device_spec(spec) for spec in specs if spec.strip()]


def probe_device(
    host: str,
    port: int = 80,
    *,
    auth: tuple[str, str] | None = None,
    timeout: float = 1.5,
    source: str = "probe",
) -> DiscoveredDevice | None:
    suffix = "" if port == 80 else f":{port}"
    url = f"http://{host}{suffix}/api/state"
    try:
        response = requests.get(url, timeout=timeout, auth=auth)
    except requests.RequestException:
        return None
    if response.status_code == 401:
        return DiscoveredDevice(host=host, port=port, source=source,
                                address=host, auth_required=True)
    if response.status_code != 200:
        return None
    try:
        body = response.json()
    except ValueError:
        return None
    device = body.get("device") or {}
    if not isinstance(device, dict) or "id" not in device:
        return None
    return DiscoveredDevice(
        host=host,
        port=port,
        source=source,
        device_id=device.get("id"),
        name=device.get("id"),
        address=host,
        auth_required=bool((body.get("webAuth") or {}).get("enabled")),
    )


def device_from_announcement(body: dict, address: str) -> DiscoveredDevice | None:
    if body.get("protocol") != "espdisp.device.announce.v1":
        return None
    device = body.get("device") if isinstance(body.get("device"), dict) else {}
    firmware = body.get("firmware") if isinstance(body.get("firmware"), dict) else {}
    display = body.get("display") if isinstance(body.get("display"), dict) else {}
    device_id = body.get("deviceId") or device.get("id")
    host = body.get("address") or address
    if not device_id or not host:
        return None
    return DiscoveredDevice(
        host=str(host),
        port=int(body.get("port") or 80),
        source="udp",
        device_id=str(device_id),
        name=str(body.get("name") or device_id),
        address=address,
        path=str(body.get("path") or "/"),
        auth_required=bool(body.get("authRequired")),
    )


def listen_udp_announcements(
    *,
    port: int = 34301,
    timeout: float = 5.0,
    bind: str = "",
) -> list[DiscoveredDevice]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind, port))
    sock.settimeout(0.25)
    devices: list[DiscoveredDevice] = []
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            try:
                packet, remote = sock.recvfrom(2048)
            except socket.timeout:
                continue
            try:
                body = json.loads(packet.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            device = device_from_announcement(body, remote[0])
            if device:
                devices.append(device)
    finally:
        sock.close()
    return dedupe_devices(devices)


def discover_mdns(timeout: float = 2.5) -> list[DiscoveredDevice]:
    try:
        from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
    except ImportError:
        return []

    devices: list[DiscoveredDevice] = []

    class Listener(ServiceListener):
        def add_service(self, zeroconf: Zeroconf, service_type: str,
                        name: str) -> None:
            self.update_service(zeroconf, service_type, name)

        def update_service(self, zeroconf: Zeroconf, service_type: str,
                           name: str) -> None:
            info = zeroconf.get_service_info(service_type, name, timeout=1000)
            if not info:
                return
            props = {
                key.decode(errors="replace"): value.decode(errors="replace")
                for key, value in info.properties.items()
            }
            addresses = [
                socket.inet_ntoa(addr)
                for addr in info.addresses
                if len(addr) == 4
            ]
            host = addresses[0] if addresses else info.server.rstrip(".")
            devices.append(DiscoveredDevice(
                host=host,
                port=info.port or 80,
                source="mdns",
                device_id=props.get("device_id"),
                name=name.rstrip("."),
                address=host,
                path=props.get("path") or "/",
            ))

        def remove_service(self, zeroconf: Zeroconf, service_type: str,
                           name: str) -> None:
            return None

    zeroconf = Zeroconf()
    try:
        ServiceBrowser(zeroconf, "_yeyboats._tcp.local.", Listener())
        time.sleep(timeout)
    finally:
        zeroconf.close()
    return devices


def scan_cidrs(
    cidrs: Iterable[str],
    *,
    auth: tuple[str, str] | None = None,
    timeout: float = 0.5,
    workers: int = 64,
) -> list[DiscoveredDevice]:
    hosts: list[str] = []
    for cidr in cidrs:
        network = ipaddress.ip_network(cidr, strict=False)
        hosts.extend(str(host) for host in network.hosts())
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [
            pool.submit(probe_device, host, auth=auth, timeout=timeout,
                        source="scan")
            for host in hosts
        ]
        return [device for device in
                (future.result() for future in futures) if device]


def dedupe_devices(devices: Iterable[DiscoveredDevice]) -> list[DiscoveredDevice]:
    seen: set[tuple] = set()
    out: list[DiscoveredDevice] = []
    for device in devices:
        key = ("id", device.device_id) if device.device_id else (
            "address", device.host, device.port)
        if key in seen:
            continue
        seen.add(key)
        out.append(device)
    return out


def discover_devices(
    *,
    explicit: Iterable[str] = (),
    mdns: bool = True,
    cidrs: Iterable[str] = (),
    udp_listen: bool = False,
    udp_timeout: float = 5.0,
    auth: tuple[str, str] | None = None,
    mdns_timeout: float = 2.5,
    probe_timeout: float = 0.8,
) -> list[DiscoveredDevice]:
    devices: list[DiscoveredDevice] = []
    explicit_targets = explicit_devices(explicit)
    for target in explicit_targets:
        probed = probe_device(target.host, target.port, auth=auth,
                              timeout=probe_timeout, source=target.source)
        devices.append(probed or target)
    if mdns:
        devices.extend(discover_mdns(timeout=mdns_timeout))
    if udp_listen:
        devices.extend(listen_udp_announcements(timeout=udp_timeout))
    if cidrs:
        devices.extend(scan_cidrs(cidrs, auth=auth, timeout=probe_timeout))
    return dedupe_devices(devices)


def env_auth() -> tuple[str, str] | None:
    username = os.environ.get("YEYBOATS_WEB_USERNAME")
    password = os.environ.get("YEYBOATS_WEB_PASSWORD")
    if username and password:
        return username, password
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Discover espdisp devices")
    parser.add_argument("--device", action="append", default=[],
                        help="Explicit host, host:port, or URL to include")
    parser.add_argument("--devices", default=os.environ.get("YEYBOATS_DEVICES"),
                        help="Comma/space-separated explicit device list")
    parser.add_argument("--no-mdns", action="store_true",
                        help="Disable _yeyboats._tcp.local mDNS discovery")
    parser.add_argument("--scan-cidr", action="append", default=[],
                        help="CIDR to actively probe, e.g. 192.168.1.0/24")
    parser.add_argument("--listen-udp", action="store_true",
                        help="Listen for espdisp.device.announce.v1 UDP packets")
    parser.add_argument("--udp-timeout", type=float, default=5.0,
                        help="UDP announcement listen timeout in seconds")
    parser.add_argument("--timeout", type=float, default=2.5,
                        help="mDNS discovery timeout in seconds")
    parser.add_argument("--json", action="store_true",
                        help="Emit JSON instead of one URL per line")
    args = parser.parse_args(argv)

    explicit = list(args.device)
    explicit.extend(split_device_specs(args.devices))
    explicit.extend(split_device_specs(os.environ.get("YEYBOATS_HOST")))
    devices = discover_devices(
        explicit=explicit,
        mdns=not args.no_mdns,
        cidrs=args.scan_cidr or split_device_specs(
            os.environ.get("YEYBOATS_DISCOVERY_CIDRS")),
        udp_listen=args.listen_udp or
        os.environ.get("YEYBOATS_DISCOVERY_UDP") == "1",
        udp_timeout=args.udp_timeout,
        auth=env_auth(),
        mdns_timeout=args.timeout,
    )
    if args.json:
        print(json.dumps([asdict(device) for device in devices], indent=2))
    else:
        for device in devices:
            label = f" {device.device_id}" if device.device_id else ""
            print(f"{device.base_url}{label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
