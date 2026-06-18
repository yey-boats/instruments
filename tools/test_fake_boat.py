import math
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

    def test_bearing_to_active_no_mutation(self):
        r = self._route()
        idx_before = r.index
        _ = r.bearing_to_active((41.000, 2.020))
        self.assertEqual(r.index, idx_before)
        self.assertFalse(r.finished)

    def test_single_tick_advances_at_most_one(self):
        # Place boat on top of the active waypoint; a single nav() call should
        # advance index by exactly 1; a *second* separate call is what advances again.
        r = self._route()
        self.assertEqual(r.index, 1)
        r.nav(pos=(41.000, 2.0200), cog=math.radians(90.0), sog=5.0)
        self.assertEqual(r.index, 2)  # advanced by 1 from the single call
        # The arrival of wp2 requires another explicit nav() call, not a phantom double.
        self.assertFalse(r.finished)


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
        maxn = 4
        counts = {ph.ais_count(t, maxn=maxn) for t in range(0, int(ph.AIS_PERIOD))}
        self.assertIn(0, counts)
        self.assertIn(maxn, counts)
        # Partial count (half) also appears in the sampled set.
        self.assertIn(maxn // 2, counts)

    def test_steady_pins_everything_populated(self):
        ph = fb.PhaseScheduler(steady=True)
        for t in (0.0, 123.0, 9999.0):
            self.assertTrue(ph.route_active(t))
            self.assertEqual(ph.ap_mode(t), "auto")
            self.assertEqual(ph.ais_count(t, maxn=4), 4)


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


if __name__ == "__main__":
    unittest.main()
