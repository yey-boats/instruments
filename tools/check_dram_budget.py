#!/usr/bin/env python3
"""Fail the build when static internal DRAM (.dram0.data + .dram0.bss) grows
past a budget.

Why (instruments#41): the ESP32-S3 idles with only ~10-15 KiB of free
internal heap once WiFi, BLE, LVGL and the web server are up. Every KiB of
static .bss/.data comes straight out of that heap. When it ran out during
an ArduinoOTA transfer, the WiFi RX path starved and the update failed with
`[ota] error 3 (RECEIVE)`. Large app buffers belong in PSRAM (see
include/psram_static.h and CLAUDE.md "Memory traps"). This check keeps them
from creeping back into internal .bss unnoticed.

Usage:
    tools/check_dram_budget.py .pio/build/esp32-4848s040/firmware.elf [--max-bytes N]

On failure it prints the largest internal-DRAM objects so the offender is
obvious. Pure Python (no toolchain needed), ELF32 little-endian only.
"""

import argparse
import struct
import sys

# Raise this only with a reason in the commit message; prefer moving the
# new buffer to PSRAM instead.
DEFAULT_MAX_BYTES = 88 * 1024

DRAM_SECTIONS = (".dram0.data", ".dram0.bss")
# ESP32-S3 internal SRAM, data-bus view.
DRAM_LO, DRAM_HI = 0x3FC88000, 0x3FD00000


def read_sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit("not an ELF32 little-endian file")
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (name, sh_type, _flags, addr, offset, size, link, _info, _align,
         entsize) = struct.unpack_from("<IIIIIIIIII", data, off)
        raw.append((name, sh_type, addr, offset, size, link, entsize))
    strtab_off = raw[e_shstrndx][3]
    secs = []
    for name, sh_type, addr, offset, size, link, entsize in raw:
        end = data.index(b"\0", strtab_off + name)
        secs.append({
            "name": data[strtab_off + name:end].decode(),
            "type": sh_type, "addr": addr, "offset": offset, "size": size,
            "link": link, "entsize": entsize,
        })
    return secs


def largest_dram_objects(data, secs, limit=15):
    symtab = next((s for s in secs if s["type"] == 2), None)  # SHT_SYMTAB
    if not symtab:
        return []
    strtab = secs[symtab["link"]]
    out = []
    for i in range(symtab["size"] // 16):
        name, value, size, info, _other, _shndx = struct.unpack_from(
            "<IIIBBH", data, symtab["offset"] + i * 16)
        if (info & 0xF) != 1 or not size:  # STT_OBJECT only
            continue
        if not (DRAM_LO <= value < DRAM_HI):
            continue
        start = strtab["offset"] + name
        sym = data[start:data.index(b"\0", start)].decode(errors="replace")
        out.append((size, sym))
    out.sort(reverse=True)
    return out[:limit]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    args = ap.parse_args()

    with open(args.elf, "rb") as f:
        data = f.read()
    secs = read_sections(data)
    sizes = {s["name"]: s["size"] for s in secs if s["name"] in DRAM_SECTIONS}
    missing = [n for n in DRAM_SECTIONS if n not in sizes]
    if missing:
        raise SystemExit(f"sections not found: {', '.join(missing)}")
    total = sum(sizes.values())
    detail = " + ".join(f"{n} {sizes[n]}" for n in DRAM_SECTIONS)
    print(f"static internal DRAM: {total} bytes ({detail}); budget {args.max_bytes}")
    if total <= args.max_bytes:
        return 0
    sys.stdout.flush()
    print(f"FAIL: {total - args.max_bytes} bytes over budget. Largest internal-DRAM objects:",
          file=sys.stderr)
    for size, sym in largest_dram_objects(data, secs):
        print(f"  {size:7d}  {sym}", file=sys.stderr)
    print("Move large app buffers to PSRAM (include/psram_static.h).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
