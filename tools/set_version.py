#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Set espdisp project MAJOR.MINOR version metadata.",
    )
    parser.add_argument(
        "version",
        help=(
            "MAJOR.MINOR (e.g. 0.3) or a full MAJOR.MINOR.BUILD / tag-style "
            "version (e.g. 0.3.42 or v0.3.42). Only MAJOR.MINOR is recorded; "
            "the VERSION file is normalised to MAJOR.MINOR.0. The BUILD "
            "component is set by CI (github.run_number) at build time and "
            "defaults to 0 for local builds -- it is not hand-edited here."
        ),
    )
    args = parser.parse_args()

    version = args.version.strip()
    if version.startswith("v"):
        version = version[1:]

    # Accept either a bare MAJOR.MINOR or a full semver; we only keep the
    # MAJOR.MINOR, which is the repo-configured source of truth. The patch /
    # BUILD and any pre-release/build metadata are dropped because CI owns
    # the BUILD number now.
    m = re.match(r"^(\d+)\.(\d+)(?:\.\d+)?(?:[-+].*)?$", version)
    if not m:
        parser.error("version must be MAJOR.MINOR or MAJOR.MINOR.BUILD, for example 0.3 or 0.3.42")
    major, minor = m.group(1), m.group(2)
    version = f"{major}.{minor}.0"

    (ROOT / "VERSION").write_text(version + "\n", encoding="utf-8")

    # The SignalK manager plugin moved to the yey-boats/Instruments-manager
    # repo; its version is bumped there. Only update it here if a plugin
    # checkout still happens to live under signalk/ in this tree.
    package_path = ROOT / "signalk/plugins/signalk-espdisp-manager/package.json"
    if package_path.exists():
        package = json.loads(package_path.read_text(encoding="utf-8"))
        package["version"] = version
        write_json(package_path, package)

    lock_path = ROOT / "signalk/plugins/signalk-espdisp-manager/package-lock.json"
    if lock_path.exists():
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        lock["version"] = version
        if "" in lock.get("packages", {}):
            lock["packages"][""]["version"] = version
        write_json(lock_path, lock)

    print(f"set project version to {version}")


if __name__ == "__main__":
    main()
