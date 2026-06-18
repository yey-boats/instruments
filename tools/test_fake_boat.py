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
