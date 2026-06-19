#!/usr/bin/env python3
"""One-shot BLE provisioning for a fresh yey-display MFD.

Encodes the BLE dance we used to do by hand:

  1. Optionally forget existing WiFi networks (so the device doesn't
     fall back onto a client-isolated AP it once joined).
  2. Save lab WiFi credentials → device reboots → re-associates.
  3. Pin SignalK host/port (so auto-discovery doesn't latch onto
     whichever responder answers first).
  4. Set the manager endpoint (so heartbeat/register go to the lab
     plugin).

Defaults come from `.env.test` if present at the repo root, so a
clean checkout + a single
    python3 tools/provision_device.py
brings up a fresh device for the current lab setup.

Override anything via flags. Use `--ble-name` if you have multiple
devices visible at once.

Requires: bleak (`pip install bleak`).
"""
from __future__ import annotations

import argparse
import asyncio
import os
import re
import sys
from pathlib import Path
from typing import Iterable

NUS_RX = "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"
NUS_TX = "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"


def load_env_test(repo_root: Path) -> dict:
    """Parse repo-root .env.test in a tolerant way (no shell exec).

    Only picks up plain `export NAME=value` lines or `NAME=value`.
    Quotes around the value are stripped.  Errors are silently
    ignored — this is a best-effort defaults loader.
    """
    out: dict[str, str] = {}
    p = repo_root / ".env.test"
    if not p.exists():
        return out
    pattern = re.compile(r'^(?:export\s+)?([A-Z_][A-Z0-9_]*)=(.*)$')
    # Recognize the shell fallback idiom `${NAME:-default}` so values
    # like `export YEYBOATS_BLE_NAME="${YEYBOATS_BLE_NAME:-yey-d}"` work:
    # we treat the current process env as authoritative, falling back to
    # the literal default the .env.test file declares.
    fallback = re.compile(r'^\$\{([A-Z_][A-Z0-9_]*):-([^}]*)\}$')
    for raw in p.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = pattern.match(line)
        if not m:
            continue
        name, val = m.group(1), m.group(2).strip()
        if (val.startswith('"') and val.endswith('"')) or \
           (val.startswith("'") and val.endswith("'")):
            val = val[1:-1]
        fb = fallback.match(val)
        if fb:
            val = os.environ.get(fb.group(1), fb.group(2))
        elif "$" in val:
            # Unsupported shell-substitution form — skip rather than
            # propagate the literal text, which would confuse callers.
            continue
        out[name] = val
    return out


async def find_device(name: str, timeout: float):
    from bleak import BleakScanner
    devs = await BleakScanner.discover(timeout=timeout)
    for d in devs:
        if d.name == name:
            return d
    return None


async def send_commands(name: str, commands: Iterable[tuple[str, float]],
                        scan_timeout: float) -> int:
    from bleak import BleakClient
    dev = await find_device(name, scan_timeout)
    if not dev:
        print(f"BLE device named {name!r} not advertising", file=sys.stderr)
        return 1
    print(f"connected to {dev.name} @ {dev.address}")

    async with BleakClient(dev) as client:
        rx_lines: list[str] = []

        def on_notify(_, data: bytearray) -> None:
            text = data.decode("utf-8", "replace")
            for line in text.splitlines():
                rx_lines.append(line)
                print(f"  < {line}")

        await client.start_notify(NUS_TX, on_notify)
        for cmd, settle in commands:
            print(f"  > {cmd}")
            await client.write_gatt_char(NUS_RX, cmd.encode(), response=False)
            await asyncio.sleep(settle)
        # Final drain so the reboot log lines are seen before we exit.
        await asyncio.sleep(1.0)

    print("---")
    print(f"sent {sum(1 for _ in commands)} command(s), saw {len(rx_lines)} log line(s)")
    return 0


