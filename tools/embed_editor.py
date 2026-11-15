#!/usr/bin/env python3
"""Embed the device-hosted MIDL editor page into a C++ header.

Usage:
    python3 tools/embed_editor.py          # regenerate include/midl_gen/editor_page.h
    python3 tools/embed_editor.py --check  # fail if the header is stale

Source: midl/editor/device/dist/index.html.gz + manifest.json in the midl
submodule (built and committed by `npm run build:device` in the midl repo; see
midl/editor/README.md). The display serves the page at /editor
(docs/midl/midl-only.md, "Device-hosted editor").

Steps:
  1. Verify the committed page against manifest.json (gzSha256 of the .gz,
     sha256 of the uncompressed page): a mismatch fails.
  2. Strip external font imports. The page's CSS imports Google Fonts, but a
     boat has no internet: the device copy must make no external requests.
     `@import "https://fonts.googleapis.com/..."` (and url()/<link> forms for
     fonts.googleapis.com / fonts.gstatic.com) are removed; the page falls
     back to its local font stack.
  3. Fail when any other http(s):// reference remains. Allowed hosts are
     identifiers, not fetched resources: www.w3.org (XML/SVG namespace URIs)
     and reactjs.org (the text of React's error-decoder message).
  4. Re-gzip (level 9, mtime 0) and write the bytes as a const array (flash
     rodata, never RAM) plus the sha256 of the served (stripped) page, used as
     the ETag.

--check compares the upstream sha256 and the stripped-page sha256 recorded in
the header (not the gzip bytes, which may differ between zlib versions) and
verifies the embedded gzip decompresses to that page.
"""

import gzip
import hashlib
import json
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
DIST_DIR = os.path.join(REPO_ROOT, "midl", "editor", "device", "dist")
OUT = os.path.join(REPO_ROOT, "include", "midl_gen", "editor_page.h")

ALLOWED_HOSTS = {"www.w3.org", "reactjs.org"}
FONT_HOSTS = r"fonts\.(?:googleapis|gstatic)\.com"

# @import "https://fonts..."; / @import url("https://fonts..."); / @import
# url(https://fonts...); with an optional media list. A quoted URL is consumed
# through its closing quote: Google Fonts URLs carry semicolons of their own
# (`wght@400;500`), so stopping at the first `;` leaves a stray quote behind
# and the browser drops the whole stylesheet.
_IMPORT_RE = re.compile(
    r"@import\s*(?:url\(\s*)?(?:"
    r"\"(?:https?:)?//" + FONT_HOSTS + r"[^\"]*\"|"
    r"'(?:https?:)?//" + FONT_HOSTS + r"[^']*'|"
    r"(?:https?:)?//" + FONT_HOSTS + r"[^)\s]*"
    r")\s*\)?[^;\"']*;", re.IGNORECASE)
# Pieces of a font URL that must never survive stripping.
_FONT_REMNANT_RE = re.compile(r"family=[A-Za-z+]+:wght@|display=swap", re.IGNORECASE)
_LINK_RE = re.compile(r"<link\b[^>]*" + FONT_HOSTS + r"[^>]*>", re.IGNORECASE)
_URL_RE = re.compile(r"(?:https?:)?//([A-Za-z0-9.-]+\.[A-Za-z]{2,})")
_SCHEME_RE = re.compile(r"https?://([A-Za-z0-9.-]+)", re.IGNORECASE)


def strip_external_fonts(page):
    """Return (page, removed_count) with the font imports / links removed."""
    out, n1 = _IMPORT_RE.subn("", page)
    out, n2 = _LINK_RE.subn("", out)
    return out, n1 + n2


def external_refs(page):
    """Hosts of http(s):// references outside ALLOWED_HOSTS, plus any remaining
    reference to a font host (protocol-relative included)."""
    bad = sorted({h for h in _SCHEME_RE.findall(page) if h.lower() not in ALLOWED_HOSTS})
    if re.search(FONT_HOSTS, page, re.IGNORECASE):
        bad.append("fonts.googleapis.com/gstatic (unstripped)")
    if _FONT_REMNANT_RE.search(page):
        bad.append("remnant of a font URL (partially stripped @import)")
    return bad


