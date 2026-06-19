#!/usr/bin/env bash
# Idempotent installer for the espdisp lab UDP log listener.
#
# Run with sudo on the lab machine (e.g. compulab):
#   sudo bash install.sh
# or remotely from the dev box:
#   tar -czf - -C tools/lab-logger . | \
#     ssh -t compulab@... 'mkdir -p /tmp/llg && tar -xzf - -C /tmp/llg && sudo bash /tmp/llg/install.sh'
#
# The Makefile target `make lab-logger-deploy` automates the second form.

set -euo pipefail

if [ "${EUID}" -ne 0 ]; then
    echo "install.sh must run as root (re-run with sudo)" >&2
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR=/opt/yeydisp-loglistener
SERVICE=yeydisp-loglistener.service
LOGROTATE_DST=/etc/logrotate.d/espdisp

echo "[install] copying listener -> ${INSTALL_DIR}"
install -d -m 0755 "${INSTALL_DIR}"
install -m 0755 "${HERE}/loglistener.py" "${INSTALL_DIR}/loglistener.py"

echo "[install] installing systemd unit -> /etc/systemd/system/${SERVICE}"
install -m 0644 "${HERE}/${SERVICE}" "/etc/systemd/system/${SERVICE}"

echo "[install] installing logrotate config -> ${LOGROTATE_DST}"
install -m 0644 "${HERE}/espdisp.logrotate" "${LOGROTATE_DST}"

echo "[install] systemctl daemon-reload"
systemctl daemon-reload

echo "[install] enable + (re)start ${SERVICE}"
systemctl enable "${SERVICE}"
systemctl restart "${SERVICE}"

# Validate logrotate config without rotating yet (would refuse on syntax error).
if command -v logrotate >/dev/null 2>&1; then
    echo "[install] validating logrotate config"
    logrotate -d "${LOGROTATE_DST}" >/dev/null
fi

echo
systemctl --no-pager --full status "${SERVICE}" | head -10 || true
echo
echo "[install] done. Tail logs with:"
echo "  sudo tail -f /var/log/yeydisp/device.log"
echo "  sudo journalctl -u ${SERVICE} -f"
