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


# --- geo helpers ---

R_EARTH = 6371000.0  # mean Earth radius, metres


def bearing(a, b):
    """Initial great-circle bearing a->b. Inputs (lat_deg, lon_deg). Radians 0..2pi."""
    lat1, lon1 = math.radians(a[0]), math.radians(a[1])
    lat2, lon2 = math.radians(b[0]), math.radians(b[1])
    dlon = lon2 - lon1
    y = math.sin(dlon) * math.cos(lat2)
    x = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(dlon)
    return math.atan2(y, x) % (2 * math.pi)


def distance(a, b):
    """Great-circle distance in metres (haversine). Inputs (lat_deg, lon_deg)."""
    lat1, lon1 = math.radians(a[0]), math.radians(a[1])
    lat2, lon2 = math.radians(b[0]), math.radians(b[1])
    dlat, dlon = lat2 - lat1, lon2 - lon1
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * R_EARTH * math.asin(min(1.0, math.sqrt(h)))


def dead_reckon(lat, lon, cog_rad, sog_ms, dt):
    """Advance (lat_deg, lon_deg) by dt seconds along cog at sog. Returns (lat_deg, lon_deg)."""
    ang = (sog_ms * dt) / R_EARTH  # angular distance travelled
    lat1, lon1 = math.radians(lat), math.radians(lon)
    lat2 = math.asin(math.sin(lat1) * math.cos(ang)
                     + math.cos(lat1) * math.sin(ang) * math.cos(cog_rad))
    lon2 = lon1 + math.atan2(math.sin(cog_rad) * math.sin(ang) * math.cos(lat1),
                             math.cos(ang) - math.sin(lat1) * math.sin(lat2))
    return (math.degrees(lat2), math.degrees(lon2))


def cross_track(start, end, p):
    """Signed cross-track distance (m) of p from great-circle start->end.
    Positive = p is to the RIGHT (starboard) of the track."""
    d13 = distance(start, p) / R_EARTH  # angular
    brg13 = bearing(start, p)
    brg12 = bearing(start, end)
    return math.asin(math.sin(d13) * math.sin(brg13 - brg12)) * R_EARTH


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
