#!/usr/bin/env bash
# Bring up the SignalK demo stack on a remote Docker host (default:
# compulab@192.168.2.11), then run fake_boat locally so it pushes
# synthetic deltas into the remote SK server.
#
# Override defaults:
#   REMOTE_HOST=user@host SK_HOST=host SK_PORT=3000 ./run-remote.sh
#
# The remote host needs: docker, rsync, ssh access without password
# prompts (key-based auth recommended).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SK_DIR="$ROOT/signalk"

REMOTE_HOST="${REMOTE_HOST:-compulab@192.168.2.11}"
REMOTE_DIR="${REMOTE_DIR:-/home/compulab/espdisp-signalk}"
SK_HOST="${SK_HOST:-${REMOTE_HOST##*@}}"
SK_PORT="${SK_PORT:-3000}"
CONTAINER="${SIGNALK_CONTAINER:-signalk-server}"
IMAGE="${SIGNALK_IMAGE:-signalk/signalk-server:latest}"
PYTHON="${PYTHON:-/usr/bin/python3}"

echo "==> remote host: $REMOTE_HOST  (sk endpoint: $SK_HOST:$SK_PORT)"
echo "==> remote dir:  $REMOTE_DIR"

# 0. quick reachability + dependency check
ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE_HOST" \
  'command -v docker >/dev/null || { echo "docker not on PATH on remote" >&2; exit 1; }'

# 1. sync config + plugins to the remote so changes here propagate
ssh "$REMOTE_HOST" "mkdir -p '$REMOTE_DIR/config/plugin-config-data' '$REMOTE_DIR/plugins'"
rsync -az --delete \
  --exclude 'node_modules' --exclude '*.log' \
  "$SK_DIR/config/" "$REMOTE_HOST:$REMOTE_DIR/config/"
rsync -az --delete \
  --exclude 'node_modules' --exclude '*.log' \
  "$SK_DIR/plugins/" "$REMOTE_HOST:$REMOTE_DIR/plugins/"

# The signalk image runs as uid 1000 (node) inside the container.  On
# Linux remotes (unlike macOS docker desktop), bind mounts enforce
# strict ownership, so npm install would fail with EACCES against the
# files we just rsynced as the SSH user.  a+rwX is fine on a dev host
# and matches what `docker run --rm -v ...` expects to see.
# `|| true` — node_modules from a prior npm install is owned by the
# in-container `node` user (uid 1000) and we can't chown those as the
# SSH user.  Those files already have the right perms; we only need
# to widen the rsync'd files (configs, plugin sources).
ssh "$REMOTE_HOST" "chmod -R a+rwX '$REMOTE_DIR/config' '$REMOTE_DIR/plugins' 2>/dev/null || true"

# 2. (re)create the container on the remote.  We replicate run.sh's
#    sequence (npm install in the volume, then a detached server) so
#    plugin changes from the synced tree take effect immediately.
ssh "$REMOTE_HOST" REMOTE_DIR="$REMOTE_DIR" CONTAINER="$CONTAINER" IMAGE="$IMAGE" bash -s <<'REMOTE'
set -euo pipefail
if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
  docker rm -f "$CONTAINER" >/dev/null
fi
docker run --rm \
  --entrypoint npm \
  -v "$REMOTE_DIR/config:/home/node/.signalk" \
  -v "$REMOTE_DIR/plugins:/home/node/plugins" \
  -w /home/node/.signalk \
  "$IMAGE" \
  install
docker run -d \
  --name "$CONTAINER" \
  --network host \
  -v "$REMOTE_DIR/config:/home/node/.signalk" \
  -v "$REMOTE_DIR/plugins:/home/node/plugins" \
  "$IMAGE" >/dev/null
# --network host: the container sees every interface on the box
# (eth0 + wlan-ap0), so UDP broadcasts from the esp-lab subnet land
# on the SK discovery responder.  This is required for the device's
# 34300 discovery probe to be answered from compulab.
REMOTE

# 3. poll until the remote SK answers HTTP, then start fake_boat locally
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  if "$PYTHON" - "$SK_HOST" "$SK_PORT" <<'PY'
import sys, urllib.request
host, port = sys.argv[1], int(sys.argv[2])
try:
    urllib.request.urlopen(f"http://{host}:{port}/signalk", timeout=2)
except Exception:
    raise SystemExit(1)
PY
  then
    break
  fi
  sleep 1
done

pkill -f "tools/fake_boat.py" 2>/dev/null || true
nohup "$PYTHON" -u "$ROOT/tools/fake_boat.py" "$SK_HOST" "$SK_PORT" \
  >/tmp/fake_boat.log 2>&1 &

cat <<EOF
SignalK at http://$SK_HOST:$SK_PORT
NMEA 0183 TCP at $SK_HOST:10110
ESP display SignalK discovery UDP at $SK_HOST:34300
ESP display device announcement UDP at $SK_HOST:34301
fake_boat log: /tmp/fake_boat.log  (running locally, targeting remote SK)

Test env: source $ROOT/.env.test
EOF
