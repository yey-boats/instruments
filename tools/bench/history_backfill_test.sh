#!/usr/bin/env bash
# Trend-history backfill test kit for the bench SignalK (docker).
#
#   tools/bench/history_backfill_test.sh install   # install + enable the memory provider, make it default, restart
#   tools/bench/history_backfill_test.sh check     # does the History API return data for the trend path yet?
#   tools/bench/history_backfill_test.sh remove    # disable it and restore the previous default provider
#
# Env: SK_CONTAINER (yey-bench-display-signalk), SK_URL (http://127.0.0.1:3400),
#      HIST_PATH (environment.wind.speedTrue), WINDOW_S (600), POINTS (120).
#
# The display side (what the test proves) is in docs/bench/history-backfill-test.md.
set -euo pipefail

SK_CONTAINER=${SK_CONTAINER:-yey-bench-display-signalk}
SK_URL=${SK_URL:-http://127.0.0.1:3400}
HIST_PATH=${HIST_PATH:-environment.wind.speedTrue}
WINDOW_S=${WINDOW_S:-600}
POINTS=${POINTS:-120}
HERE=$(cd "$(dirname "$0")" && pwd)
SKDIR=/home/node/.signalk
ID=yey-memory-history

sk() { docker exec "$SK_CONTAINER" sh -c "$1"; }

case "${1:-}" in
  install)
    docker cp "$HERE/signalk-memory-history" "$SK_CONTAINER:/home/node/$ID"
    sk "cd $SKDIR && npm install --no-audit --no-fund /home/node/$ID >/dev/null"
    sk "mkdir -p $SKDIR/plugin-config-data && printf '%s' '{\"enabled\":true,\"configuration\":{\"retentionMinutes\":60}}' > $SKDIR/plugin-config-data/$ID.json"
    # Remember the current default provider, then make ours the default.
    sk "cd $SKDIR && [ -f settings.json.pre-$ID ] || cp settings.json settings.json.pre-$ID"
    sk "cd $SKDIR && node -e \"const f='settings.json',fs=require('fs'),s=JSON.parse(fs.readFileSync(f));s.historyApi=Object.assign({},s.historyApi,{defaultProvider:'$ID'});fs.writeFileSync(f,JSON.stringify(s,null,2))\""
    docker restart "$SK_CONTAINER" >/dev/null
    for _ in $(seq 1 40); do curl -sf -o /dev/null "$SK_URL/signalk" && break; sleep 2; done
    echo "installed; providers: $(curl -s "$SK_URL/signalk/v2/api/history/_providers")"
    echo "let it record for ${WINDOW_S}s, then: $0 check"
    ;;
  check)
    step=$(awk "BEGIN{printf \"%.3f\", $WINDOW_S/$POINTS}")
    url="$SK_URL/signalk/v2/api/history/values?paths=$HIST_PATH:average&duration=PT${WINDOW_S}S&resolution=$step"
    curl -s "$url" | python3 -c '
import json, sys
d = json.load(sys.stdin)
rows = d.get("data", [])
pts = int(sys.argv[1])
print(len(rows), "of", pts, "buckets have data, range", d.get("range"))
if rows: print("first", rows[0], "last", rows[-1])
sys.exit(0 if len(rows) >= pts * 0.8 else 1)' "$POINTS"
    ;;
  remove)
    sk "cd $SKDIR && [ -f settings.json.pre-$ID ] && mv settings.json.pre-$ID settings.json || true"
    sk "printf '%s' '{\"enabled\":false}' > $SKDIR/plugin-config-data/$ID.json"
    docker restart "$SK_CONTAINER" >/dev/null
    echo "disabled; previous default provider restored"
    ;;
  *) sed -n 2,13p "$0"; exit 2 ;;
esac
