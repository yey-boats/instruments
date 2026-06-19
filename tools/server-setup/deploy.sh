#!/usr/bin/env bash
# Unattended deploy of server-setup to a remote nav-server.
#
# Usage:  bash tools/server-setup/deploy.sh <user@host> [install-flags...]
#
# Install flags are passed through to install.sh on the remote:
#   --skip-ap   Skip WiFi AP setup
#   --skip-fw   Skip ufw firewall config
#   --sk-only   Only re-install/restart the SignalK service
#
# Examples:
#   bash tools/server-setup/deploy.sh compulab@192.168.2.11
#   bash tools/server-setup/deploy.sh compulab@192.168.2.11 --sk-only
#
# Auth (same precedence as lab-logger/deploy.sh):
#   1. Passwordless sudo on the remote.
#   2. REMOTE_SUDO_PASS in env or .env.test.local (gitignored).
#   3. Script exits with a one-line fix — no interactive fallback.

set -euo pipefail

REMOTE="${1:-}"
shift || true
INSTALL_FLAGS="$*"

if [ -z "${REMOTE}" ]; then
    echo "usage: $0 <user@host> [--skip-ap] [--skip-fw] [--sk-only]" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

# The SignalK manager plugin + its config/scripts now live in the sibling
# yey-boats/Instruments-manager repo, which deploys SignalK from its own
# deploy/ tree. Clone it next to this repo (../signalk-espdisp-manager) or
# set MANAGER_DIR to point elsewhere; it is referenced in operator-facing
# messages below.
MANAGER_DIR="${MANAGER_DIR:-${REPO_ROOT}/../signalk-espdisp-manager}"

# Pull REMOTE_SUDO_PASS from .env.test.local if not in env.
if [ -z "${REMOTE_SUDO_PASS:-}" ] && [ -f "${REPO_ROOT}/.env.test.local" ]; then
    REMOTE_SUDO_PASS="$(awk '
        /^[[:space:]]*(export[[:space:]]+)?REMOTE_SUDO_PASS=/ {
            sub(/^[[:space:]]*(export[[:space:]]+)?REMOTE_SUDO_PASS=/, "")
            sub(/^"/, ""); sub(/"$/, "")
            sub(/^'\''/, ""); sub(/'\''$/, "")
            print; exit
        }' "${REPO_ROOT}/.env.test.local")" || true
fi

# Pull ESP_LAB_PSK from .env.test.local if not in env.
if [ -z "${ESP_LAB_PSK:-}" ] && [ -f "${REPO_ROOT}/.env.test.local" ]; then
    ESP_LAB_PSK="$(awk '
        /^[[:space:]]*(export[[:space:]]+)?ESP_LAB_PSK=/ {
            sub(/^[[:space:]]*(export[[:space:]]+)?ESP_LAB_PSK=/, "")
            sub(/^"/, ""); sub(/"$/, "")
            sub(/^'\''/, ""); sub(/'\''$/, "")
            print; exit
        }' "${REPO_ROOT}/.env.test.local")" || true
fi

log() { printf '[server-setup] %s\n' "$*"; }
die() { printf '[server-setup] %s\n' "$*" >&2; exit 1; }

# The SignalK config/plugins and the manager plugin itself moved out of this
# firmware repo into yey-boats/Instruments-manager. This installer used to ship
# $REPO_ROOT/signalk/{config,plugins,scripts/lab-ap-setup.sh}; those trees are
# gone. SignalK/manager provisioning is therefore deployed from the
# Instruments-manager repo's own deploy/ tree, not from here.
#
# --sk-only used to re-push just the SK config/plugins from signalk/. That
# source no longer exists, so point the operator at the manager repo and bail.
case " ${INSTALL_FLAGS} " in
    *" --sk-only "*)
        die "SignalK/manager deployment moved to the yey-boats/Instruments-manager repo (${MANAGER_DIR}). Deploy SignalK from there (deploy/scripts/run-remote.sh / deploy/README.md) instead of 'server-sk-only'."
        ;;
esac

REMOTE_TMP="/tmp/yeydisp-server-setup"
INSTALL_CMD="bash ${REMOTE_TMP}/tools/server-setup/install.sh ${INSTALL_FLAGS}"

# --- Upload the relevant repo subtrees ---
log "uploading repo -> ${REMOTE}:${REMOTE_TMP}"
ssh -o BatchMode=yes "${REMOTE}" \
    "rm -rf '${REMOTE_TMP}' && mkdir -p '${REMOTE_TMP}'"

# Ship server-setup scripts and lab-logger.
rsync -az \
    "$REPO_ROOT/tools/server-setup/" \
    "${REMOTE}:${REMOTE_TMP}/tools/server-setup/"
rsync -az \
    "$REPO_ROOT/tools/lab-logger/" \
    "${REMOTE}:${REMOTE_TMP}/tools/lab-logger/"

log "upload complete"

# --- Run install.sh with sudo on remote ---
if ssh -o BatchMode=yes "${REMOTE}" 'sudo -n true' 2>/dev/null; then
    log "passwordless sudo available; running install"
    ssh -o BatchMode=yes "${REMOTE}" \
        "ESP_LAB_PSK='${ESP_LAB_PSK:-}' sudo -E -n ${INSTALL_CMD}"
elif [ -n "${REMOTE_SUDO_PASS:-}" ]; then
    log "using REMOTE_SUDO_PASS via sudo -S (unattended)"
    printf '%s\n' "${REMOTE_SUDO_PASS}" | \
        ssh -o BatchMode=yes "${REMOTE}" \
        "ESP_LAB_PSK='${ESP_LAB_PSK:-}' sudo -S -p '' ${INSTALL_CMD}"
else
    die "no sudo path. Set REMOTE_SUDO_PASS in .env.test.local or configure passwordless sudo."
fi

log "deploy complete"