def build_command_sequence(args, env: dict) -> list[tuple[str, float]]:
    """Build (command, settle-seconds) tuples."""
    cmds: list[tuple[str, float]] = []

    # 1. Forget extras so the device doesn't fall back to them.
    for ssid in args.forget:
        cmds.append((f"wifi-forget {ssid}", 1.0))

    # 2. WiFi credentials.  Quote the SSID if it has spaces.
    if args.wifi_ssid:
        ssid_arg = f'"{args.wifi_ssid}"' if " " in args.wifi_ssid else args.wifi_ssid
        cmds.append((f"wifi {ssid_arg} {args.wifi_pass}", 1.5))
        # wifi <ssid> <pass> triggers a reboot — give the device time
        # to come back up before sending more commands.
        cmds.append(("# (device rebooting; pausing 18s for assoc + DHCP)", 0.0))
        # Sentinel — the leading '#' is not a valid command on the
        # device, so the dispatcher will ignore it.  We hijack the
        # settle time to wait.
        cmds[-1] = ("# wait-for-reboot", 18.0)

    # 3. SignalK target.  Skip if unset.
    if args.sk_host:
        cmds.append((f"sk {args.sk_host} {args.sk_port}", 1.5))
        # `sk` also reboots the device.
        cmds.append(("# wait-for-reboot", 18.0))

    # 4. Manager endpoint.  No reboot here — just settle.
    if args.manager_url:
        cmds.append((f"manager-register {args.manager_url}", 2.0))

    # 5. Final status snapshot.
    cmds.append(("ip", 0.8))
    cmds.append(("sk-status", 0.8))
    cmds.append(("manager-status", 1.0))

    return cmds


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    env = load_env_test(repo_root)

    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--ble-name", default=env.get("YEYBOATS_BLE_NAME", "yey-d"),
                   help="BLE advertised name (default: yey-d)")
    p.add_argument("--wifi-ssid", default=env.get("ESP_LAB_SSID", "esp-lab"),
                   help="WiFi SSID to join (default: esp-lab, or ESP_LAB_SSID)")
    p.add_argument("--wifi-pass",
                   default=os.environ.get("ESP_LAB_PSK",
                                          env.get("ESP_LAB_PSK", "")),
                   help="WiFi password (default: $ESP_LAB_PSK from "
                        "process env or .env.test.local). Required unless "
                        "--skip-wifi.")
    p.add_argument("--sk-host", default=env.get("SK_HOST", "192.168.2.11"),
                   help="SignalK host (default from .env.test SK_HOST)")
    p.add_argument("--sk-port", type=int,
                   default=int(env.get("SK_PORT", "3000")),
                   help="SignalK port (default 3000)")
    p.add_argument("--manager-url",
                   default=(f"http://{env.get('SK_HOST', '192.168.2.11')}:"
                            f"{env.get('SK_PORT', '3000')}"
                            f"/plugins/yey-boats-display-manager"),
                   help="Manager plugin URL")
    p.add_argument("--forget", action="append", default=[],
                   help="SSID(s) to forget first (repeatable). "
                        "Useful when an old client-isolated AP is still saved.")
    p.add_argument("--scan-timeout", type=float, default=10.0,
                   help="BLE scan timeout in seconds (default 10)")
    p.add_argument("--skip-wifi", action="store_true",
                   help="Don't change WiFi credentials (skip step 2)")
    p.add_argument("--skip-sk", action="store_true",
                   help="Don't change SignalK target (skip step 3)")
    p.add_argument("--skip-manager", action="store_true",
                   help="Don't change manager endpoint (skip step 4)")
    args = p.parse_args()

    if args.skip_wifi:
        args.wifi_ssid = ""
    elif not args.wifi_pass:
        print("error: WiFi password required (set ESP_LAB_PSK in env or "
              ".env.test.local, or pass --wifi-pass). Use --skip-wifi to "
              "leave WiFi unchanged.", file=sys.stderr)
        return 64
    if args.skip_sk:
        args.sk_host = ""
    if args.skip_manager:
        args.manager_url = ""

    cmds = build_command_sequence(args, env)
    print("Plan:")
    for cmd, settle in cmds:
        print(f"  - {cmd}   (then wait {settle:.1f}s)")
    print()

    # The '# wait-for-reboot' sentinel is a comment to the device
    # (its command dispatcher ignores anything starting with '#'),
    # so we can interleave them into the same write stream.  But
    # they ARE a no-op send — let's filter them out to avoid wasting
    # GATT writes, while keeping their settle delays.
    def actual_commands():
        for cmd, settle in cmds:
            if cmd.startswith("#"):
                # Sleep without writing.
                yield ("", settle)
            else:
                yield (cmd, settle)

    return asyncio.run(_run_sequence(args.ble_name, list(actual_commands()),
                                     args.scan_timeout))


async def _run_sequence(name: str, commands: list[tuple[str, float]],
                        scan_timeout: float) -> int:
    """Like send_commands but skips empty-string command writes (sleep-only)."""
    from bleak import BleakClient
    dev = await find_device(name, scan_timeout)
    if not dev:
        print(f"BLE device named {name!r} not advertising", file=sys.stderr)
        return 1
    print(f"connected to {dev.name} @ {dev.address}")

    rx_count = 0

    async with BleakClient(dev) as client:
        def on_notify(_, data: bytearray) -> None:
            nonlocal rx_count
            text = data.decode("utf-8", "replace")
            for line in text.splitlines():
                rx_count += 1
                print(f"  < {line}")

        await client.start_notify(NUS_TX, on_notify)
        for cmd, settle in commands:
            if cmd:
                print(f"  > {cmd}")
                try:
                    await client.write_gatt_char(NUS_RX, cmd.encode(),
                                                 response=False)
                except Exception as e:
                    # Device may have rebooted between writes; tolerate.
                    print(f"    (write failed: {e!s}; reconnecting...)")
                    try:
                        await client.disconnect()
                    except Exception:
                        pass
                    dev = await find_device(name, scan_timeout)
                    if not dev:
                        print(f"  ! device {name!r} did not re-advertise",
                              file=sys.stderr)
                        return 2
                    await client.connect()
                    await client.start_notify(NUS_TX, on_notify)
                    await client.write_gatt_char(NUS_RX, cmd.encode(),
                                                 response=False)
            await asyncio.sleep(settle)
        await asyncio.sleep(1.0)

    print("---")
    print(f"done.  saw {rx_count} log line(s) from the device.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
