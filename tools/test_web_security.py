"""instruments#21: web security guards that a C++ unit test cannot see.

- No release env (resolved through extends + ${sec.opt}) may define the
  lab-only YEYBOATS_LAB_OPEN_WEB auth bypass, and no workflow may inject it
  through PLATFORMIO_BUILD_FLAGS. web.cpp additionally #errors if the flag
  meets YEYBOATS_RELEASE_BUILD, which covers ad-hoc env-var injection.
- instruments#30: the same guards for the lab-only YEYBOATS_LAB_OPEN_OTA flag
  (password-less ArduinoOTA on a dev build). net.cpp #errors if it meets
  YEYBOATS_RELEASE_BUILD, and release builds compile ArduinoOTA out entirely.
- instruments#58: no workflow may define YEYBOATS_RELEASE_BUILD either. Since
  #56 the guards test its value, so `-D YEYBOATS_RELEASE_BUILD=0` appended
  through PLATFORMIO_BUILD_FLAGS would silently turn a release-* build into a
  dev build (ArduinoOTA back in, lab flags allowed).
- instruments#58: release.yml builds every [env:release-*] (listed by
  tools/release_envs.py) and gates each on the DRAM budget.
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
RELEASE_FLAG = "YEYBOATS_RELEASE_BUILD"
# Build-mode flags only platformio.ini may set; a workflow naming one is
# injecting it (PLATFORMIO_BUILD_FLAGS, a `-D` on a pio command line, ...).
WORKFLOW_BANNED_FLAGS = LAB_FLAGS + (RELEASE_FLAG,)


def workflow_flag_violations(text):
    """(line number, flag) for every banned build flag a workflow sets.

    Every non-comment line counts, not just `PLATFORMIO_BUILD_FLAGS:` ones,
    so a folded/multi-line value or an `export` in a run block is caught
    too. The guard steps that grep for the flags are exempt."""
    hits = []
    for n, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#"):
            continue
        if "check_release_flags" in line or "test_web_security" in line:
            continue
        for flag in WORKFLOW_BANNED_FLAGS:
            if flag in line:
                hits.append((n, flag))
    return hits


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
                # Guards test the VALUE (#56): =0 or =yes would build as dev.
                self.assertRegex(str(e["flags"]), r"YEYBOATS_RELEASE_BUILD=1\b")

    def test_no_env_defines_lab_open_ota(self):
        # Lab-open OTA is opt-in per build (PLATFORMIO_BUILD_FLAGS or
        # `make ... LAB_OPEN_OTA=1`), never baked into any env, dev included.
        for name, e in ENVS.items():
            with self.subTest(env=name):
                self.assertNotIn("YEYBOATS_LAB_OPEN_OTA", e["flags"])

    def test_no_workflow_injects_build_mode_flags(self):
        wfs = glob.glob(os.path.join(ROOT, ".github", "workflows", "*.yml"))
        self.assertTrue(wfs)
        for wf in wfs:
            with self.subTest(workflow=os.path.basename(wf)):
                with open(wf, encoding="utf-8") as f:
                    self.assertEqual(workflow_flag_violations(f.read()), [])

    def test_workflow_scan_catches_release_flag_override(self):
        # instruments#58: each of these would turn a release-* env into a
        # dev build, so the scan must flag it.
        bad = {
            "env key": "    env:\n      PLATFORMIO_BUILD_FLAGS: -D YEYBOATS_RELEASE_BUILD=0\n",
            "no space": "      PLATFORMIO_BUILD_FLAGS: \"-DYEYBOATS_RELEASE_BUILD=0\"\n",
            "folded": "      PLATFORMIO_BUILD_FLAGS: >-\n        -D FOO=1\n        -D YEYBOATS_RELEASE_BUILD=0\n",
            "export": "        run: |\n          export PLATFORMIO_BUILD_FLAGS='-D YEYBOATS_RELEASE_BUILD=0'\n",
            "undef": "      PLATFORMIO_BUILD_FLAGS: -U YEYBOATS_RELEASE_BUILD\n",
            "lab": "      PLATFORMIO_BUILD_FLAGS: -D YEYBOATS_LAB_OPEN_OTA=1\n",
        }
        for name, text in bad.items():
            with self.subTest(case=name):
                self.assertTrue(workflow_flag_violations(text))
        hits = workflow_flag_violations(bad["folded"])
        self.assertEqual(hits, [(3, RELEASE_FLAG)])

    def test_workflow_scan_passes_clean_workflow(self):
        ok = (
            "    env:\n"
            "      PLATFORMIO_BUILD_FLAGS: -D YEYBOATS_TRACE=1\n"
            "      # never set YEYBOATS_RELEASE_BUILD here (instruments#58)\n"
            "      - run: python3 -m unittest discover -s tools -p 'test_web_security.py'\n"
        )
        self.assertEqual(workflow_flag_violations(ok), [])

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
        files = []
        for top in ("src", "include", "lib", "test"):
            for ext in ("*.c", "*.cpp", "*.h", "*.hpp"):
                files += glob.glob(os.path.join(ROOT, top, "**", ext), recursive=True)
        self.assertTrue(files)
        for path in files:
            with open(path, encoding="utf-8") as f:
                for n, line in enumerate(f, 1):
                    if path.endswith(os.path.join("include", "build_config.h")):
                        continue  # the one place that supplies the default
                    if re.search(r"defined\s*\(\s*YEYBOATS_RELEASE_BUILD\s*\)|#\s*ifn?def\s+YEYBOATS_RELEASE_BUILD\b", line):
                        with self.subTest(file=os.path.relpath(path, ROOT), line=n):
                            self.fail(f"{os.path.relpath(path, ROOT)}:{n} tests YEYBOATS_RELEASE_BUILD with defined()")

    def test_ota_policy_reads_the_flag_from_build_config(self):
        hdr = read("include/ota_policy.h")
        self.assertIn('#include "build_config.h"', hdr)


class TestReleaseWorkflowBuildsEveryReleaseEnv(unittest.TestCase):
    """instruments#58: release.yml once kept its own env list and shipped 8
    of 12 release-* envs. It now builds what tools/release_envs.py prints."""

    def test_lister_matches_platformio_release_envs(self):
        import release_envs

        expected = sorted(n for n in ENVS if n.startswith("release-"))
        self.assertEqual(sorted(release_envs.release_envs()), expected)

    def test_release_workflow_derives_envs_and_checks_dram(self):
        wf = read(".github/workflows/release.yml")
        self.assertIn("python3 tools/release_envs.py", wf)
        self.assertRegex(wf, r'make dram-check ENV="\$env"')
        # No hand-kept list may creep back in: no env is named in the code.
        code = "\n".join(l for l in wf.splitlines() if not l.lstrip().startswith("#"))
        for name in ENVS:
            if name.startswith("release-"):
                with self.subTest(env=name):
                    self.assertIsNone(re.search(re.escape(name) + r"\b", code))


class TestNoWildcardCors(unittest.TestCase):
    def test_no_wildcard_allow_origin_in_firmware(self):
        pat = re.compile(r'Access-Control-Allow-Origin"\s*,\s*"\*"')
        for path in glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True):
            with self.subTest(file=os.path.relpath(path, ROOT)):
                with open(path, encoding="utf-8") as f:
                    self.assertIsNone(pat.search(f.read()))


if __name__ == "__main__":
    unittest.main()
