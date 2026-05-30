#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SK_DIR="$ROOT/signalk"
CONFIG_DIR="$SK_DIR/config"
CONTAINER="${SIGNALK_CONTAINER:-signalk-server}"
IMAGE="${SIGNALK_IMAGE:-signalk/signalk-server:latest}"
SK_HOST="${SK_HOST:-localhost}"
SK_PORT="${SK_PORT:-3000}"
PYTHON="${PYTHON:-/usr/bin/python3}"

mkdir -p "$CONFIG_DIR/plugin-config-data"

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
  docker rm -f "$CONTAINER" >/dev/null
fi

docker run --rm \
  --entrypoint npm \
  -v "$CONFIG_DIR:/home/node/.signalk" \
  -v "$SK_DIR/plugins:/home/node/plugins" \
  -w /home/node/.signalk \
  "$IMAGE" \
  install

docker run -d \
  --name "$CONTAINER" \
  -p 3000:3000 \
  -p 10110:10110 \
  -p 34300:34300/udp \
  -p 34301:34301/udp \
  -v "$CONFIG_DIR:/home/node/.signalk" \
  -v "$SK_DIR/plugins:/home/node/plugins" \
  "$IMAGE" >/dev/null

for _ in 1 2 3 4 5 6 7 8 9 10; do
  if "$PYTHON" - "$SK_HOST" "$SK_PORT" <<'PY'
import sys
import urllib.request

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

echo "SignalK at http://$SK_HOST:$SK_PORT"
echo "NMEA 0183 TCP at $SK_HOST:10110"
echo "ESP display SignalK discovery UDP at $SK_HOST:34300"
echo "ESP display device announcement UDP at $SK_HOST:34301"
echo "fake_boat log: /tmp/fake_boat.log"
