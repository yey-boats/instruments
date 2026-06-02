#!/usr/bin/env python3
"""Discover an espdisp MFD on the local network or over BLE.

Resolution order (each step capped by --timeout, default 5 s):

  1. mDNS `_espdisp._tcp` - the firmware's own service advertisement.
     Carries IP + TXT records (device_id, board, firmware, version).
  2. mDNS `_arduino._tcp` - ArduinoOTA's service. Carries IP + the
     OTA port (3232 by default).
  3. BLE NUS - scan for advertisers whose name starts with `espdisp`,
     connect, send `ip`, parse the `ip=<x.x.x.x>` line from the log
     stream (the device echoes log lines over NUS notifications).

Output:
  - Default: prints `<ip>` on stdout (one line, no trailing data) so
    callers can do `IP=$(tools/discover_device.py)`.
  - `--json`: prints `{"ip","name","port","source","txt"}` on stdout.

Filtering & ambiguity:
  - `--name <id>` exact-matches (case-insensitive) the mDNS instance
    name, the mDNS `device_id` TXT field, or the BLE advertised name.
    This is the safe default - it prevents a spoofed `espdisp-evil`
    responder from being picked when you only meant your own device.
  - `--name-contains <substr>` re-enables loose substring matching for
    interactive use; never use it in flash automation.
  - If more than one device matches (or more than one is visible and
    no filter is given), the script exits non-zero and lists the
    candidates instead of silently picking the first responder.
  - `--method auto|mdns|ble` skips the other transport.

Security note:
  mDNS and the BLE NUS `ip` reply are both unauthenticated, so any
  host on the same L2 segment (or within BLE range) can advertise an
  `espdisp` service or BLE name and steer this script at an attacker
  IP. If `make ota` is then run, `firmware.bin` - including any WiFi
  PSK baked into secrets.h - is uploaded to that IP. `OTA_PASSWORD`
  in secrets.h is the only authentication on the OTA channel; leaving
  it empty means anyone who wins a discovery spoof captures the
  firmware blob. Pin `DEVICE_IP=<addr>` for production lab use, or
  set `OTA_PASSWORD` and verify the device with `--name <device_id>`.

Exit codes:
  0 success, 1 not found, 2 ambiguous (multiple candidates).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Optional


@dataclass
class Device:
    ip: str
    name: str = ""
    port: int = 0
    source: str = ""
    txt: dict = field(default_factory=dict)


def _name_matches(device: Device, needle: Optional[str], exact: bool) -> bool:
    """True if `device` matches `needle` against name or device_id TXT.

    `exact=True`  -> case-insensitive equality (safe default).
    `exact=False` -> substring containment (only for --name-contains).
    No filter (`needle is None`) always matches.
    """
    if not needle:
        return True
    needle_lc = needle.lower()
    haystack = [(device.name or "").lower(),
                (device.txt.get("device_id", "") or "").lower()]
    if exact:
        return any(h == needle_lc for h in haystack)
    return any(needle_lc in h for h in haystack)


def discover_mdns(service: str, timeout: float) -> list[Device]:
    """Browse `_<service>._tcp.local.` for the full `timeout` and return
    every responder. Caller is responsible for filtering and
    ambiguity handling - this function never picks a winner."""
    try:
        from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
    except ImportError:
        print("zeroconf not installed; skipping mDNS (pip install zeroconf)",
              file=sys.stderr)
        return []

    found: list[Device] = []
    seen_keys: set[tuple[str, str]] = set()

    class Listener(ServiceListener):
        def add_service(self, zc: "Zeroconf", typ: str, name: str) -> None:
            info = zc.get_service_info(typ, name, timeout=int(timeout * 1000))
            if not info:
                return
            addrs = info.parsed_scoped_addresses() or info.parsed_addresses()
            ipv4 = next((a for a in addrs if ":" not in a), None)
            if not ipv4:
                return
            txt: dict[str, str] = {}
            for k, v in (info.properties or {}).items():
                try:
                    txt[k.decode() if isinstance(k, bytes) else k] = (
                        v.decode() if isinstance(v, bytes) else v
                    )
                except UnicodeDecodeError:
                    continue
            instance = name.split(f"._{service}._tcp.")[0]
            key = (instance, ipv4)
            if key in seen_keys:
                return
            seen_keys.add(key)
            found.append(
                Device(ip=ipv4, name=instance, port=info.port or 0,
                       source=f"mdns:{service}", txt=txt))

        def update_service(self, *a, **kw):  # noqa: D401 - zeroconf API
            pass

        def remove_service(self, *a, **kw):  # noqa: D401 - zeroconf API
            pass

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, f"_{service}._tcp.local.", Listener())
        # Always wait the full window so a second, slower responder
        # can't be missed; ambiguity must be visible to the caller.
        time.sleep(timeout)
    finally:
        zc.close()
    return found


async def discover_ble(timeout: float) -> list[Device]:
    """Scan for `espdisp*` BLE advertisers for `timeout`, then query
    each over NUS for its IP. Returns every device that reported a
    valid IPv4. No filtering - caller decides which to use."""
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("bleak not installed; skipping BLE (pip install bleak)",
              file=sys.stderr)
        return []

    NUS_RX = "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"
    NUS_TX = "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"
    ip_re = re.compile(r"ip=(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")

    devices = await BleakScanner.discover(timeout=timeout)
    candidates = [d for d in devices
                  if (d.name or "").lower().startswith("espdisp")]
    out: list[Device] = []
    for d in candidates:
        ip_holder: dict[str, str] = {}
        try:
            async with BleakClient(d) as client:
                got = asyncio.Event()

                def on_notify(_: int, data: bytearray) -> None:
                    text = data.decode("utf-8", "replace")
                    m = ip_re.search(text)
                    if m and m.group(1) != "0.0.0.0":
                        ip_holder["ip"] = m.group(1)
                        got.set()

                await client.start_notify(NUS_TX, on_notify)
                await client.write_gatt_char(NUS_RX, b"ip", response=False)
                try:
                    await asyncio.wait_for(got.wait(), timeout=timeout)
                except asyncio.TimeoutError:
                    continue
                out.append(Device(ip=ip_holder["ip"], name=d.name or "",
                                  source="ble"))
        except Exception as exc:  # noqa: BLE001 - best-effort scan
            print(f"ble: {d.name} ({d.address}): {exc}", file=sys.stderr)
            continue
    return out


def _filter(devs: list[Device], needle: Optional[str], exact: bool) -> list[Device]:
    return [d for d in devs if _name_matches(d, needle, exact)]


def _format_candidates(devs: list[Device]) -> str:
    lines = []
    for d in devs:
        did = d.txt.get("device_id", "")
        lines.append(f"  - {d.source:14s}  ip={d.ip:<15s}  name={d.name!r}"
                     + (f"  device_id={did!r}" if did else ""))
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--method", choices=("auto", "mdns", "ble"), default="auto",
                    help="Discovery transport (default: auto - mDNS then BLE).")
    ap.add_argument("--name", default=None,
                    help="Exact-match (case-insensitive) device name or "
                         "device_id. Safe default for flash automation.")
    ap.add_argument("--name-contains", default=None,
                    help="Substring match instead of exact. Interactive use "
                         "only - susceptible to spoofed names.")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="Per-method timeout in seconds (default: 5).")
    ap.add_argument("--json", action="store_true",
                    help="Emit full JSON record instead of just the IP.")
    args = ap.parse_args()

    if args.name and args.name_contains:
        print("error: --name and --name-contains are mutually exclusive",
              file=sys.stderr)
        return 2
    needle = args.name or args.name_contains
    exact = args.name is not None

    found: list[Device] = []
    if args.method in ("auto", "mdns"):
        for svc in ("espdisp", "arduino"):
            for d in discover_mdns(svc, args.timeout):
                # Dedupe across both services by (ip, name).
                if not any(x.ip == d.ip and x.name == d.name for x in found):
                    found.append(d)
            if args.method == "mdns" and found:
                break  # mdns-only: don't keep scanning if anything responded
    if not found and args.method in ("auto", "ble"):
        found.extend(asyncio.run(discover_ble(args.timeout)))

    matches = _filter(found, needle, exact)
    if not matches:
        if found:
            print(f"no device matches {needle!r}; visible candidates:",
                  file=sys.stderr)
            print(_format_candidates(found), file=sys.stderr)
        else:
            print("no espdisp device found", file=sys.stderr)
        return 1
    if len(matches) > 1:
        print(f"ambiguous: {len(matches)} devices match; pass --name to pin one:",
              file=sys.stderr)
        print(_format_candidates(matches), file=sys.stderr)
        return 2

    dev = matches[0]
    if args.json:
        print(json.dumps(asdict(dev)))
    else:
        print(dev.ip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
