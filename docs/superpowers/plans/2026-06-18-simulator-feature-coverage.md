# Simulator Feature-Coverage Enrichment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `tools/fake_boat.py` into a phase-driven simulator that emits the firmware's usually-empty paths (route/waypoint, autopilot, polar, depthKeel), full AIS vessel contexts, and a self-context bearing path that compass markers can bind to — with usually-empty states cycling populated↔cleared.

**Architecture:** Refactor `fake_boat.py` into pure, importable units — `geo` helpers, `BoatState`, `Route`, `AisFleet`, `PhaseScheduler`, and delta builders — composed by a thin async `main()` guarded under `__main__`. Position is dead-reckoned so derived nav (XTE/BTW/VMG) is self-consistent; when a route is active the boat steers toward the active waypoint so it actually progresses. A `--dump` mode prints deltas as JSON lines through the exact same builders the live path uses, enabling a dependency-free smoke test.

**Tech Stack:** Python 3 (stdlib only for logic + tests: `math`, `argparse`, `json`, `unittest`); `websockets` imported lazily inside the connect path so unit tests don't need it. Spec: `docs/superpowers/specs/2026-06-18-simulator-feature-coverage-design.md`.

---

## File Structure

- **Modify** `tools/fake_boat.py` — all simulator logic + CLI. Sections: geo helpers → `BoatState` → `Route` → `AisFleet` → `PhaseScheduler` → delta builders → `run()`/`main()` → `if __name__ == "__main__"`.
- **Create** `tools/test_fake_boat.py` — `unittest` tests for the pure units. Run from repo root:
  `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v` (unittest puts `tools/` on `sys.path`, so the test does `import fake_boat`).

Conventions: all angles in **radians** in deltas (SignalK native); internal math uses radians; `geo` functions take `(lat_deg, lon_deg)` tuples and return degrees for positions, radians for bearings. XTE sign: **positive = right/starboard of track** (matches firmware `sk::Data::xte`).

---

## Task 1: Make the module importable + CLI scaffold

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing test**

Create `tools/test_fake_boat.py`:

```python
import unittest
import fake_boat as fb


class TestModuleShape(unittest.TestCase):
    def test_import_does_not_connect(self):
        # Importing must not run the asyncio loop or require websockets.
        self.assertTrue(hasattr(fb, "parse_args"))
        self.assertTrue(hasattr(fb, "main"))

    def test_parse_args_defaults(self):
        ns = fb.parse_args([])
        self.assertEqual(ns.host, "localhost")
        self.assertEqual(ns.port, 3000)
        self.assertFalse(ns.dump)
        self.assertFalse(ns.steady)
        self.assertEqual(ns.ais, 4)

    def test_parse_args_flags(self):
        ns = fb.parse_args(["myhost", "3001", "--dump", "--steady", "--ais", "2"])
        self.assertEqual(ns.host, "myhost")
        self.assertEqual(ns.port, 3001)
        self.assertTrue(ns.dump)
        self.assertTrue(ns.steady)
        self.assertEqual(ns.ais, 2)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — import error or `module 'fake_boat' has no attribute 'parse_args'` (current file runs `asyncio.run` at import and imports `websockets` at top).

- [ ] **Step 3: Rewrite the top of `tools/fake_boat.py`**

Replace the whole file's header/footer so it is importable. Keep the existing sinusoid body for now inside `main()` (later tasks replace it). New top:

```python
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
```

And the footer (replace the trailing `asyncio.run(main())`):

```python
def main(argv=None):
    args = parse_args(argv)
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
```

For this task, define a temporary `run` so the module imports cleanly (replaced in Task 8). Add near the bottom, above `main`:

```python
async def run(args):  # replaced in Task 8
    raise NotImplementedError
```

Move `import websockets` OUT of module scope — it will be imported lazily inside `run()` in Task 8. Remove the old top-level `import sys` / `HOST` / `PORT` / `import websockets` lines and the old `asyncio.run(main())` call.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (3 tests).

- [ ] **Step 5: Lint**

Run: `python3 -m py_compile tools/fake_boat.py tools/test_fake_boat.py`
Expected: no output (success).

- [ ] **Step 6: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "refactor(sim): make fake_boat importable + argparse CLI scaffold"
```

