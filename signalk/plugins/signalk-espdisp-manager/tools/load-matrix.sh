#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
PLUGIN_DIR="$ROOT/signalk/plugins/signalk-espdisp-manager"

SIGNALK_URL="${SIGNALK_URL:-http://192.168.2.11:3000}"
REMOTE="${REMOTE:-compulab@192.168.2.11}"
CONTAINER="${SIGNALK_CONTAINER:-signalk}"
LEVELS="${LEVELS:-10 20 50 100}"
DURATION_SEC="${DURATION_SEC:-300}"
HEARTBEAT_MS="${HEARTBEAT_MS:-30000}"
COMMAND_POLL_MS="${COMMAND_POLL_MS:-15000}"
CONFIG_EVERY="${CONFIG_EVERY:-10}"
SAMPLE_SEC="${SAMPLE_SEC:-5}"
USERNAME="${SIGNALK_USERNAME:-admin}"
PASSWORD="${SIGNALK_PASSWORD:-admin}"
DEVICE_TOKEN="${ESPDISP_MANAGER_TOKEN:-espdisp-dev}"
OUT_DIR="${OUT_DIR:-$ROOT/.tmp/load-bench-$(date +%Y%m%d-%H%M%S)}"
RESUME="${RESUME:-0}"
SUMMARY_ONLY="${SUMMARY_ONLY:-0}"

mkdir -p "$OUT_DIR"

CURRENT_REMOTE_STOP=""
CURRENT_SAMPLER_PID=""
CURRENT_LEVEL_DIR=""
CURRENT_DEVICES=""

usage() {
  cat <<EOF
Usage:
  SIGNALK_URL=http://192.168.2.11:3000 \\
  REMOTE=compulab@192.168.2.11 \\
  LEVELS="10 20 50 100" \\
  DURATION_SEC=300 \\
  signalk/plugins/signalk-espdisp-manager/tools/load-matrix.sh

Environment:
  SIGNALK_URL       SignalK root URL. Default: $SIGNALK_URL
  REMOTE            SSH host for docker stats. Default: $REMOTE
  SIGNALK_CONTAINER Container name. Default: $CONTAINER
  LEVELS            Device counts to test. Default: "$LEVELS"
  DURATION_SEC      Duration per level. Default: $DURATION_SEC
  SAMPLE_SEC        docker stats sample interval. Default: $SAMPLE_SEC
  SIGNALK_USERNAME  SignalK login username. Default: admin
  SIGNALK_PASSWORD  SignalK login password. Default: admin
  SIGNALK_TOKEN     Optional existing SignalK bearer token.
  ESPDISP_MANAGER_TOKEN Device/dev token. Default: espdisp-dev
  OUT_DIR           Result directory. Default: $OUT_DIR
  RESUME            Skip levels whose meta.txt has EXIT_CODE=0. Default: 0
  SUMMARY_ONLY      Only regenerate summary.txt from OUT_DIR. Default: 0
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

log() {
  printf '[load-matrix] %s\n' "$*"
}

timestamp() {
  date -u '+%Y-%m-%dT%H:%M:%SZ'
}

timestamp_file() {
  date -u '+%Y%m%d-%H%M%S'
}

cleanup_current() {
  if [[ -n "$CURRENT_REMOTE_STOP" ]]; then
    ssh "$REMOTE" "touch '$CURRENT_REMOTE_STOP'" 2>/dev/null || true
  fi
  if [[ -n "$CURRENT_SAMPLER_PID" ]]; then
    kill "$CURRENT_SAMPLER_PID" 2>/dev/null || true
    wait "$CURRENT_SAMPLER_PID" 2>/dev/null || true
  fi
  if [[ -n "$CURRENT_REMOTE_STOP" ]]; then
    ssh "$REMOTE" "rm -f '$CURRENT_REMOTE_STOP'" 2>/dev/null || true
  fi
  CURRENT_REMOTE_STOP=""
  CURRENT_SAMPLER_PID=""
}

handle_signal() {
  local rc=130
  if [[ -n "$CURRENT_LEVEL_DIR" && -n "$CURRENT_DEVICES" ]]; then
    printf 'END=%s\nEXIT_CODE=%s\nINTERRUPTED=1\n' "$(timestamp)" "$rc" >>"$CURRENT_LEVEL_DIR/meta.txt" 2>/dev/null || true
    cleanup_current
    write_failure_diagnostics "$CURRENT_LEVEL_DIR" "$CURRENT_DEVICES" "$rc" || true
  else
    cleanup_current
  fi
  exit "$rc"
}

trap 'handle_signal' INT TERM

sample_stats() {
  local out="$1"
  local stop_file="$2"
  ssh "$REMOTE" "while [ ! -f '$stop_file' ]; do \
    printf '%s ' \"\$(date -u '+%Y-%m-%dT%H:%M:%SZ')\"; \
    docker stats --no-stream --format 'cpu={{.CPUPerc}} mem={{.MemUsage}} mempct={{.MemPerc}} net={{.NetIO}} block={{.BlockIO}} pids={{.PIDs}}' '$CONTAINER' 2>/dev/null || true; \
    sleep '$SAMPLE_SEC'; \
  done" >"$out"
}

write_failure_diagnostics() {
  local level_dir="$1"
  local devices="$2"
  local rc="$3"
  local load_log="$level_dir/load-test.log"
  local stats_log="$level_dir/docker-stats.log"
  local diag="$level_dir/diagnostics.txt"

  {
    echo "diagnostics_created=$(timestamp)"
    echo "devices=$devices"
    echo "exit_code=$rc"
    echo
    echo "== interpretation =="
    case "$rc" in
      0) echo "completed successfully" ;;
      124) echo "timeout wrapper terminated the benchmark" ;;
      130) echo "interrupted by SIGINT/Ctrl-C" ;;
      137) echo "killed by SIGKILL; check host OOM killer or external kill" ;;
      143) echo "terminated by SIGTERM" ;;
      *) echo "non-zero exit; inspect logs below" ;;
    esac
    echo
    echo "== load-test tail =="
    tail -n 80 "$load_log" 2>/dev/null || echo "load-test log unavailable"
    echo
    echo "== docker stats tail =="
    tail -n 40 "$stats_log" 2>/dev/null || echo "docker stats log unavailable"
    echo
    echo "== remote docker state =="
    ssh "$REMOTE" "docker ps --filter name='$CONTAINER' --format '{{.Names}} {{.Status}}'; docker stats --no-stream --format 'cpu={{.CPUPerc}} mem={{.MemUsage}} mempct={{.MemPerc}} net={{.NetIO}} block={{.BlockIO}} pids={{.PIDs}}' '$CONTAINER' 2>/dev/null || true" 2>&1 || true
    echo
    echo "== remote related processes =="
    ssh "$REMOTE" "ps -eo pid,ppid,stat,pcpu,pmem,etime,args | grep -E 'signalk|node|npm|load-test|docker stats' | grep -v grep" 2>&1 || true
  } >"$diag"
  log "diagnostics written: $diag"
}

