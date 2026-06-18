#!/usr/bin/env python3

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    errors = []
    if not SEMVER_RE.match(version):
        errors.append(f"VERSION is not semver: {version!r}")

    # The SignalK manager plugin moved to the yey-boats/Instruments-manager
    # repo; its version is checked there. Only validate it here if a plugin
    # checkout still happens to live under signalk/ in this tree.
    plugin_package = ROOT / "signalk/plugins/signalk-espdisp-manager/package.json"
    if plugin_package.exists():
        plugin_version = read_json(plugin_package).get("version")
        if plugin_version != version:
            errors.append(f"{plugin_package.relative_to(ROOT)} version {plugin_version!r} != {version!r}")

    plugin_lock = ROOT / "signalk/plugins/signalk-espdisp-manager/package-lock.json"
    if plugin_lock.exists():
        lock = read_json(plugin_lock)
        if lock.get("version") != version:
            errors.append(f"{plugin_lock.relative_to(ROOT)} version {lock.get('version')!r} != {version!r}")
        root_pkg = lock.get("packages", {}).get("", {})
        if root_pkg.get("version") != version:
            errors.append(
                f"{plugin_lock.relative_to(ROOT)} packages[''].version "
                f"{root_pkg.get('version')!r} != {version!r}"
            )

    if errors:
        for error in errors:
            print(f"version check: {error}", file=sys.stderr)
        return 1

    print(f"version check: {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