def load_source():
    man_path = os.path.join(DIST_DIR, "manifest.json")
    if not os.path.exists(man_path):
        raise SystemExit("%s missing: git submodule update --init midl" % man_path)
    with open(man_path, encoding="utf-8") as f:
        man = json.load(f)
    entry = next((e for e in man.get("files", []) if e.get("path") == "index.html.gz"), None)
    if not entry:
        raise SystemExit("manifest.json lists no index.html.gz")
    with open(os.path.join(DIST_DIR, entry["path"]), "rb") as f:
        gz = f.read()
    if hashlib.sha256(gz).hexdigest() != entry.get("gzSha256"):
        raise SystemExit("index.html.gz does not match manifest.json gzSha256")
    raw = gzip.decompress(gz)
    if hashlib.sha256(raw).hexdigest() != entry.get("sha256"):
        raise SystemExit("index.html.gz content does not match manifest.json sha256")
    return entry, raw


def build():
    """(source_sha, served_sha, raw_len, gz_bytes, fonts_removed)."""
    entry, raw = load_source()
    page = raw.decode("utf-8")
    page, removed = strip_external_fonts(page)
    bad = external_refs(page)
    if bad:
        raise SystemExit("editor page still references external resources: " + ", ".join(bad))
    served = page.encode("utf-8")
    gz = gzip.compress(served, compresslevel=9, mtime=0)
    return entry["sha256"], hashlib.sha256(served).hexdigest(), len(served), gz, removed


def render(source_sha, served_sha, raw_len, gz, removed):
    lines = [
        "#pragma once",
        "// AUTO-GENERATED by tools/embed_editor.py - do not edit.",
        "// Source: midl/editor/device/dist/index.html.gz (midl submodule), external",
        "// font imports stripped (%d removed), re-gzipped. Regenerate with" % removed,
        "// `make embed-editor` (or `make midl-latest`).",
        "//",
        "// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0",
        "",
        "#include <stddef.h>",
        "",
        "// clang-format off",
        "namespace midl_editor {",
        "",
        "// sha256 of the upstream page (midl manifest.json `sha256`).",
        'inline constexpr const char *SOURCE_SHA256 = "%s";' % source_sha,
        "// sha256 of the served (stripped) page: the ETag.",
        'inline constexpr const char *SHA256 = "%s";' % served_sha,
        "inline constexpr size_t RAW_BYTES = %d;" % raw_len,
        "inline constexpr size_t GZ_LEN = %d;" % len(gz),
        "// gzip bytes, served as-is with Content-Encoding: gzip (rodata/flash).",
        "alignas(4) inline constexpr unsigned char GZ[] = {",
    ]
    for i in range(0, len(gz), 24):
        lines.append(",".join("0x%02x" % b for b in gz[i:i + 24]) + ",")
    lines += [
        "};",
        "",
        "}  // namespace midl_editor",
        "// clang-format on",
        "",
    ]
    return "\n".join(lines)


def parse_header(text):
    def field(name):
        m = re.search(r'\b%s = "([0-9a-f]+)"' % name, text)
        return m.group(1) if m else None

    m = re.search(r"GZ\[\] = \{(.*?)\};", text, re.S)
    gz = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(1))) if m else b""
    return field("SOURCE_SHA256"), field("SHA256"), gz


def main(argv):
    source_sha, served_sha, raw_len, gz, removed = build()
    rel = os.path.relpath(OUT, REPO_ROOT)
    if "--check" in argv:
        if not os.path.exists(OUT):
            print("missing %s (run `make embed-editor`)" % rel)
            return 1
        with open(OUT, encoding="utf-8") as f:
            cur_src, cur_served, cur_gz = parse_header(f.read())
        ok = cur_src == source_sha and cur_served == served_sha
        try:
            ok = ok and hashlib.sha256(gzip.decompress(cur_gz)).hexdigest() == served_sha
        except OSError:
            ok = False
        if not ok:
            print("stale %s (run `make embed-editor`)" % rel)
            return 1
        print("%s up to date (page sha256 %s)" % (rel, served_sha[:12]))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(render(source_sha, served_sha, raw_len, gz, removed))
    print("wrote %s (%d B page, %d B gzip, %d font import(s) stripped)" %
          (rel, raw_len, len(gz), removed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
