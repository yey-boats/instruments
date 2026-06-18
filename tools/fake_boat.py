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


# --- own-vessel state ---

def _wrap_pi(a):
    """Wrap angle to (-pi, pi]."""
    return (a + math.pi) % (2 * math.pi) - math.pi


class BoatState:
    """Integrates own position from a desired course each tick. SOG/heading set
    by the caller (driven by the route when active, else a gentle wander)."""

    TURN_RATE = math.radians(10.0)  # max rad/s the heading slews toward desired

    def __init__(self, lat, lon, cog=0.0):
        self.lat = lat
        self.lon = lon
        self.cog = cog
        self.sog = 0.0

    def step(self, dt, desired_cog, sog):
        # Slew cog toward desired_cog, capped by TURN_RATE.
        err = _wrap_pi(desired_cog - self.cog)
        max_step = self.TURN_RATE * dt
        self.cog = (self.cog + max(-max_step, min(max_step, err))) % (2 * math.pi)
        self.sog = sog
        self.lat, self.lon = dead_reckon(self.lat, self.lon, self.cog, sog, dt)


# --- route ---

class Route:
    """Ordered waypoints. Derives track/waypoint nav relative to the boat's
    moving position. Advances to the next waypoint on arrival."""

    ARRIVAL_M = 60.0  # metres; within this of the active waypoint -> advance

    def __init__(self, waypoints):
        self.waypoints = list(waypoints)
        self.index = 1  # active waypoint; leg is waypoints[index-1] -> waypoints[index]
        self.finished = False

    def active(self):
        return self.waypoints[self.index]

    def nav(self, pos, cog, sog):
        """Return dict {xte, cts, btw, dtw, vmg} for the active leg, advancing
        the waypoint index if we have arrived. Returns None when finished."""
        if self.finished:
            return None
        leg_start = self.waypoints[self.index - 1]
        wp = self.waypoints[self.index]
        btw = bearing(pos, wp)
        dtw = distance(pos, wp)
        cts = bearing(leg_start, wp)  # course to steer = the leg bearing
        xte = cross_track(leg_start, wp, pos)
        vmg = sog * math.cos(_wrap_pi(btw - cog))
        result = {"xte": xte, "cts": cts, "btw": btw, "dtw": dtw, "vmg": vmg}
        if dtw < self.ARRIVAL_M:
            if self.index + 1 < len(self.waypoints):
                self.index += 1
            else:
                self.finished = True
        return result


# --- AIS ---

_SHIP_TYPES = [
    (30, "Fishing"), (36, "Sailing"), (60, "Passenger"),
    (70, "Cargo"), (80, "Tanker"), (52, "Tug"),
]
_NAMES = ["MARINA", "ORCA", "SEA BREEZE", "NORDKAP", "ALBATROS", "POSEIDON"]


class AisVessel:
    def __init__(self, slot, center):
        self.slot = slot
        mmsi = 244060800 + slot
        self.context = "vessels.urn:mrn:imo:mmsi:%d" % mmsi
        self.mmsi = mmsi
        self.name = _NAMES[slot % len(_NAMES)]
        self.ship_type = _SHIP_TYPES[slot % len(_SHIP_TYPES)]
        # Place around the center on a ~1.5 NM ring; spread by slot.
        ang = (slot / 6.0) * 2 * math.pi
        self.lat, self.lon = dead_reckon(center[0], center[1], ang, 2800.0, 1.0)
        self.sog = 3.0 + (slot % 4)  # 3..6 m/s
        # Slot 0 converges on the center (CPA); others run roughly tangential.
        if slot == 0:
            self.cog = bearing((self.lat, self.lon), center)
        else:
            self.cog = (ang + math.pi / 2) % (2 * math.pi)
        self.heading = self.cog

    def step(self, dt):
        self.lat, self.lon = dead_reckon(self.lat, self.lon, self.cog, self.sog, dt)


class AisFleet:
    def __init__(self, center):
        self.center = center
        self.vessels = []

    def set_count(self, n):
        n = max(0, n)
        while len(self.vessels) < n:
            self.vessels.append(AisVessel(len(self.vessels), self.center))
        while len(self.vessels) > n:
            self.vessels.pop()

    def step(self, dt):
        for v in self.vessels:
            v.step(dt)

    def nearest(self, pos):
        if not self.vessels:
            return None
        return min(self.vessels, key=lambda v: distance(pos, (v.lat, v.lon)))


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
