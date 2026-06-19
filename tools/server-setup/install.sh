#!/usr/bin/env bash
# Idempotent installer for the espdisp nav-server.
# Sets up: Docker, SignalK (persistent systemd service), lab-logger,
# WiFi AP (hostapd + dnsmasq), and ufw firewall rules.
#
# Run on the nav-server as root:
#   sudo bash install.sh [--skip-ap] [--skip-fw] [ESP_LAB_PSK=<psk>]
#
# Or via make (unattended, from dev laptop):
#   make server-setup REMOTE=compulab@192.168.2.11
#
# Options:
#   --skip-ap   Skip WiFi AP setup (useful if AP is managed separately)
#   --skip-fw   Skip ufw firewall configuration
#   --sk-only   Only install/restart the SignalK service (fast re-deploy)

set -euo pipefail

SKIP_AP=0
SKIP_FW=0
SK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --skip-ap) SKIP_AP=1 ;;
        --skip-fw) SKIP_FW=1 ;;
        --sk-only) SK_ONLY=1 ;;
    esac
done

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    echo "install.sh must run as root (re-run with sudo)" >&2
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
SK_DIR="/home/compulab/yeydisp-signalk"
SK_SERVICE="yeydisp-signalk"

log()  { printf '\033[1;34m[server-setup]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[server-setup] ✓\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[server-setup] !\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[server-setup] ✗\033[0m %s\n' "$*" >&2; exit 1; }

# -----------------------------------------------------------------------
# 1. Docker
# -----------------------------------------------------------------------
if [ "$SK_ONLY" -eq 0 ]; then
    if ! command -v docker >/dev/null 2>&1; then
        log "Docker not found – installing via apt"
        apt-get update -qq
        apt-get install -y -qq ca-certificates curl gnupg
        install -m 0755 -d /etc/apt/keyrings
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
            | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
        chmod a+r /etc/apt/keyrings/docker.gpg
        echo \
          "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
          https://download.docker.com/linux/ubuntu \
          $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
          > /etc/apt/sources.list.d/docker.list
        apt-get update -qq
        apt-get install -y -qq docker-ce docker-ce-cli containerd.io
        systemctl enable docker
        systemctl start docker
        ok "Docker installed"
    else
        ok "Docker already present ($(docker --version | head -1))"
    fi
fi

# -----------------------------------------------------------------------
# 2. Pull SignalK image (non-blocking – run in background first time)
# -----------------------------------------------------------------------
if [ "$SK_ONLY" -eq 0 ]; then
    IMAGE="signalk/signalk-server:latest"
    log "pulling $IMAGE (no-op if already cached)"
    docker pull -q "$IMAGE"
    ok "Image ready"
fi

# -----------------------------------------------------------------------
# 3. Create yeydisp-signalk directory structure
# -----------------------------------------------------------------------
log "ensuring $SK_DIR/{config,plugins}"
install -d -m 0775 "$SK_DIR/config/plugin-config-data" \
                    "$SK_DIR/plugins"

# Copy config + plugins from the repo checkout (if present alongside us).
REPO_SK_DIR="$REPO_ROOT/signalk"
if [ -d "$REPO_SK_DIR/config" ]; then
    log "rsyncing signalk/config -> $SK_DIR/config"
    rsync -a --exclude 'node_modules' --exclude '*.log' \
        "$REPO_SK_DIR/config/" "$SK_DIR/config/"
fi
if [ -d "$REPO_SK_DIR/plugins" ]; then
    log "rsyncing signalk/plugins -> $SK_DIR/plugins"
    rsync -a --exclude 'node_modules' --exclude '*.log' \
        "$REPO_SK_DIR/plugins/" "$SK_DIR/plugins/"
fi

# Wide perms so the in-container `node` user (uid 1000) can write
# to the config volume (npm install creates files as uid 1000).
chmod -R a+rwX "$SK_DIR/config" "$SK_DIR/plugins" 2>/dev/null || true
ok "Directories ready"

# -----------------------------------------------------------------------
# 4. Install SignalK systemd service
# -----------------------------------------------------------------------
SERVICE_FILE="/etc/systemd/system/${SK_SERVICE}.service"
log "installing $SERVICE_FILE"
install -m 0644 "$HERE/signalk.service" "$SERVICE_FILE"
# Stamp the resolved SIGNALK_DIR into the unit (avoids per-user override).
sed -i "s|Environment=SIGNALK_DIR=.*|Environment=SIGNALK_DIR=${SK_DIR}|" \
    "$SERVICE_FILE"

systemctl daemon-reload
systemctl enable "$SK_SERVICE"
log "(re)starting $SK_SERVICE – this runs npm install, may take ~30 s"
systemctl restart "$SK_SERVICE"

# Wait up to 60 s for SK HTTP to respond.
for i in $(seq 1 20); do
    if curl -sf http://localhost:3000/signalk >/dev/null 2>&1; then
        ok "SignalK is up at http://localhost:3000"
        break
    fi
    [ "$i" -eq 20 ] && warn "SK did not respond within 60 s – check: journalctl -u $SK_SERVICE"
    sleep 3
done

# -----------------------------------------------------------------------
# 5. Lab UDP log listener
# -----------------------------------------------------------------------
if [ "$SK_ONLY" -eq 0 ]; then
    LOGGER_SRC="$REPO_ROOT/tools/lab-logger"
    if [ -d "$LOGGER_SRC" ]; then
        log "installing lab-logger"
        bash "$LOGGER_SRC/install.sh"
        ok "lab-logger installed"
    else
        warn "tools/lab-logger not found; skipping (repo checkout missing?)"
    fi
fi

# -----------------------------------------------------------------------
# 6. WiFi AP (hostapd + dnsmasq)
# -----------------------------------------------------------------------
if [ "$SK_ONLY" -eq 0 ] && [ "$SKIP_AP" -eq 0 ]; then
    AP_SCRIPT="$REPO_ROOT/signalk/scripts/lab-ap-setup.sh"
    PSK="${ESP_LAB_PSK:-${1:-}}"
    if [ -z "$PSK" ]; then
        warn "ESP_LAB_PSK not set – skipping AP setup (run manually:"
        warn "  ESP_LAB_PSK=<psk> sudo bash $AP_SCRIPT)"
    elif [ -f "$AP_SCRIPT" ]; then
        log "setting up esp-lab AP"
        ESP_LAB_PSK="$PSK" bash "$AP_SCRIPT"
        ok "AP configured"
    else
        warn "lab-ap-setup.sh not found at $AP_SCRIPT"
    fi
fi

# -----------------------------------------------------------------------
# 7. ufw firewall
# -----------------------------------------------------------------------
if [ "$SK_ONLY" -eq 0 ] && [ "$SKIP_FW" -eq 0 ]; then
    if command -v ufw >/dev/null 2>&1; then
        log "applying ufw rules"
        bash "$HERE/ufw-rules.sh"
        ok "ufw configured"
    else
        warn "ufw not installed (apt install ufw) – skipping firewall"
    fi
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------
echo
echo "=============================="
echo " espdisp nav-server status"
echo "=============================="
systemctl --no-pager --full status yeydisp-signalk   2>/dev/null | head -8  || true
systemctl --no-pager --full status yeydisp-lab-ap    2>/dev/null | head -5  || true
systemctl --no-pager --full status yeydisp-loglistener 2>/dev/null | head -5 || true
echo "=============================="
echo "SignalK:    http://$(hostname -I | awk '{print $1}'):3000"
echo "Logs (UDP): sudo tail -f /var/log/yeydisp/device.log"
echo "=============================="
