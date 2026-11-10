"""instruments#19: each firmware env embeds ITS display class's MIDL manifest.

Run: python3 -m unittest discover -s tools -p 'test_midl_class.py'
"""

import configparser
import os
import re
import subprocess
import sys
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import midl_class  # noqa: E402
import embed_manifest  # noqa: E402

# Expected class per base board env (release-* / ota-* inherit the board).
EXPECTED = {
    "esp32-4848s040": "square-480",
    "waveshare-touch-lcd-4": "square-480",
    "esp32-8048s050": "landscape-800x480",
    "esp32-8048s070": "landscape-800x480",
    "waveshare-touch-lcd-4_3": "landscape-800x480",
    "waveshare-touch-lcd-4_3b": "landscape-800x480",
    "waveshare-touch-lcd-5_800x480": "landscape-800x480",
    "waveshare-touch-lcd-7_800x480": "landscape-800x480",
    "waveshare-touch-lcd-5_1024x600": "landscape-1024x600",
    "waveshare-touch-lcd-7b_1024x600": "landscape-1024x600",
    "waveshare-knob-1_8": "round-360",
    "waveshare-touch-lcd-1_28": "round-360",
    "harness-s3-devkitc": "square-480",
}


def load_envs():
    """Resolve platformio.ini envs (extends + ${sec.opt} interpolation)."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(os.path.join(ROOT, "platformio.ini"))

    def raw(section, opt, seen=()):
        if cp.has_option(section, opt):
            return cp.get(section, opt)
        if cp.has_option(section, "extends"):
            for parent in cp.get(section, "extends").split(","):
                parent = parent.strip()
                if parent not in seen:
                    v = raw(parent, opt, seen + (section,))
                    if v is not None:
                        return v
        return None

    def expand(text, depth=0):
        if depth > 10:
            return text
        return re.sub(r"\$\{([^}.]+)\.([^}]+)\}",
                      lambda m: expand(raw(m.group(1), m.group(2)) or "", depth + 1), text)

    envs = {}
    for sec in cp.sections():
        if not sec.startswith("env:"):
            continue
        flags = expand(raw(sec, "build_flags") or "")
        flags = " ".join(l.split(";")[0] for l in flags.splitlines())
        envs[sec[4:]] = {
            "flags": flags,
            "platform": raw(sec, "platform") or "",
            "extra_scripts": raw(sec, "extra_scripts") or "",
        }
    return envs


ENVS = load_envs()
DEVICE_ENVS = {n: e for n, e in ENVS.items() if e["platform"].startswith("espressif32")}
SIM_ENVS = {n: e for n, e in ENVS.items() if n == "sim" or n.startswith("sim-")}


class TestMidlClass(unittest.TestCase):
    def test_embed_manifest_knows_every_generated_class(self):
        gen = os.path.join(ROOT, "midl", "schemas", "gen")
        on_disk = sorted(re.match(r"yb-midl-capabilities\.(.+)\.json$", f).group(1)
                         for f in os.listdir(gen) if f.startswith("yb-midl-capabilities."))
        self.assertIn("round-360", on_disk)
        self.assertEqual(sorted(embed_manifest.VALID_CLASSES), on_disk)
        self.assertEqual(sorted(midl_class.CLASSES), on_disk)

    def test_every_device_env_runs_the_class_pre_script(self):
        self.assertTrue(DEVICE_ENVS)
        for name, e in DEVICE_ENVS.items():
            with self.subTest(env=name):
                self.assertIn("pre:tools/midl_class.py", e["extra_scripts"])

    def test_every_device_env_maps_to_a_class(self):
        for name, e in DEVICE_ENVS.items():
            with self.subTest(env=name):
                self.assertIn(midl_class.resolve_class(e["flags"]), midl_class.CLASSES)

    def test_board_envs_map_to_expected_class(self):
        for name, cls in EXPECTED.items():
            with self.subTest(env=name):
                self.assertEqual(cls, midl_class.resolve_class(ENVS[name]["flags"]))

    def test_release_and_ota_envs_match_their_board(self):
        for name, e in DEVICE_ENVS.items():
            base = re.sub(r"^(release-|ota-)", "", name)
            base = {"ota": "esp32-4848s040", "ota-debug": "esp32-4848s040"}.get(name, base)
            base = re.sub(r"-(debug|perf|idf5|arduino|ble)$", "", base)
            if base in EXPECTED:
                with self.subTest(env=name):
                    self.assertEqual(EXPECTED[base], midl_class.resolve_class(e["flags"]))

    def test_sim_envs_render_their_own_class(self):
        # Host render envs pick their class too (sim-knob is round-360, not the
        # square-480 host default); size-less sims fall back to square-480.
        self.assertIn("sim-knob", SIM_ENVS)
        for name, e in SIM_ENVS.items():
            with self.subTest(env=name):
                self.assertIn("pre:tools/midl_class.py", e["extra_scripts"])
        self.assertEqual("round-360", midl_class.resolve_class(
            SIM_ENVS["sim-knob"]["flags"], default="square-480"))
        self.assertEqual("square-480", midl_class.resolve_class(
            SIM_ENVS["sim-control"]["flags"], default="square-480"))
        # tools/sim_render.sh injects the size through PLATFORMIO_BUILD_FLAGS.
        self.assertEqual("landscape-800x480", midl_class.resolve_class(
            SIM_ENVS["sim"]["flags"] + " -DSIM_LCD_W=800 -DSIM_LCD_H=480", default="square-480"))
        self.assertEqual("square-480", midl_class.resolve_class(
            SIM_ENVS["sim"]["flags"], default="square-480"))
        # The default never hides an unmapped board or size.
        with self.assertRaises(ValueError):
            midl_class.resolve_class("-D SIM_LCD_W=320 -D SIM_LCD_H=480", default="square-480")

    def test_unmapped_board_fails_loudly(self):
        with self.assertRaises(ValueError):
            midl_class.resolve_class("-D BOARD_ID_DEFINED -D BOARD_ID_SOMETHING_NEW")
        with self.assertRaises(ValueError):
            midl_class.resolve_class("-D LCD_W=320 -D LCD_H=480")

    def test_per_class_headers_committed_and_fresh(self):
        for cls in midl_class.CLASSES:
            with self.subTest(cls=cls):
                d = os.path.join(ROOT, "include", "midl_gen", cls)
                for f in ("manifest.h", "limits.h"):
                    p = os.path.join(d, f)
                    self.assertTrue(os.path.isfile(p), p)
                    with open(p, encoding="utf-8") as fh:
                        self.assertIn('CLASS_ID = "%s"' % cls, fh.read())
                with open(os.path.join(d, "manifest.h"), encoding="utf-8") as fh:
                    got = fh.read()
                self.assertEqual(embed_manifest.render(cls)[0], got,
                                 "stale: run `make embed-manifest`")

    def test_dispatcher_headers_cover_every_class(self):
        for f in ("generated_midl_manifest.h", "generated_midl_limits.h"):
            with open(os.path.join(ROOT, "include", f), encoding="utf-8") as fh:
                txt = fh.read()
            for cls in midl_class.CLASSES:
                with self.subTest(header=f, cls=cls):
                    self.assertIn(midl_class.macro_for(cls), txt)
                    self.assertIn('"midl_gen/%s/' % cls, txt)


if __name__ == "__main__":
    unittest.main()
