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
    return math.asin(max(-1.0, min(1.0, math.sin(d13) * math.sin(brg13 - brg12)))) * R_EARTH


# --- own-vessel state ---

def _wrap_pi(a):
    """Wrap angle to [-pi, pi)."""
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

    def bearing_to_active(self, pos):
        """Bearing (rad) to the active waypoint; no state mutation."""
        return bearing(pos, self.waypoints[self.index])

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
    def __init__(self, slot, center, count=6):
        self.slot = slot
        mmsi = 244060800 + slot
        self.context = "vessels.urn:mrn:imo:mmsi:%d" % mmsi
        self.mmsi = mmsi
        self.name = _NAMES[slot % len(_NAMES)]
        self.ship_type = _SHIP_TYPES[slot % len(_SHIP_TYPES)]
        # Place around the center on a ~1.5 NM ring; spread by slot.
        # Divide the full circle by max(len(_SHIP_TYPES), count) so vessels
        # stay distinct even when count > 6.
        ang = (slot / max(len(_SHIP_TYPES), count)) * 2 * math.pi
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
            self.vessels.append(AisVessel(len(self.vessels), self.center, count=n))
        while len(self.vessels) > n:
            self.vessels.pop()

    def step(self, dt):
        for v in self.vessels:
            v.step(dt)

    def nearest(self, pos):
        if not self.vessels:
            return None
        return min(self.vessels, key=lambda v: distance(pos, (v.lat, v.lon)))


# --- phase scheduling ---

