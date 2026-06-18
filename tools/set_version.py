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
    parser = argparse.ArgumentParser(description="Set espdisp project version metadata.")
    parser.add_argument("version", help="Semver version without a leading v, for example 0.1.0 or 0.1.0-rc1")
    args = parser.parse_args()

    version = args.version.strip()
    if version.startswith("v"):
        version = version[1:]
    if not SEMVER_RE.match(version):
        parser.error("version must be semver, for example 0.1.0 or 0.1.0-rc1")

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
