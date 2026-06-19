#!/usr/bin/env bash
# Wrap espota.py + post-flash verification.
#
# Why: espota.py reports "Error Uploading" when the device finishes
# flashing and reboots before sending the final ACK, even though the
# new firmware is on the device. The exit code is non-zero, so the
# Makefile target fails when the flash actually succeeded.
#
# This wrapper runs espota.py, then verifies success by:
#   1. waiting for the device to come back online (ping or HTTP /);
#   2. embedding a build-time marker in the binary; if the device
#      reports the new marker via /api/state.device.build, success.
#
# Usage:
#   tools/ota_flash.sh <ip> <firmware.bin> [--remote user@host]
#
# Set YEYBOATS_OTA_PASSWORD or OTA_PASSWORD to pass ArduinoOTA auth.
#
# If --remote is given, scp the binary + espota.py to that host and
# run there (used to flash a device on a network only reachable from
# the lab box).

set -euo pipefail

ESPOTA_DEFAULT="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/espota.py"
PORT=3232
# espota.py occasionally leaves the device in a state where it claims STA
# mode internally (BLE `ip` still reports the lease) but the AP's ARP
# table marks it INCOMPLETE - some half-up WiFi condition that survives
# soft reset until something kicks the supplicant. A BLE-driven
# `wifi-reconnect` reliably recovers it. We give the device 30 s to come
# back unaided; if it doesn't, we BLE-kick once and wait the rest.
TIMEOUT_BOOT_SOFT=30
TIMEOUT_BOOT_HARD=120

DEVICE_IP=""
FW=""
REMOTE=""
OTA_AUTH="${YEYBOATS_OTA_PASSWORD:-${OTA_PASSWORD:-}}"
while [ $# -gt 0 ]; do
    case "$1" in
        --remote) REMOTE="$2"; shift 2;;
        --port) PORT="$2"; shift 2;;
        --auth) OTA_AUTH="$2"; shift 2;;
        --espota) ESPOTA_DEFAULT="$2"; shift 2;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0;;
        *)
            if [ -z "${DEVICE_IP}" ]; then DEVICE_IP="$1"
            elif [ -z "${FW}" ]; then FW="$1"
            else echo "unexpected arg: $1" >&2; exit 2
            fi
            shift;;
    esac
done

if [ -z "${DEVICE_IP}" ] || [ -z "${FW}" ]; then
    echo "usage: $0 <ip> <firmware.bin> [--remote user@host] [--port 3232]" >&2
    exit 2
fi
if [ ! -f "${FW}" ]; then
    echo "firmware file not found: ${FW}" >&2
    exit 2
fi

log() { printf '[ota] %s\n' "$*"; }
fail() { printf '[ota] %s\n' "$*" >&2; exit 1; }

# Extract a stable marker from the firmware binary so we can confirm
# the device rebooted into THIS image (and not just any prior boot).
# The build embeds __DATE__ __TIME__ via web.cpp's /api/state.device.build
# field, which is a string baked into firmware.bin. We grep it out and
# compare against what the device reports after the flash. Falls back
# to an md5 of the file as a weaker but always-present identifier.
fw_build_marker() {
    # Look for ASCII runs that match "Mmm  D YYYY HH:MM:SS" (__DATE__ __TIME__).
    if strings -a "${FW}" 2>/dev/null | \
        grep -Eo '[A-Z][a-z]{2}[[:space:]]+[0-9 ]{1,2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}' | \
        head -1; then
        return
    fi
    if command -v md5 >/dev/null 2>&1; then md5 -q "${FW}"
    elif command -v md5sum >/dev/null 2>&1; then md5sum "${FW}" | awk '{print $1}'
    else echo "?"
    fi
}
MARKER="$(fw_build_marker)"
log "expected build marker: ${MARKER}"

