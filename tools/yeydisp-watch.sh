#!/bin/bash
# yeydisp lab watcher. Runs forever; one log line per sample. Captures
# reachability + internal heap + chip temp so we can correlate any future
# L2 disassociation or heap pressure with what the device was doing at
# the time.
#
# Designed to be run as a systemd service on the lab AP host. See
# tools/yeydisp-watch.service for the unit file. Logs land in
# /var/log/yeydisp/watch.log with built-in daily rotation; install
# tools/yeydisp-watch.logrotate into /etc/logrotate.d/ to keep ~14 days.

set -u

DEVICE_IP="${YEYBOATS_DEVICE_IP:-10.42.0.67}"
DEVICE_MAC="${YEYBOATS_DEVICE_MAC:-28:37:2f:8a:02:90}"
INTERVAL_S="${YEYBOATS_WATCH_INTERVAL_S:-60}"
LOG_DIR="${YEYBOATS_WATCH_LOG_DIR:-/var/log/yeydisp}"
LOG_FILE="${LOG_DIR}/watch.log"

mkdir -p "$LOG_DIR" 2>/dev/null || {
    # Fall back to /tmp if no permission for /var/log/yeydisp.
    LOG_DIR=/tmp
    LOG_FILE="${LOG_DIR}/yeydisp-watch.log"
}

log() {
    # ISO-8601 timestamp + the rest of args. Single line per sample so
    # `tail -F` + grep work without parser tricks.
    local t="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "${t} $*" >> "$LOG_FILE"
}

probe_once() {
    local ping_rtt="UNREACHABLE"
    local state_json=""
    local fields="state=fail"

    # 1 ICMP probe, 1 s deadline. Short timeout matches the device's
    # web-task heap-breaker so a 503 / latency excursion is visible.
    local ping_out
    ping_out=$(ping -c 1 -W 1 "$DEVICE_IP" 2>/dev/null || true)
    if [[ -n "$ping_out" ]]; then
        ping_rtt=$(echo "$ping_out" | sed -n 's/.*time=\([0-9.]*\) ms.*/\1/p' | head -1)
        if [[ -z "$ping_rtt" ]]; then ping_rtt="UNREACHABLE"; fi
    fi

    state_json=$(curl -sS --max-time 3 "http://${DEVICE_IP}/api/state" 2>/dev/null || true)
    if [[ -n "$state_json" ]]; then
        # NOTE: `python3 - <<EOF` would replace stdin with the heredoc,
        # killing the pipe. Use `-c` so the pipe stays connected.
        fields=$(printf '%s' "$state_json" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    dev = d.get("device", {})
    mgr = d.get("manager", {})
    sk = d.get("sk", {})
    print("state=ok int_free={} largest={} min_ever={} psram={} temp={} uptime={} sk={} mgr_reg={} mgr_hb={}".format(
        dev.get("heap_internal_free", "?"),
        dev.get("heap_internal_largest", "?"),
        dev.get("heap_min_ever", "?"),
        dev.get("psram_free", "?"),
        dev.get("chip_temp_c", "?"),
        dev.get("uptime_ms", "?"),
        sk.get("state", "?"),
        mgr.get("lastRegisterCode", "?"),
        mgr.get("lastHeartbeatCode", "?"),
    ))
except Exception as e:
    print("state=parse-fail err={}".format(e))
' 2>/dev/null)
        if [[ -z "$fields" ]]; then fields="state=parse-empty"; fi
    fi

    log "ping=${ping_rtt}ms ${fields}"
}

log "=== watcher start pid=$$ device=${DEVICE_IP} mac=${DEVICE_MAC} interval=${INTERVAL_S}s log=${LOG_FILE} ==="
trap 'log "=== watcher exit pid=$$ signal=$? ==="' EXIT
while true; do
    probe_once
    sleep "$INTERVAL_S"
done
