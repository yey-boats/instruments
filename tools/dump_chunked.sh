#!/bin/bash
# Chunked flash backup with per-chunk retries.
# Works around flaky CH340 SLIP framing by keeping each esptool invocation small.
# Resumable: re-running skips already-completed chunks.
set -u

PORT=${PORT:-/dev/cu.usbserial-110}
BAUD=${BAUD:-115200}
CHUNK=$((64 * 1024))
TOTAL=$((16 * 1024 * 1024))
RETRIES=${RETRIES:-6}
OUT=${OUT:-backup/full_flash_16MB.bin}
CHUNKDIR=${CHUNKDIR:-backup/chunks}

mkdir -p "$CHUNKDIR"

ok=0
fail=0
for ((offset = 0; offset < TOTAL; offset += CHUNK)); do
    name=$(printf "%08x" "$offset")
    cf="$CHUNKDIR/chunk_${name}.bin"
    if [ -f "$cf" ] && [ "$(stat -f%z "$cf")" -eq "$CHUNK" ]; then
        ok=$((ok + 1))
        continue
    fi
    got=0
    for ((try = 1; try <= RETRIES; try++)); do
        if esptool.py --port "$PORT" --baud "$BAUD" \
            read_flash "$offset" "$CHUNK" "$cf" >/dev/null 2>&1; then
            sz=$(stat -f%z "$cf" 2>/dev/null || echo 0)
            if [ "$sz" -eq "$CHUNK" ]; then
                got=1
                break
            fi
        fi
        rm -f "$cf"
        sleep 1
    done
    if [ "$got" -eq 1 ]; then
        ok=$((ok + 1))
        printf "ok   0x%s  (%d/%d)\n" "$name" "$ok" $((TOTAL / CHUNK))
    else
        fail=$((fail + 1))
        printf "FAIL 0x%s\n" "$name"
    fi
done

if [ "$fail" -gt 0 ]; then
    echo "incomplete: $fail chunk(s) missing, rerun to resume"
    exit 1
fi

cat "$CHUNKDIR"/chunk_*.bin >"$OUT"
sz=$(stat -f%z "$OUT")
echo "wrote $OUT ($sz bytes)"
shasum -a 256 "$OUT"
