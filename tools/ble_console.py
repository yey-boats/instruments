#!/usr/bin/env python3
"""Connect to the ESP's Nordic UART, stream log notifications,
   and optionally send commands. macOS may prompt for BT permission
   on first run (give Terminal/iTerm access in System Settings)."""
import asyncio
import sys
from bleak import BleakClient, BleakScanner

NUS_SERVICE = "6e400001-b5a3-f393-e0a3-9f4dd9e3a05a"
NUS_RX      = "6e400002-b5a3-f393-e0a3-9f4dd9e3a05a"  # write commands here
NUS_TX      = "6e400003-b5a3-f393-e0a3-9f4dd9e3a05a"  # notifications

NAME = "espdisp"


async def main():
    print(f"scanning for '{NAME}'...")
    device = await BleakScanner.find_device_by_name(NAME, timeout=10)
    if not device:
        print("not found")
        sys.exit(1)
    print(f"found {device.name}  addr={device.address}")
    async with BleakClient(device) as client:
        def on_notify(_, data: bytearray):
            try:
                print(data.decode("utf-8", "replace").rstrip())
            except Exception:
                print(repr(data))
        await client.start_notify(NUS_TX, on_notify)
        # Send any commands passed as args, then stream notifications
        for cmd in sys.argv[1:]:
            print(f"-> {cmd}")
            await client.write_gatt_char(NUS_RX, cmd.encode(), response=False)
            await asyncio.sleep(0.2)
        if not sys.argv[1:]:
            # Default: ask for status
            for cmd in ("ip", "sk-status"):
                print(f"-> {cmd}")
                await client.write_gatt_char(NUS_RX, cmd.encode(), response=False)
                await asyncio.sleep(0.3)
        # Stream for a bit
        await asyncio.sleep(20)


asyncio.run(main())
