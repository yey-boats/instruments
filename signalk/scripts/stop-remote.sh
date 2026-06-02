#!/usr/bin/env bash
# Stop the SignalK demo stack on the remote Docker host and kill the
# local fake_boat that was pushing into it.
set -euo pipefail

REMOTE_HOST="${REMOTE_HOST:-nav-server}"
CONTAINER="${SIGNALK_CONTAINER:-signalk-server}"

pkill -f "tools/fake_boat.py" 2>/dev/null || true
ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE_HOST" \
  "docker stop '$CONTAINER' >/dev/null 2>&1 || true"

echo "remote demo stopped (host=$REMOTE_HOST container=$CONTAINER)."