---

## Task 2: `geo` great-circle helpers

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_fake_boat.py`:

```python
class TestGeo(unittest.TestCase):
    def test_bearing_due_east(self):
        b = fb.bearing((0.0, 0.0), (0.0, 1.0))
        self.assertAlmostEqual(math.degrees(b), 90.0, places=2)

    def test_bearing_due_north(self):
        b = fb.bearing((0.0, 0.0), (1.0, 0.0))
        self.assertAlmostEqual(math.degrees(b), 0.0, places=2)

    def test_distance_one_degree_lat(self):
        # 1 deg of latitude ~= 111.2 km
        d = fb.distance((0.0, 0.0), (1.0, 0.0))
        self.assertAlmostEqual(d, 111195.0, delta=200.0)

    def test_dead_reckon_advances_north(self):
        # 10 m/s due north (cog=0) for 100 s -> ~1000 m north
        lat, lon = fb.dead_reckon(0.0, 0.0, 0.0, 10.0, 100.0)
        self.assertAlmostEqual(lon, 0.0, places=4)
        self.assertGreater(lat, 0.0)
        self.assertAlmostEqual(fb.distance((0.0, 0.0), (lat, lon)), 1000.0, delta=2.0)

    def test_cross_track_right_is_positive(self):
        # Track due east; a point to the SOUTH is to the right (starboard).
        xt = fb.cross_track((0.0, 0.0), (0.0, 1.0), (-0.01, 0.5))
        self.assertGreater(xt, 0.0)

    def test_cross_track_left_is_negative(self):
        xt = fb.cross_track((0.0, 0.0), (0.0, 1.0), (0.01, 0.5))
        self.assertLess(xt, 0.0)
```

Add `import math` at the top of the test file (alongside `import unittest`).

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'bearing'`.

- [ ] **Step 3: Implement the helpers**

Add to `tools/fake_boat.py` after `parse_args` (a `# --- geo helpers ---` section):

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (9 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): great-circle geo helpers (bearing/distance/dead-reckon/XTE)"
```

---

## Task 3: `BoatState` — dead-reckoned own position

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append:

```python
class TestBoatState(unittest.TestCase):
    def test_step_moves_along_cog(self):
        bs = fb.BoatState(lat=41.0, lon=2.0)
        start = (bs.lat, bs.lon)
        bs.step(dt=10.0, desired_cog=math.radians(90.0), sog=5.0)  # due east
        self.assertGreater(bs.lon, start[1])           # moved east
        self.assertAlmostEqual(bs.lat, start[0], places=3)
        self.assertAlmostEqual(bs.cog, math.radians(90.0), delta=math.radians(5))

    def test_cog_lags_toward_desired(self):
        bs = fb.BoatState(lat=41.0, lon=2.0, cog=0.0)
        bs.step(dt=1.0, desired_cog=math.radians(90.0), sog=5.0)
        # heading turns toward the target but does not snap instantly
        self.assertGreater(bs.cog, 0.0)
        self.assertLess(bs.cog, math.radians(90.0))
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'BoatState'`.

- [ ] **Step 3: Implement `BoatState`**

Add a `# --- own-vessel state ---` section:

```python
def _wrap_pi(a):
    """Wrap angle to (-pi, pi]."""
    return (a + math.pi) % (2 * math.pi) - math.pi


class BoatState:
    """Integrates own position from a desired course each tick. SOG/heading set
    by the caller (driven by the route when active, else a gentle wander)."""

    TURN_RATE = math.radians(6.0)  # max rad/s the heading slews toward desired

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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (11 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): BoatState dead-reckoned position with heading slew"
```

---

## Task 4: `Route` — derived waypoint/track nav

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append:

