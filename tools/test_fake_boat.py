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


if __name__ == "__main__":
    unittest.main()
