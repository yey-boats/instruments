#!/usr/bin/env python3
"""instruments#58: print every release-* env in platformio.ini, one per line.

release.yml builds exactly this list, so a new release-* env gets a release
artifact without a second hand-kept list in the workflow. Exits non-zero if
platformio.ini defines no release env (a parse slip must not ship an empty
release).

Run: python3 tools/release_envs.py
"""

import configparser
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def release_envs(path=os.path.join(ROOT, "platformio.ini")):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    if not cp.read(path):
        raise FileNotFoundError(path)
    return [s[4:] for s in cp.sections() if s.startswith("env:release-")]


def main():
    envs = release_envs()
    if not envs:
        print("no [env:release-*] sections in platformio.ini", file=sys.stderr)
        return 1
    print("\n".join(envs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
