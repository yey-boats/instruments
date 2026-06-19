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

    # VERSION is the repo-configured MAJOR.MINOR source of truth. The third
    # component (BUILD) is owned by CI -- it is github.run_number on CI builds
    # and 0 for local builds -- so the committed VERSION file must always pin
    # it to MAJOR.MINOR.0. This catches an accidental hand-bump of the patch.
    mm = re.match(r"^(\d+)\.(\d+)\.(\d+)$", version)
    if mm is None:
        errors.append(
            f"VERSION must be MAJOR.MINOR.0 (BUILD is CI-owned): {version!r}"
        )
    elif mm.group(3) != "0":
        errors.append(
            f"VERSION patch/BUILD must be 0 (it is set by CI run_number, "
            f"not hand-edited): {version!r}"
        )

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