run_level() {
  local devices="$1"
  local prefix="loadtest-${devices}"
  local level_dir="$OUT_DIR/devices-${devices}"
  local stats_log="$level_dir/docker-stats.log"
  local load_log="$level_dir/load-test.log"
  local meta_file="$level_dir/meta.txt"
  local remote_stop="/tmp/espdisp-load-matrix-stop-${devices}-$$"

  mkdir -p "$level_dir"
  for existing_log in "$stats_log" "$load_log"; do
    if [[ -e "$existing_log" ]]; then
      mv "$existing_log" "${existing_log}.previous-$(timestamp_file)"
    fi
  done
  CURRENT_LEVEL_DIR="$level_dir"
  CURRENT_DEVICES="$devices"
  cat >"$meta_file" <<EOF
SIGNALK_URL=$SIGNALK_URL
REMOTE=$REMOTE
CONTAINER=$CONTAINER
DEVICES=$devices
DURATION_SEC=$DURATION_SEC
HEARTBEAT_MS=$HEARTBEAT_MS
COMMAND_POLL_MS=$COMMAND_POLL_MS
CONFIG_EVERY=$CONFIG_EVERY
SAMPLE_SEC=$SAMPLE_SEC
PREFIX=$prefix
START=$(timestamp)
EOF

  ssh "$REMOTE" "rm -f '$remote_stop'"
  CURRENT_REMOTE_STOP="$remote_stop"
  sample_stats "$stats_log" "$remote_stop" &
  local sampler_pid=$!
  CURRENT_SAMPLER_PID="$sampler_pid"

  log "running level devices=$devices duration=${DURATION_SEC}s"
  local cmd=(
    npm run load-test --prefix "$PLUGIN_DIR" --
    --url "$SIGNALK_URL"
    --devices "$devices"
    --duration-sec "$DURATION_SEC"
    --heartbeat-ms "$HEARTBEAT_MS"
    --command-poll-ms "$COMMAND_POLL_MS"
    --config-every "$CONFIG_EVERY"
    --prefix "$prefix"
    --device-token "$DEVICE_TOKEN"
  )
  if [[ -n "${SIGNALK_TOKEN:-}" ]]; then
    cmd+=(--signalk-token "$SIGNALK_TOKEN")
  else
    cmd+=(--username "$USERNAME" --password "$PASSWORD")
  fi

  set +e
  "${cmd[@]}" >"$load_log" 2>&1
  local rc=$?
  set -e

  ssh "$REMOTE" "touch '$remote_stop'"
  wait "$sampler_pid" 2>/dev/null || true
  ssh "$REMOTE" "rm -f '$remote_stop'"
  CURRENT_REMOTE_STOP=""
  CURRENT_SAMPLER_PID=""
  printf 'END=%s\nEXIT_CODE=%s\n' "$(timestamp)" "$rc" >>"$meta_file"
  if [[ "$rc" -ne 0 ]]; then
    write_failure_diagnostics "$level_dir" "$devices" "$rc"
  fi
  CURRENT_LEVEL_DIR=""
  CURRENT_DEVICES=""

  if [[ "$rc" -ne 0 ]]; then
    log "level devices=$devices failed; see $load_log"
    return "$rc"
  fi
  log "level devices=$devices complete"
}

