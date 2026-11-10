"""PlatformIO pre: script -- select this env's MIDL display class (instruments#19).

Every firmware build used to embed the square-480 capability manifest/limits.
This script maps the env's board (its `-D BOARD_ID_*` build flag, else its
`LCD_W`/`LCD_H`) to a MIDL display class and defines `YB_MIDL_CLASS_<ID>`;
include/generated_midl_manifest.h / generated_midl_limits.h then pick the
committed per-class headers under include/midl_gen/<class>/ (regenerate with
`make embed-manifest`). A device env whose board is not mapped fails the build
instead of silently serving the wrong manifest.

Importable without SCons (`resolve_class`) so tools/test_midl_class.py can
check every env in platformio.ini.
"""

import re

CLASSES = ("square-480", "landscape-800x480", "landscape-1024x600", "round-360")

# BOARD_ID_<X> -> MIDL display class. Keep in sync with platformio.ini envs.
BOARD_CLASS = {
    "SUNTON_4848S040": "square-480",              # 480x480 square
    "WAVESHARE_TOUCH_LCD_4": "square-480",        # 480x480 square
    "SUNTON_8048S050": "landscape-800x480",       # 5.0" 800x480
    "SUNTON_8048S070": "landscape-800x480",       # 7.0" 800x480
    "WAVESHARE_TOUCH_LCD_4_3": "landscape-800x480",
    "WAVESHARE_TOUCH_LCD_4_3B": "landscape-800x480",
    "WAVESHARE_TOUCH_LCD_5_800X480": "landscape-800x480",
    "WAVESHARE_TOUCH_LCD_7_800X480": "landscape-800x480",
    "WAVESHARE_TOUCH_LCD_5_1024X600": "landscape-1024x600",
    "WAVESHARE_TOUCH_LCD_7B_1024X600": "landscape-1024x600",
    "WAVESHARE_KNOB_1_8": "round-360",            # round 360x360
    # Round 240x240: no round-240 class exists; round-360 is the only round
    # class (4 tiles / depth 2), the closest fit.
    "WAVESHARE_TOUCH_LCD_1_28": "round-360",
    "NATIVE_FAKE": "square-480",                  # host tests
}

DIM_CLASS = {
    (480, 480): "square-480",
    (800, 480): "landscape-800x480",
    (1024, 600): "landscape-1024x600",
    (360, 360): "round-360",
}

_BOARD_RE = re.compile(r"-D\s*BOARD_ID_(?!DEFINED\b)([A-Z0-9_]+)\b")


def _define_int(flags, name):
    m = re.search(r"-D\s*%s=(\d+)" % name, flags)
    return int(m.group(1)) if m else None


def macro_for(class_id):
    """'landscape-800x480' -> 'YB_MIDL_CLASS_LANDSCAPE_800X480'."""
    return "YB_MIDL_CLASS_" + re.sub(r"[^A-Z0-9]", "_", class_id.upper())


def resolve_class(flags):
    """Map an env's (flattened) build flags to a MIDL display class id.

    Raises ValueError when the env cannot be mapped.
    """
    boards = _BOARD_RE.findall(flags)
    for b in boards:
        if b not in BOARD_CLASS:
            raise ValueError("BOARD_ID_%s has no MIDL display class; map it in "
                             "tools/midl_class.py BOARD_CLASS" % b)
    if boards:
        # Variant envs inherit the family flag list and append their own id
        # (waveshare-touch-lcd-4_3 = ${waveshare-touch-lcd-4} + _4_3), and the
        # board code lets the variant win (board_waveshare_touch_lcd_4.cpp
        # profile()); the last-listed id is the variant.
        return BOARD_CLASS[boards[-1]]
    for wn, hn in (("LCD_W", "LCD_H"), ("SIM_LCD_W", "SIM_LCD_H")):
        w, h = _define_int(flags, wn), _define_int(flags, hn)
        if w is not None and h is not None:
            cls = DIM_CLASS.get((w, h))
            if cls is None:
                raise ValueError("%dx%d has no MIDL display class" % (w, h))
            return cls
    if re.search(r"-D\s*YEYBOATS_HARNESS\b", flags):
        return "square-480"  # headless soak harness: no display, never serves the manifest
    raise ValueError("no BOARD_ID_* / LCD_W+LCD_H flag to derive a MIDL display class from")


def _flatten(build_flags):
    if isinstance(build_flags, (list, tuple)):
        return " ".join(str(x) for x in build_flags)
    return str(build_flags or "")


try:
    Import("env")  # noqa: F821  (SCons builtin; absent when imported by tests)
except NameError:
    env = None

if env is not None:
    import os

    _flags = _flatten(env.GetProjectOption("build_flags", []))
    _flags += " " + os.environ.get("PLATFORMIO_BUILD_FLAGS", "")
    try:
        _cls = resolve_class(_flags)
    except ValueError as e:
        raise SystemExit("[midl_class] env:%s: %s" % (env["PIOENV"], e))
    env.Append(CPPDEFINES=[macro_for(_cls)])
    print("[midl_class] env:%s -> MIDL display class %s" % (env["PIOENV"], _cls))