```python
class TestRoute(unittest.TestCase):
    def _route(self):
        # Two legs heading roughly east from the start area.
        return fb.Route([(41.000, 2.000), (41.000, 2.020), (41.005, 2.040)])

    def test_nav_toward_active_waypoint(self):
        r = self._route()
        nav = r.nav(pos=(41.000, 2.010), cog=math.radians(90.0), sog=5.0)
        # BTW points east-ish toward wp index 1 at (41.000, 2.020)
        self.assertAlmostEqual(math.degrees(nav["btw"]), 90.0, delta=2.0)
        self.assertGreater(nav["dtw"], 0.0)
        # VMG ~ sog when heading at the waypoint
        self.assertAlmostEqual(nav["vmg"], 5.0, delta=0.5)

    def test_xte_sign_right_of_track(self):
        r = self._route()
        # Boat south of an eastbound leg -> right of track -> positive XTE.
        nav = r.nav(pos=(40.999, 2.010), cog=math.radians(90.0), sog=5.0)
        self.assertGreater(nav["xte"], 0.0)

    def test_advances_on_arrival(self):
        r = self._route()
        self.assertEqual(r.index, 1)
        # Sit essentially on top of waypoint 1.
        r.nav(pos=(41.000, 2.0200), cog=math.radians(90.0), sog=5.0)
        self.assertEqual(r.index, 2)

    def test_finished_after_last_waypoint(self):
        r = self._route()
        r.nav(pos=(41.000, 2.0200), cog=0.0, sog=5.0)  # arrive wp1 -> index 2
        r.nav(pos=(41.005, 2.0400), cog=0.0, sog=5.0)  # arrive wp2 -> finished
        self.assertTrue(r.finished)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'Route'`.

- [ ] **Step 3: Implement `Route`**

Add a `# --- route ---` section:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (15 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): Route derives XTE/CTS/BTW/DTW/VMG, advances on arrival"
```

---

## Task 5: `AisFleet` — synthetic AIS targets

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append:

```python
class TestAisFleet(unittest.TestCase):
    def test_set_count_spawns_and_despawns(self):
        fleet = fb.AisFleet(center=(41.0, 2.0))
        fleet.set_count(3)
        self.assertEqual(len(fleet.vessels), 3)
        fleet.set_count(1)
        self.assertEqual(len(fleet.vessels), 1)
        fleet.set_count(0)
        self.assertEqual(len(fleet.vessels), 0)

    def test_vessel_has_distinct_context_and_moves(self):
        fleet = fb.AisFleet(center=(41.0, 2.0))
        fleet.set_count(2)
        ctxs = {v.context for v in fleet.vessels}
        self.assertEqual(len(ctxs), 2)
        for c in ctxs:
            self.assertTrue(c.startswith("vessels.urn:mrn:imo:mmsi:"))
        before = (fleet.vessels[0].lat, fleet.vessels[0].lon)
        fleet.step(dt=10.0)
        after = (fleet.vessels[0].lat, fleet.vessels[0].lon)
        self.assertNotEqual(before, after)

    def test_nearest_returns_closest(self):
        fleet = fb.AisFleet(center=(41.0, 2.0))
        fleet.set_count(3)
        near = fleet.nearest((41.0, 2.0))
        self.assertIsNotNone(near)
        d_near = fb.distance((41.0, 2.0), (near.lat, near.lon))
        for v in fleet.vessels:
            self.assertLessEqual(d_near, fb.distance((41.0, 2.0), (v.lat, v.lon)) + 1.0)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'AisFleet'`.

- [ ] **Step 3: Implement `AisVessel` + `AisFleet`**

Add a `# --- AIS ---` section. Deterministic (no RNG) so `--dump` output and tests are stable; targets are placed on a ring around the center, one on a converging course:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (18 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): AisFleet synthetic targets (converging CPA + nearest)"
```

---

## Task 6: `PhaseScheduler` — cycle usually-empty states

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append:

```python
class TestPhaseScheduler(unittest.TestCase):
    def test_route_starts_active(self):
        ph = fb.PhaseScheduler(steady=False)
        self.assertTrue(ph.route_active(t=0.0))

    def test_route_clears_then_reactivates(self):
        ph = fb.PhaseScheduler(steady=False)
        # ROUTE_PERIOD splits into active / cleared windows; sample both.
        active_seen = any(ph.route_active(t) for t in range(0, 60))
        cleared_seen = any(not ph.route_active(t)
                           for t in range(0, int(ph.ROUTE_PERIOD)))
        self.assertTrue(active_seen)
        self.assertTrue(cleared_seen)

    def test_ap_cycles_through_modes(self):
        ph = fb.PhaseScheduler(steady=False)
        modes = {ph.ap_mode(t) for t in range(0, int(ph.AP_PERIOD))}
        self.assertEqual(modes, {"standby", "auto", "wind"})

    def test_ais_count_breathes(self):
        ph = fb.PhaseScheduler(steady=False)
        counts = {ph.ais_count(t, maxn=4) for t in range(0, int(ph.AIS_PERIOD))}
        self.assertIn(0, counts)
        self.assertIn(4, counts)

    def test_steady_pins_everything_populated(self):
        ph = fb.PhaseScheduler(steady=True)
        for t in (0.0, 123.0, 9999.0):
            self.assertTrue(ph.route_active(t))
            self.assertEqual(ph.ap_mode(t), "auto")
            self.assertEqual(ph.ais_count(t, maxn=4), 4)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'PhaseScheduler'`.

- [ ] **Step 3: Implement `PhaseScheduler`**

Add a `# --- phase scheduling ---` section:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (23 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): PhaseScheduler cycles route/AP/AIS populated<->cleared"
```

---

## Task 7: Delta builders

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing tests**

Append:

```python
def _values_to_map(delta):
    out = {}
    for upd in delta["updates"]:
        for v in upd["values"]:
            out[v["path"]] = v["value"]
    return out


