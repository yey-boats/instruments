#!/usr/bin/env bash
# Unattended deploy of the espdisp lab UDP log listener.
#
# Usage:   bash tools/lab-logger/deploy.sh <user@host>
#
# Auth precedence (no interactive prompts, ever):
#
#   1. If `ssh <host> sudo -n true` succeeds (i.e. the remote user has
#      passwordless sudo configured already), we just run the install.
#   2. Otherwise we look for REMOTE_SUDO_PASS in:
#        - the environment
#        - <repo>/.env.test.local (gitignored)
#      and feed it to `sudo -S` over an SSH stdin pipe. No tty needed.
#   3. Otherwise the script fails with a one-line fix.
#
# This is the entry point for `make lab-logger-deploy` and `make lab-up`.

set -euo pipefail

REMOTE="${1:-}"
if [ -z "${REMOTE}" ]; then
    echo "usage: $0 <user@host>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

# Pull REMOTE_SUDO_PASS from .env.test.local if not already in env.
# Tolerant parser: `KEY=value` or `export KEY=value`, optional quotes.
if [ -z "${REMOTE_SUDO_PASS:-}" ] && [ -f "${REPO_ROOT}/.env.test.local" ]; then
    REMOTE_SUDO_PASS="$(awk '
        /^[[:space:]]*(export[[:space:]]+)?REMOTE_SUDO_PASS=/ {
            sub(/^[[:space:]]*(export[[:space:]]+)?REMOTE_SUDO_PASS=/, "")
            sub(/^"/, ""); sub(/"$/, "")
            sub(/^'\''/, ""); sub(/'\''$/, "")
            print
            exit
        }' "${REPO_ROOT}/.env.test.local")" || true
fi

log() { printf '[lab-logger] %s\n' "$*"; }
die() { printf '[lab-logger] %s\n' "$*" >&2; exit 1; }

INSTALL_CMD='bash /tmp/yeydisp-lab-logger/install.sh'

# --- upload (no sudo) ---
log "uploading tools/lab-logger/ -> ${REMOTE}:/tmp/yeydisp-lab-logger"
ssh -o BatchMode=yes "${REMOTE}" \
    'rm -rf /tmp/yeydisp-lab-logger && mkdir -p /tmp/yeydisp-lab-logger'
tar -czf - -C "${HERE}" . | \
    ssh -o BatchMode=yes "${REMOTE}" 'tar -xzf - -C /tmp/yeydisp-lab-logger'

# --- install (sudo) ---
if ssh -o BatchMode=yes "${REMOTE}" 'sudo -n true' 2>/dev/null; then
    log "passwordless sudo available; running install"
    ssh -o BatchMode=yes "${REMOTE}" "sudo -n ${INSTALL_CMD}"
elif [ -n "${REMOTE_SUDO_PASS:-}" ]; then
    log "using REMOTE_SUDO_PASS via sudo -S (unattended)"
    # `sudo -p ''` suppresses the prompt so the password line itself
    # doesn't end up in any captured output. printf avoids ps-leak of
    # the password as a command-line arg.
    printf '%s\n' "${REMOTE_SUDO_PASS}" | \
        ssh -o BatchMode=yes "${REMOTE}" "sudo -S -p '' ${INSTALL_CMD}"
else
    die "no sudo path available. Either configure passwordless sudo for the
       remote user, or put REMOTE_SUDO_PASS=<password> in .env.test.local
       (gitignored). I refuse to prompt - this target is supposed to run
       unattended from CI / cron / orchestrators."
fi

log "deploy complete"
