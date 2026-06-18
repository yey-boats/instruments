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


if __name__ == "__main__":
    unittest.main()
