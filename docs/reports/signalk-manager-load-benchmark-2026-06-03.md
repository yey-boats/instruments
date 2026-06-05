# SignalK Manager Load Benchmark - 2026-06-03

## Scope

This benchmark exercises the ESP Display Manager control plane on the lab
SignalK server:

- `POST /plugins/espdisp-manager/devices/register`
- `POST /plugins/espdisp-manager/devices/:id/status`
- `GET /plugins/espdisp-manager/devices/:id/config`
- `GET /plugins/espdisp-manager/devices/:id/commands`

It does not benchmark SignalK WebSocket fanout to real displays.

## Environment

| Item | Value |
| --- | --- |
| SignalK URL | `http://192.168.2.11:3000` |
| Remote host | `compulab@192.168.2.11` |
| Container | `signalk` |
| Container status at start | `Up 21 hours` |
| Heartbeat interval | `30000 ms` |
| Command poll interval | `15000 ms` |
| Config fetch | every 10 heartbeats |
| Duration per completed level | about 307 seconds |
| Result directory | `.tmp/load-bench-20260603-130803` |

## Commands

Initial matrix command:

```sh
LEVELS="10 20 50 100" \
DURATION_SEC=300 \
SIGNALK_URL=http://192.168.2.11:3000 \
REMOTE=compulab@192.168.2.11 \
SIGNALK_CONTAINER=signalk \
SIGNALK_USERNAME=admin \
SIGNALK_PASSWORD=admin \
signalk/plugins/signalk-espdisp-manager/tools/load-matrix.sh
```

Report regeneration command:

```sh
SUMMARY_ONLY=1 \
OUT_DIR=.tmp/load-bench-20260603-130803 \
LEVELS="10 20 50" \
signalk/plugins/signalk-espdisp-manager/tools/load-matrix.sh
```

## Results

| Devices | Status | Errors | Heartbeat p95 / p99 | Config p95 / p99 | Commands p95 / p99 | Avg CPU | Max CPU | Avg Mem | Max Mem |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | complete | 0 | 38.1 / 42.8 ms | 56.9 / 69.9 ms | 70.7 / 107.1 ms | 7.16% | 100.02% | 5.54% | 5.59% |
| 20 | complete | 0 | 42.7 / 57.6 ms | 27.3 / 83.8 ms | 93.7 / 101.9 ms | 6.30% | 94.86% | 5.57% | 5.64% |
| 50 | interrupted after registration | unknown | not measured | not measured | not measured | invalid | invalid | invalid | invalid |
| 100 | not run | not measured | not measured | not measured | not measured | not measured | not measured | not measured | not measured |

Detailed completed-level operation summaries:

```text
10 devices:
register    count=10  ok=10  p95=33.4 ms  p99=33.4 ms
heartbeat   count=100 ok=100 p95=38.1 ms  p99=42.8 ms
config      count=100 ok=100 p95=56.9 ms  p99=69.9 ms
commands    count=200 ok=200 p95=70.7 ms  p99=107.1 ms

20 devices:
register    count=20  ok=20  p95=26.6 ms  p99=31.0 ms
heartbeat   count=200 ok=200 p95=42.7 ms  p99=57.6 ms
config      count=200 ok=200 p95=27.3 ms  p99=83.8 ms
commands    count=400 ok=400 p95=93.7 ms  p99=101.9 ms
```

## Findings

- 10 and 20 simulated devices are clean on the current lab server: no HTTP
  errors and sub-110 ms p99 for steady-state heartbeat/config/command traffic.
- CPU averages stayed low, around 6-7% for completed levels.
- CPU max values near or above 100% are short spikes from `docker stats`
  samples, most likely around registration, config generation, and synchronous
  registry persistence.
- Memory stayed stable for completed levels, around 199-203 MiB. The partial
  50-device run reached about 221 MiB.
- The 50-device run completed registration but was interrupted before the
  steady-state latency summary. It is not valid evidence that 50 devices are
  safe for sustained operation.
- After the interruption, `devices-50/docker-stats.log` kept receiving samples.
  That means the old stats sampler survived the interrupted shell process. The
  50-device CPU and memory averages are therefore contaminated by idle samples
  and must not be used for capacity conclusions.

## Rerun Attempt

The matrix was rerun from this Codex workspace with `RESUME=1`, but the
workspace is not permitted to open SSH sessions to the lab host:

```text
ssh: connect to host 192.168.2.11 port 22: Operation not permitted
```

The same command with escalated network access was also rejected by the local
runner policy, so no new lab benchmark data was produced from this session.

## Runner Issue Fixed

The first matrix run printed:

```text
date: invalid argument 's' for -I
```

Cause: `date -Is` is not portable across the local/remote environments used in
the benchmark. The runner now uses portable UTC timestamps:

```sh
date -u '+%Y-%m-%dT%H:%M:%SZ'
```

Additional runner hardening added:

- `RESUME=1` skips already completed levels.
- `SUMMARY_ONLY=1` regenerates `summary.txt` from existing logs.
- Ctrl-C/TERM cleanup stops the remote stats sampler.
- Ctrl-C/TERM now records `INTERRUPTED=1` and writes `diagnostics.txt` for the
  active level before exiting.
- Incomplete levels are explicitly marked in the summary.
- CPU/memory summary now includes averages and max values.
- Rerunning a level now moves previous `load-test.log` and `docker-stats.log`
  files aside with a timestamp before opening new logs. This prevents a stale
  sampler from appending into the new benchmark file.

## Recommendation

Treat 20 devices as validated for this lab setup.

Do not treat 50 or 100 devices as validated until a full uninterrupted run is
completed. The expected next run is:

```sh
RESUME=1 \
OUT_DIR=.tmp/load-bench-20260603-130803 \
LEVELS="10 20 50 100" \
DURATION_SEC=300 \
SIGNALK_URL=http://192.168.2.11:3000 \
REMOTE=compulab@192.168.2.11 \
SIGNALK_CONTAINER=signalk \
SIGNALK_USERNAME=admin \
SIGNALK_PASSWORD=admin \
signalk/plugins/signalk-espdisp-manager/tools/load-matrix.sh
```

Before relying on 50+ devices, implement the async persistence architecture
documented in `docs/architecture/signalk-manager-load-and-async.md`: debounce
registry writes, cache generated config hashes, and expose plugin metrics.