class PhaseScheduler:
    """Drives usually-empty states through populated<->cleared phases on slow
    timers. With steady=True everything is pinned fully populated."""

    ROUTE_PERIOD = 180.0  # s: active for first 70%, cleared for the rest
    AP_PERIOD = 150.0     # s: standby -> auto -> wind, equal thirds
    AIS_PERIOD = 240.0    # s: count 0 -> max -> partial -> 0

    def __init__(self, steady=False):
        self.steady = steady

    def route_active(self, t):
        if self.steady:
            return True
        return (t % self.ROUTE_PERIOD) < (0.70 * self.ROUTE_PERIOD)

    def ap_mode(self, t):
        if self.steady:
            return "auto"
        third = self.AP_PERIOD / 3.0
        phase = (t % self.AP_PERIOD) // third
        return {0: "standby", 1: "auto", 2: "wind"}[phase]

    def ais_count(self, t, maxn):
        if self.steady:
            return maxn
        quarter = self.AIS_PERIOD / 4.0
        phase = (t % self.AIS_PERIOD) // quarter
        return {0: 0, 1: maxn, 2: max(0, maxn // 2), 3: 0}[phase]


# --- delta builders ---

BEAT_ANGLE = math.radians(42.0)   # optimal upwind TWA off bow
GYBE_ANGLE = math.radians(150.0)  # optimal downwind TWA off bow
KEEL_DRAFT_M = 1.8                 # transducer-to-keel offset


def _val(path, value):
    return {"path": path, "value": value}


def build_self_delta(pos, sog, cog, heading, awa, aws, twa, tws, depth,
                     water_temp, battv, current_set, current_drift,
                     nav, ap_mode, ap_target, closest, ts):
    values = [
        _val("navigation.position", {"latitude": pos[0], "longitude": pos[1]}),
        _val("navigation.speedOverGround", sog),
        _val("navigation.speedThroughWater", sog * 0.92),
        _val("navigation.courseOverGroundTrue", cog),
        _val("navigation.headingTrue", heading),
        _val("environment.wind.angleApparent", awa),
        _val("environment.wind.speedApparent", aws),
        _val("environment.wind.angleTrueWater", twa),
        _val("environment.wind.speedTrue", tws),
        _val("environment.depth.belowTransducer", depth),
        _val("environment.depth.belowKeel", depth - KEEL_DRAFT_M),
        _val("environment.water.temperature", water_temp),
        _val("environment.current.setTrue", current_set),
        _val("environment.current.drift", current_drift),
        _val("electrical.batteries.house.voltage", battv),
        _val("electrical.batteries.house.stateOfCharge", 0.82),
        _val("tanks.fuel.0.currentLevel", 0.65),
        _val("tanks.freshWater.0.currentLevel", 0.40),
        _val("performance.beatAngle", BEAT_ANGLE),
        _val("performance.gybeAngle", GYBE_ANGLE),
        _val("steering.autopilot.state", ap_mode),
    ]
    if ap_target is not None:
        values.append(_val("steering.autopilot.target.headingTrue", ap_target))
    if nav is not None:
        values += [
            _val("navigation.courseRhumbline.crossTrackError", nav["xte"]),
            _val("navigation.courseRhumbline.bearingTrackTrue", nav["cts"]),
            _val("navigation.courseRhumbline.nextPoint.bearingTrue", nav["btw"]),
            _val("navigation.courseRhumbline.nextPoint.distance", nav["dtw"]),
            _val("navigation.courseRhumbline.velocityMadeGood", nav["vmg"]),
        ]
    if closest is not None:
        values += [
            _val("navigation.closestApproach.bearingTrue", closest["bearing"]),
            _val("navigation.closestApproach.distance", closest["distance"]),
        ]
    return {
        "context": "vessels.self",
        "updates": [{"$source": "fake_boat.py", "timestamp": ts, "values": values}],
    }


def build_ais_delta(v, ts, include_static):
    values = [
        _val("navigation.position", {"latitude": v.lat, "longitude": v.lon}),
        _val("navigation.speedOverGround", v.sog),
        _val("navigation.courseOverGroundTrue", v.cog),
        _val("navigation.headingTrue", v.heading),
    ]
    if include_static:
        values += [
            _val("name", v.name),
            _val("mmsi", str(v.mmsi)),
            _val("design.aisShipType", {"id": v.ship_type[0], "name": v.ship_type[1]}),
        ]
    return {
        "context": v.context,
        "updates": [{"$source": "fake_boat.py", "timestamp": ts, "values": values}],
    }


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


# --- simulator + loop ---

START = (41.3851, 2.1734)  # Barcelona-ish, matches the original sim


def _route_from(start):
    # Three legs trending NE from the start; 400 m each so waypoints advance
    # within the active phase window and the route lifecycle is exercised.
    wps = [start]
    pos, brg = start, math.radians(50.0)
    for _ in range(3):
        pos = dead_reckon(pos[0], pos[1], brg, 400.0, 1.0)
        wps.append(pos)
        brg = (brg + math.radians(20.0)) % (2 * math.pi)
    return Route(wps)


class Simulator:
    def __init__(self, steady=False, ais=4):
        self.phase = PhaseScheduler(steady=steady)
        self.boat = BoatState(lat=START[0], lon=START[1], cog=math.radians(50.0))
        self.fleet = AisFleet(center=START)
        self.route = _route_from(START)
        self.ais_max = ais
        self._static_at = -1e9

    def tick(self, t, dt):
        # Environment sinusoids (unchanged character from the original sim).
        sog = 4.0 + 1.5 * math.sin(t / 30)
        awa = math.radians(35 + 20 * math.sin(t / 12))
        aws = 6.0 + 2 * math.cos(t / 18)
        twa = awa + 0.1
        tws = aws + 1.5
        depth = 12.0 + 4 * math.sin(t / 45)
        water_temp = 273.15 + 19 + math.sin(t / 600)
        battv = 12.7 + 0.2 * math.sin(t / 90)
        current_set = math.radians((210 + 30 * math.sin(t / 120)) % 360)
        current_drift = 0.7 + 0.4 * math.sin(t / 75)

        # Route phase -> derived nav; steer the boat down the leg when active.
        # Use bearing_to_active() (non-mutating) to derive desired_cog, then call
        # nav() exactly once AFTER boat.step() to compute the delta and advance.
        nav = None
        route_on = self.phase.route_active(t) and not self.route.finished
        if route_on:
            btw_pre = self.route.bearing_to_active((self.boat.lat, self.boat.lon))
            desired_cog = btw_pre + math.radians(3.0 * math.sin(t / 20))
        else:
            desired_cog = math.radians((50 + 10 * math.sin(t / 60)) % 360)
        self.boat.step(dt, desired_cog, sog)
        if route_on:
            nav = self.route.nav((self.boat.lat, self.boat.lon), self.boat.cog, sog)

        # Autopilot phase.
        ap_mode = self.phase.ap_mode(t)
        ap_target = None
        if ap_mode == "auto":
            ap_target = self.boat.cog
        elif ap_mode == "wind":
            # Placeholder: populate target.headingTrue (a heading path) so the
            # firmware widget has a value to display; this is NOT a semantically
            # correct wind-angle target — a real AP would use a wind-angle path.
            ap_target = (self.boat.cog - twa) % (2 * math.pi)  # hold current TWA

        # AIS fleet.
        self.fleet.set_count(self.phase.ais_count(t, self.ais_max))
        self.fleet.step(dt)
        closest = None
        near = self.fleet.nearest((self.boat.lat, self.boat.lon))
        if near is not None:
            closest = {
                "bearing": bearing((self.boat.lat, self.boat.lon), (near.lat, near.lon)),
                "distance": distance((self.boat.lat, self.boat.lon), (near.lat, near.lon)),
            }

        ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        heading = self.boat.cog + 0.05
        deltas = [build_self_delta(
            (self.boat.lat, self.boat.lon), sog, self.boat.cog, heading,
            awa, aws, twa, tws, depth, water_temp, battv,
            current_set, current_drift, nav, ap_mode, ap_target, closest, ts)]

        include_static = (t - self._static_at) >= 6.0
        if include_static:
            self._static_at = t
        for v in self.fleet.vessels:
            deltas.append(build_ais_delta(v, ts, include_static))
        return deltas


async def _login_token(host, port):
    login = {"username": "admin", "password": "admin"}
    req = urllib.request.Request(
        "http://%s:%d/signalk/v1/auth/login" % (host, port),
        data=json.dumps(login).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())["token"]


async def run(args):
    sim = Simulator(steady=args.steady, ais=args.ais)
    if args.dump:
        for tick in range(0, 3):  # a few ticks is enough for inspection/smoke
            for d in sim.tick(float(tick), 1.0):
                print(json.dumps(d))
        return

    import websockets  # lazy: unit tests and --dump don't need it
    token = await _login_token(args.host, args.port)
    print("got token len=%d" % len(token))
    uri = "ws://%s:%d/signalk/v1/stream?subscribe=none&token=%s" % (
        args.host, args.port, token)
    print("connecting (authenticated)")
    async with websockets.connect(uri) as ws:
        hello = await ws.recv()
        print("server hello: %s..." % hello[:120])
        t0 = time.time()
        while True:
            t = time.time() - t0
            for d in sim.tick(t, 1.0):
                await ws.send(json.dumps(d))
            await asyncio.sleep(1.0)


def main(argv=None):
    args = parse_args(argv)
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