class TestDeltaBuilders(unittest.TestCase):
    def test_self_delta_has_core_and_route_when_active(self):
        nav = {"xte": 12.0, "cts": 1.5, "btw": 1.4, "dtw": 800.0, "vmg": 4.0}
        d = fb.build_self_delta(
            pos=(41.0, 2.0), sog=5.0, cog=1.4, heading=1.45,
            awa=0.6, aws=6.0, twa=0.7, tws=7.5, depth=12.0, water_temp=290.0,
            battv=12.7, current_set=3.6, current_drift=0.7,
            nav=nav, ap_mode="auto", ap_target=1.45,
            closest=None, ts="2026-06-18T10:00:00Z")
        m = _values_to_map(d)
        self.assertEqual(d["context"], "vessels.self")
        self.assertIn("navigation.position", m)
        self.assertEqual(m["navigation.courseRhumbline.crossTrackError"], 12.0)
        self.assertEqual(m["navigation.courseRhumbline.nextPoint.bearingTrue"], 1.4)
        self.assertEqual(m["navigation.courseRhumbline.nextPoint.distance"], 800.0)
        self.assertEqual(m["navigation.courseRhumbline.velocityMadeGood"], 4.0)
        self.assertEqual(m["steering.autopilot.state"], "auto")
        self.assertEqual(m["steering.autopilot.target.headingTrue"], 1.45)
        self.assertIn("environment.depth.belowKeel", m)
        self.assertIn("performance.beatAngle", m)
        self.assertIn("performance.gybeAngle", m)

    def test_self_delta_standby_omits_route_and_target(self):
        d = fb.build_self_delta(
            pos=(41.0, 2.0), sog=5.0, cog=1.4, heading=1.45,
            awa=0.6, aws=6.0, twa=0.7, tws=7.5, depth=12.0, water_temp=290.0,
            battv=12.7, current_set=3.6, current_drift=0.7,
            nav=None, ap_mode="standby", ap_target=None,
            closest=None, ts="2026-06-18T10:00:00Z")
        m = _values_to_map(d)
        self.assertEqual(m["steering.autopilot.state"], "standby")
        self.assertNotIn("steering.autopilot.target.headingTrue", m)
        self.assertNotIn("navigation.courseRhumbline.crossTrackError", m)

    def test_self_delta_closest_approach_bearing(self):
        d = fb.build_self_delta(
            pos=(41.0, 2.0), sog=5.0, cog=1.4, heading=1.45,
            awa=0.6, aws=6.0, twa=0.7, tws=7.5, depth=12.0, water_temp=290.0,
            battv=12.7, current_set=3.6, current_drift=0.7,
            nav=None, ap_mode="standby", ap_target=None,
            closest={"bearing": 2.1, "distance": 1500.0}, ts="t")
        m = _values_to_map(d)
        self.assertEqual(m["navigation.closestApproach.bearingTrue"], 2.1)
        self.assertEqual(m["navigation.closestApproach.distance"], 1500.0)

    def test_ais_delta_shape(self):
        v = fb.AisVessel(0, center=(41.0, 2.0))
        d = fb.build_ais_delta(v, ts="t", include_static=True)
        m = _values_to_map(d)
        self.assertEqual(d["context"], v.context)
        self.assertIn("navigation.position", m)
        self.assertIn("navigation.speedOverGround", m)
        self.assertIn("navigation.courseOverGroundTrue", m)
        self.assertEqual(m["name"], v.name)
        self.assertEqual(m["mmsi"], str(v.mmsi))

    def test_ais_delta_dynamic_only_omits_static(self):
        v = fb.AisVessel(0, center=(41.0, 2.0))
        m = _values_to_map(fb.build_ais_delta(v, ts="t", include_static=False))
        self.assertNotIn("name", m)
        self.assertIn("navigation.position", m)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'build_self_delta'`.

- [ ] **Step 3: Implement the builders**

Add a `# --- delta builders ---` section. Polar/keel constants kept here:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (29 tests total).

