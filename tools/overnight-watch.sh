#!/usr/bin/env bash
# Overnight stability watch: poll /api/state via the lab compulab
# bridge every 60 s and record a single CSV line per probe. Reboot
# events are deduced from uptime regressions across consecutive
# successful polls (the same logic espdisp watch uses now that it
# correctly skips failed polls).
#
# Output: /var/log/yeydisp/overnight-<YYYYMMDD>.csv
# Columns: iso_ts, uptime_s, heap_kb, psram_kb, sk_state, mgr_hb,
#          sk_iters, fetch_ok
#
# A `summary` line is appended every 10 polls so a glance at the
# tail tells you the running totals.

set -u
LOG_DIR=${LOG_DIR:-/var/log/yeydisp}
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/overnight-$(date +%Y%m%d).csv"

DEVICE_IP=${DEVICE_IP:-10.42.0.67}
INTERVAL=${INTERVAL:-60}

echo "# $(date -Iseconds) overnight watch started, device=${DEVICE_IP}, interval=${INTERVAL}s" >> "${LOG_FILE}"
echo "iso_ts,uptime_s,heap_kb,psram_kb,sk_state,mgr_hb,sk_iters,fetch_ok" >> "${LOG_FILE}"

prev_uptime=0
reboot_count=0
poll_count=0
fail_count=0

while true; do
    ts=$(date -Iseconds)
    state=$(curl -sf --max-time 10 "http://${DEVICE_IP}/api/state" 2>/dev/null || true)
    poll_count=$((poll_count + 1))
    if [ -z "${state}" ]; then
        fail_count=$((fail_count + 1))
        echo "${ts},,,,,,,0" >> "${LOG_FILE}"
    else
        # Single-pass python parser; avoid jq dep so this runs anywhere.
        # Plain dict access + .format() avoids f-string escape conflicts
        # that python 3.11 on compulab rejects with `f-string expression
        # part cannot include a backslash`.
        line=$(printf '%s' "${state}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
dev = d["device"]; sk = d["sk"]; mgr = d["manager"]
print("{},{},{},{},{},{},1".format(
    dev["uptime_ms"]//1000,
    dev["heap_free"]//1024,
    dev["psram_free"]//1024,
    sk.get("state",""),
    mgr.get("lastHeartbeatCode",0),
    sk.get("task_iters",0),
))
' 2>/dev/null)
        if [ -n "${line}" ]; then
            uptime_s=$(printf '%s' "${line}" | cut -d, -f1)
            if [ "${prev_uptime}" -gt 0 ] && [ "${uptime_s}" -gt 0 ] && [ "${uptime_s}" -lt "${prev_uptime}" ]; then
                reboot_count=$((reboot_count + 1))
                echo "# REBOOT detected: prev_uptime=${prev_uptime}s, now=${uptime_s}s, count=${reboot_count}" >> "${LOG_FILE}"
            fi
            prev_uptime=${uptime_s}
            echo "${ts},${line}" >> "${LOG_FILE}"
        else
            fail_count=$((fail_count + 1))
            echo "${ts},,,,,,,0" >> "${LOG_FILE}"
        fi
    fi
    if [ $((poll_count % 10)) -eq 0 ]; then
        echo "# summary @ ${ts}: polls=${poll_count}, fails=${fail_count}, reboots=${reboot_count}, last_uptime=${prev_uptime}s" >> "${LOG_FILE}"
    fi
    sleep "${INTERVAL}"
done
