"""instruments#21: web security guards that a C++ unit test cannot see.

- No release env (resolved through extends + ${sec.opt}) may define the
  lab-only YEYBOATS_LAB_OPEN_WEB auth bypass, and no workflow may inject it
  through PLATFORMIO_BUILD_FLAGS. web.cpp additionally #errors if the flag
  meets YEYBOATS_RELEASE_BUILD, which covers ad-hoc env-var injection.
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
                self.assertNotIn(LAB_FLAG, e["flags"])
                self.assertIn("YEYBOATS_RELEASE_BUILD", e["flags"])

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
                        self.assertNotIn(LAB_FLAG, line)

    def test_web_cpp_refuses_lab_flag_in_release_build(self):
        src = read("src/web.cpp")
        self.assertRegex(
            src,
            r"#if\s+defined\(YEYBOATS_LAB_OPEN_WEB\)\s*&&\s*defined\(YEYBOATS_RELEASE_BUILD\)"
            r"\s*\n\s*#error",
        )


class TestNoWildcardCors(unittest.TestCase):
    def test_no_wildcard_allow_origin_in_firmware(self):
        pat = re.compile(r'Access-Control-Allow-Origin"\s*,\s*"\*"')
        for path in glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True):
            with self.subTest(file=os.path.relpath(path, ROOT)):
                with open(path, encoding="utf-8") as f:
                    self.assertIsNone(pat.search(f.read()))


if __name__ == "__main__":
    unittest.main()
