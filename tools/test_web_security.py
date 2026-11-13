"""instruments#21: web security guards that a C++ unit test cannot see.

- No release env (resolved through extends + ${sec.opt}) may define the
  lab-only YEYBOATS_LAB_OPEN_WEB auth bypass, and no workflow may inject it
  through PLATFORMIO_BUILD_FLAGS. web.cpp additionally #errors if the flag
  meets YEYBOATS_RELEASE_BUILD, which covers ad-hoc env-var injection.
- instruments#30: the same guards for the lab-only YEYBOATS_LAB_OPEN_OTA flag
  (password-less ArduinoOTA on a dev build). net.cpp #errors if it meets
  YEYBOATS_RELEASE_BUILD, and release builds compile ArduinoOTA out entirely.
- The device web server never sends a wildcard CORS header. Nothing needs
  cross-origin browser access: the manager talks to the device server-side
  (Node) and the layout editor fetches previews through the plugin proxy.

Run: python3 -m unittest discover -s tools -p 'test_web_security.py'
"""

import glob
import os
import re
import sys
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

from test_midl_class import ENVS  # noqa: E402  (shared platformio.ini resolver)

LAB_FLAG = "YEYBOATS_LAB_OPEN_WEB"
LAB_FLAGS = (LAB_FLAG, "YEYBOATS_LAB_OPEN_OTA")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


class TestLabBypassNeverInRelease(unittest.TestCase):
    def test_release_envs_exist(self):
        self.assertTrue([n for n in ENVS if n.startswith("release-")])

    def test_no_release_env_defines_lab_open_web(self):
        for name, e in ENVS.items():
            if not name.startswith("release-"):
                continue
            with self.subTest(env=name):
                for flag in LAB_FLAGS:
                    self.assertNotIn(flag, e["flags"])
                self.assertIn("YEYBOATS_RELEASE_BUILD", e["flags"])

    def test_no_env_defines_lab_open_ota(self):
        # Lab-open OTA is opt-in per build (PLATFORMIO_BUILD_FLAGS or
        # `make ... LAB_OPEN_OTA=1`), never baked into any env, dev included.
        for name, e in ENVS.items():
            with self.subTest(env=name):
                self.assertNotIn("YEYBOATS_LAB_OPEN_OTA", e["flags"])

    def test_no_workflow_injects_lab_open_web(self):
        for wf in glob.glob(os.path.join(ROOT, ".github", "workflows", "*.yml")):
            with self.subTest(workflow=os.path.basename(wf)):
                with open(wf, encoding="utf-8") as f:
                    for line in f:
                        if line.lstrip().startswith("#"):
                            continue
                        # The guard step itself names the flag in a grep.
                        if "check_release_flags" in line or "test_web_security" in line:
                            continue
                        for flag in LAB_FLAGS:
                            self.assertNotIn(flag, line)

    def test_web_cpp_refuses_lab_flag_in_release_build(self):
        src = read("src/web.cpp")
        self.assertRegex(
            src,
            r"#if\s+defined\(YEYBOATS_LAB_OPEN_WEB\)\s*&&\s*YEYBOATS_RELEASE_BUILD\b"
            r"\s*\n\s*#error",
        )


class TestArduinoOtaNeverInRelease(unittest.TestCase):
    def test_net_cpp_refuses_lab_ota_flag_in_release_build(self):
        src = read("src/net.cpp")
        self.assertRegex(
            src,
            r"#if\s+defined\(YEYBOATS_LAB_OPEN_OTA\)\s*&&\s*YEYBOATS_RELEASE_BUILD\b"
            r"\s*\n\s*#error",
        )

    def test_release_build_compiles_arduino_ota_out(self):
        # Every ArduinoOTA use in net.cpp sits under YEYBOATS_ARDUINO_OTA,
        # which ota_policy.h defines to 0 for release builds.
        hdr = read("include/ota_policy.h")
        self.assertRegex(
            hdr,
            r"#if\s+YEYBOATS_RELEASE_BUILD\s*\n\s*#define\s+YEYBOATS_ARDUINO_OTA\s+0",
        )
        src = read("src/net.cpp")
        self.assertRegex(src, r"#if\s+YEYBOATS_ARDUINO_OTA\s*\n\s*#include\s+<ArduinoOTA\.h>")
        for n, line in enumerate(src.splitlines(), 1):
            stripped = line.strip()
            if "ArduinoOTA." in line and not stripped.startswith("//"):
                with self.subTest(line=n):
                    self.assertTrue(self._guarded(src, n), f"net.cpp:{n} uses ArduinoOTA unguarded")

    @staticmethod
    def _guarded(src, lineno):
        """True if line `lineno` sits inside an `#if YEYBOATS_ARDUINO_OTA` block."""
        stack = []
        for n, line in enumerate(src.splitlines(), 1):
            s = line.strip()
            if s.startswith("#if"):
                stack.append("YEYBOATS_ARDUINO_OTA" in s and "!" not in s)
            elif s.startswith("#else") or s.startswith("#elif"):
                if stack:
                    stack[-1] = False
            elif s.startswith("#endif"):
                if stack:
                    stack.pop()
            if n == lineno:
                return any(stack)
        return False


class TestReleaseFlagIsTestedByValue(unittest.TestCase):
    """build_config.h always defines YEYBOATS_RELEASE_BUILD (0 for dev builds),
    so `defined(YEYBOATS_RELEASE_BUILD)` is true in any file that includes it
    first. That made every LAB_OPEN_OTA / LAB_OPEN_WEB build hit the release
    #error, and left the ArduinoOTA decision depending on include order.
    Guards must test the value: `#if YEYBOATS_RELEASE_BUILD`."""

    def test_no_guard_tests_release_flag_with_defined(self):
        files = glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True)
        files += glob.glob(os.path.join(ROOT, "include", "**", "*.h"), recursive=True)
        self.assertTrue(files)
        for path in files:
            with open(path, encoding="utf-8") as f:
                for n, line in enumerate(f, 1):
                    if re.search(r"defined\s*\(\s*YEYBOATS_RELEASE_BUILD\s*\)", line):
                        with self.subTest(file=os.path.relpath(path, ROOT), line=n):
                            self.fail(f"{os.path.relpath(path, ROOT)}:{n} tests YEYBOATS_RELEASE_BUILD with defined()")

    def test_ota_policy_reads_the_flag_from_build_config(self):
        hdr = read("include/ota_policy.h")
        self.assertIn('#include "build_config.h"', hdr)


class TestNoWildcardCors(unittest.TestCase):
    def test_no_wildcard_allow_origin_in_firmware(self):
        pat = re.compile(r'Access-Control-Allow-Origin"\s*,\s*"\*"')
        for path in glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True):
            with self.subTest(file=os.path.relpath(path, ROOT)):
                with open(path, encoding="utf-8") as f:
                    self.assertIsNone(pat.search(f.read()))


if __name__ == "__main__":
    unittest.main()