summarize() {
  local summary="$OUT_DIR/summary.txt"
  {
    echo "SignalK ESP Display Manager load matrix"
    echo "created=$(timestamp)"
    echo "out_dir=$OUT_DIR"
    echo
    for devices in $LEVELS; do
      local level_dir="$OUT_DIR/devices-${devices}"
      local load_log="$level_dir/load-test.log"
      local stats_log="$level_dir/docker-stats.log"
      echo "== devices=$devices =="
      if [[ -f "$load_log" ]]; then
        if ! awk '/Load test summary/{found=1; flag=1} flag{print} END{exit found ? 0 : 1}' "$load_log"; then
          echo "load test incomplete or interrupted"
          tail -n 20 "$load_log"
        fi
      else
        echo "load-test.log missing"
      fi
      echo
      echo "docker stats samples:"
      if [[ -s "$stats_log" ]]; then
        awk '
          {
            for (i = 1; i <= NF; i++) {
              if ($i ~ /^cpu=/) {
                cpu = $i
                sub(/^cpu=/, "", cpu)
                sub(/%$/, "", cpu)
                cpu += 0
                total_cpu += cpu
                if (cpu > max_cpu) max_cpu = cpu
              }
              if ($i ~ /^mempct=/) {
                mempct = $i
                sub(/^mempct=/, "", mempct)
                sub(/%$/, "", mempct)
                mempct += 0
                total_mempct += mempct
                if (mempct > max_mempct) max_mempct = mempct
              }
            }
          }
          END {
            avg_cpu = NR ? total_cpu / NR : 0
            avg_mempct = NR ? total_mempct / NR : 0
            printf "avg_cpu_pct=%.2f max_cpu_pct=%.2f avg_mem_pct=%.2f max_mem_pct=%.2f samples=%d\n", avg_cpu, max_cpu, avg_mempct, max_mempct, NR
          }
        ' "$stats_log"
        tail -n 3 "$stats_log"
      else
        echo "docker stats unavailable"
      fi
      echo
    done
  } >"$summary"
  cat "$summary"
}

log "output: $OUT_DIR"

if [[ "$SUMMARY_ONLY" == "1" ]]; then
  summarize
  exit 0
fi

ssh "$REMOTE" "docker ps --filter name='$CONTAINER' --format '{{.Names}} {{.Status}}' && docker stats --no-stream '$CONTAINER' >/dev/null"

failed=0
for devices in $LEVELS; do
  if [[ "$RESUME" == "1" ]] &&
    grep -q '^EXIT_CODE=0$' "$OUT_DIR/devices-${devices}/meta.txt" 2>/dev/null; then
    log "skipping completed level devices=$devices"
    continue
  fi
  if ! run_level "$devices"; then
    failed=1
    break
  fi
done

summarize
exit "$failed"