- [ ] **Step 5: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): self + AIS delta builders (route/AP/polar/keel/closest)"
```

---

## Task 8: Compose the run loop (live + `--dump`)

**Files:**
- Modify: `tools/fake_boat.py`
- Test: `tools/test_fake_boat.py`

- [ ] **Step 1: Write the failing test**

Append (drives one tick through the same builders the live path uses):

```python
class TestTick(unittest.TestCase):
    def test_steady_tick_emits_all_feature_paths(self):
        sim = fb.Simulator(steady=True, ais=4)
        deltas = sim.tick(t=0.0, dt=1.0)          # list of delta dicts
        self_delta = deltas[0]
        m = _values_to_map(self_delta)
        # route active
        self.assertIn("navigation.courseRhumbline.crossTrackError", m)
        self.assertIn("navigation.courseRhumbline.nextPoint.distance", m)
        # autopilot engaged with a target
        self.assertEqual(m["steering.autopilot.state"], "auto")
        self.assertIn("steering.autopilot.target.headingTrue", m)
        # polar + keel
        self.assertIn("performance.beatAngle", m)
        self.assertIn("environment.depth.belowKeel", m)
        # closest-approach marker path present (fleet non-empty in steady)
        self.assertIn("navigation.closestApproach.bearingTrue", m)
        # at least one AIS context
        ais_ctxs = [d for d in deltas if d["context"].startswith("vessels.urn:")]
        self.assertGreaterEqual(len(ais_ctxs), 1)

    def test_xte_consistent_with_offset(self):
        sim = fb.Simulator(steady=True, ais=0)
        sim.boat.lat -= 0.002  # nudge south of an eastbound leg -> right of track
        deltas = sim.tick(t=0.0, dt=0.0)
        m = _values_to_map(deltas[0])
        self.assertGreater(m["navigation.courseRhumbline.crossTrackError"], 0.0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: FAIL — `module 'fake_boat' has no attribute 'Simulator'`.

- [ ] **Step 3: Implement `Simulator`, `run`, and `--dump`**

Add a `# --- simulator + loop ---` section. The route is anchored near the classic start position so the boat sails it; environment sinusoids preserved from the original:

```python
START = (41.3851, 2.1734)  # Barcelona-ish, matches the original sim


def _route_from(start):
    # Three legs trending NE from the start; ~1-2 km each so legs advance.
    wps = [start]
    pos, brg = start, math.radians(50.0)
    for _ in range(3):
        pos = dead_reckon(pos[0], pos[1], brg, 1500.0, 1.0)
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
        nav = None
        route_on = self.phase.route_active(t) and not self.route.finished
        if route_on:
            preview = self.route.nav((self.boat.lat, self.boat.lon), self.boat.cog, sog)
            desired_cog = preview["btw"] + math.radians(3.0 * math.sin(t / 20))
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
```

Delete the old sinusoid body that previously lived in `main()`/`run()` (it is fully replaced by `Simulator.tick`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tools -p "test_fake_boat.py" -v`
Expected: PASS (31 tests total).

- [ ] **Step 5: Offline smoke check**

Run: `python3 tools/fake_boat.py --dump --steady --ais 4 | python3 -c "import sys,json; paths=set(); ctx=set(); [ (ctx.add(d['context']), [paths.add(v['path']) for u in d['updates'] for v in u['values']]) for d in (json.loads(l) for l in sys.stdin) ]; req={'navigation.courseRhumbline.crossTrackError','navigation.courseRhumbline.nextPoint.distance','steering.autopilot.state','steering.autopilot.target.headingTrue','performance.beatAngle','environment.depth.belowKeel','navigation.closestApproach.bearingTrue'}; missing=req-paths; ais=[c for c in ctx if c.startswith('vessels.urn:')]; assert not missing, ('MISSING %s'%missing); assert ais, 'NO AIS CONTEXT'; print('SMOKE OK: %d paths, %d AIS contexts'%(len(paths),len(ais)))"`
Expected: `SMOKE OK: <n> paths, <m> AIS contexts`

- [ ] **Step 6: Lint**

Run: `python3 -m py_compile tools/fake_boat.py tools/test_fake_boat.py`
Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add tools/fake_boat.py tools/test_fake_boat.py
git commit -m "feat(sim): compose phase-driven run loop + --dump smoke path"
```

---

## Task 9: Verify against the live demo server

**Files:** none (verification only).

- [ ] **Step 1: Bring up the demo SignalK server + sim**

Run: `make demo-up`
Expected: docker SignalK starts; the Makefile launches the manager's demo scripts which invoke `tools/fake_boat.py`.

- [ ] **Step 2: Confirm the new data is flowing**

Run (in another shell): `python3 tools/fake_boat.py localhost 3000` for ~30 s while watching the SignalK data browser (`http://localhost:3000/admin` → Data Browser).
Expected: `vessels.self` shows `navigation.courseRhumbline.*`, `steering.autopilot.state` cycling standby/auto/wind, `performance.beatAngle`, `environment.depth.belowKeel`; and `vessels.urn:mrn:imo:mmsi:*` targets appearing/disappearing.

- [ ] **Step 3: Confirm device + manager render it**

On the device (or `make sim`): the Route/Nav screens show BTW/DTW, the AP HUD shows engaged + target, a compass marker bound to `navigation.closestApproach.bearingTrue` shows an AIS bug. The manager live-view mirrors the same.
Expected: usually-empty fields are populated during active phases and clear during cleared phases.

- [ ] **Step 4: Tear down**

Run: `make demo-down`

- [ ] **Step 5: Final commit (docstring/tidy if anything changed during verification)**

```bash
git add -A tools/
git commit -m "test(sim): verify phase-driven feature coverage against demo server" --allow-empty
```

---

## Self-Review

**Spec coverage:**
- Route/waypoint derived nav → Tasks 2, 4, 7, 8. ✓
- Autopilot phases (standby/auto/wind + target) → Tasks 6, 7, 8. ✓
- Polar targets (beatAngle/gybeAngle) → Task 7. ✓
- depthKeel → Task 7. ✓
- AIS full vessel contexts → Tasks 5, 7, 8. ✓
- Self-context `navigation.closestApproach.*` marker path → Tasks 7, 8. ✓
- Phase cycling populated↔cleared → Task 6, applied in Task 8. ✓
- Position dead-reckoned, derived nav self-consistent → Tasks 3, 8. ✓
- CLI `--steady` / `--dump` / `--ais` → Tasks 1, 8. ✓
- Single file, no new deps; lazy `websockets` → Tasks 1, 8. ✓
- Verification: `py_compile`, `--dump` smoke, live `make demo-up` → Tasks 8, 9. ✓

**Placeholder scan:** none — every code step has complete code; every run step has an exact command + expected output.

**Type consistency:** `nav` dict keys `{xte, cts, btw, dtw, vmg}` defined in Task 4 (`Route.nav`) and consumed identically in Task 7 (`build_self_delta`). `closest` dict `{bearing, distance}` produced in Task 8, consumed in Task 7. `AisVessel` attributes (`context, mmsi, name, ship_type, lat, lon, sog, cog, heading`) defined in Task 5, consumed in Task 7 `build_ais_delta`. `PhaseScheduler` methods (`route_active, ap_mode, ais_count`) defined in Task 6, consumed in Task 8. `BoatState.step(dt, desired_cog, sog)` defined in Task 3, called in Task 8. Consistent. ✓
