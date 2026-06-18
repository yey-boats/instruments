#!/usr/bin/env python3
"""Push synthetic boat data into a local SignalK server so the dashboard has
something to draw. Connects as a WebSocket provider client and sends deltas
every second.

Beyond basic instruments it exercises the firmware's usually-empty paths
(route/waypoint, autopilot, polar targets, depth-below-keel), emits AIS vessel
contexts, and publishes a self-context bearing-to-nearest-target path that a
compass marker ring can bind to. Usually-empty states cycle populated<->cleared.

Usage: python3 fake_boat.py [host] [port] [--dump] [--steady] [--ais N]
  --dump    print deltas as JSON lines to stdout (no connection)
  --steady  freeze phases fully populated (for screenshots / soak)
  --ais N   number of AIS targets (default 4)
"""
import argparse
import asyncio
import json
import math
import time
import urllib.request


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="SignalK boat-data simulator")
    p.add_argument("host", nargs="?", default="localhost")
    p.add_argument("port", nargs="?", type=int, default=3000)
    p.add_argument("--dump", action="store_true",
                   help="print deltas as JSON lines instead of connecting")
    p.add_argument("--steady", action="store_true",
                   help="freeze phases fully populated")
    p.add_argument("--ais", type=int, default=4, help="number of AIS targets")
    return p.parse_args(argv)


async def run(args):  # replaced in Task 8
    raise NotImplementedError


def main(argv=None):
    args = parse_args(argv)
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