run_espota() {
    local auth_args=()
    if [ -n "${OTA_AUTH}" ]; then
        auth_args=(-a "${OTA_AUTH}")
    fi
    if [ -n "${REMOTE}" ]; then
        log "flashing via ${REMOTE}"
        ssh "${REMOTE}" 'rm -rf /tmp/yeydisp-ota && mkdir -p /tmp/yeydisp-ota'
        scp -q "${ESPOTA_DEFAULT}" "${FW}" "${REMOTE}:/tmp/yeydisp-ota/"
        local auth_remote=""
        if [ -n "${OTA_AUTH}" ]; then
            printf -v auth_remote ' -a %q' "${OTA_AUTH}"
        fi
        ssh "${REMOTE}" "python3 /tmp/yeydisp-ota/espota.py -i ${DEVICE_IP} -p ${PORT}${auth_remote} -f /tmp/yeydisp-ota/$(basename "${FW}")" \
            2>&1 | tr '\r' '\n' | grep -E "Sending|Error|Uploading 1|Success|Result" | tail -3 || true
    else
        log "flashing direct (this host -> ${DEVICE_IP})"
        python3 "${ESPOTA_DEFAULT}" -i "${DEVICE_IP}" -p "${PORT}" "${auth_args[@]}" -f "${FW}" \
            2>&1 | tr '\r' '\n' | grep -E "Sending|Error|Uploading 1|Success|Result" | tail -3 || true
    fi
}

curl_state() {
    local cmd="curl -sf --max-time 5 http://${DEVICE_IP}/api/state"
    if [ -n "${REMOTE}" ]; then
        ssh "${REMOTE}" "${cmd}"
    else
        eval "${cmd}"
    fi
}

ping_dev() {
    local cmd="ping -c 1 -W 1 ${DEVICE_IP} >/dev/null 2>&1"
    if [ -n "${REMOTE}" ]; then
        ssh "${REMOTE}" "${cmd}"
    else
        eval "${cmd}"
    fi
}

# --- run flash ---
run_espota || true  # ignore espota's exit code; we verify next

# --- wait for reboot ---
log "waiting up to ${TIMEOUT_BOOT_SOFT}s for device to come back"
deadline=$(( $(date +%s) + TIMEOUT_BOOT_SOFT ))
while [ "$(date +%s)" -lt "${deadline}" ]; do
    if ping_dev; then break; fi
    sleep 2
done
if ! ping_dev; then
    log "device still down after ${TIMEOUT_BOOT_SOFT}s - attempting BLE wifi-reconnect"
    python3 - <<'PY' 2>&1 || true
import asyncio
from bleak import BleakClient, BleakScanner
async def main():
    d = await BleakScanner.find_device_by_filter(
        lambda dev, adv: (dev.name or "").lower().startswith("yey-d"), timeout=15)
    if not d:
        print("no ble device found")
        return
    try:
        async with BleakClient(d) as c:
            await c.write_gatt_char("6e400002-b5a3-f393-e0a3-9f4dd9e3a05a",
                                    b"wifi-reconnect", response=False)
            print("ble: wifi-reconnect sent")
            await asyncio.sleep(5)
    except Exception as e:
        print(f"ble error: {e}")
asyncio.run(main())
PY
    deadline=$(( $(date +%s) + (TIMEOUT_BOOT_HARD - TIMEOUT_BOOT_SOFT) ))
    while [ "$(date +%s)" -lt "${deadline}" ]; do
        if ping_dev; then break; fi
        sleep 2
    done
fi
if ! ping_dev; then
    fail "device did not come back online within ${TIMEOUT_BOOT_HARD}s (even after BLE wifi-reconnect)"
fi

# --- verify build marker ---
# Give the web stack a moment after WiFi rejoin before hammering it.
sleep 2
state="$(curl_state || true)"
if [ -z "${state}" ]; then
    fail "device pingable but /api/state did not respond"
fi
running="$(printf '%s' "${state}" | python3 -c 'import sys, json
try: print(json.load(sys.stdin).get("device",{}).get("build",""))
except Exception: print("")' 2>/dev/null || true)"
log "device reports build: ${running:-(none)}"

if [ -n "${running}" ] && [ -n "${MARKER}" ] && [ "${running}" = "${MARKER}" ]; then
    log "OK - device is running the just-flashed firmware (${running})"
    exit 0
fi
if [ -n "${running}" ] && [ -n "${MARKER}" ]; then
    # Both sides reported a build string and they differ. The device is
    # alive but on OLD firmware - OTA flash genuinely failed (espota
    # error wasn't a false negative; the upload didn't complete and the
    # bootloader rolled back to the previous slot). This is the case the
    # wrapper was added to detect; fail clearly so callers don't proceed
    # as if the new code is running.
    fail "OTA failed: device runs build='${running}', expected='${MARKER}'"
fi
# device.build is empty - likely an older firmware revision that doesn't
# expose __DATE__. Fall back to "alive" without a strong assertion.
log "WARNING - device alive but /api/state.device.build is empty;"
log "          cannot verify the new firmware is running. Treating as"
log "          success on the basis that the device responds."
exit 0
