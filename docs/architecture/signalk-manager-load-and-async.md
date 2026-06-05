# SignalK Manager Load and Async Design

## Current Load Model

Each ESP display creates two kinds of SignalK server load:

- SignalK data stream: one WebSocket subscription to `signalk/v1/stream`.
- Manager control plane: REST calls to the `espdisp-manager` plugin.

The current firmware requests 24 SignalK paths with `period=1000` ms,
`minPeriod=200` ms, and `policy=instant`. With the repo simulator
(`tools/fake_boat.py`), this is normally one delta per second carrying the
changed navigation values.

Manager traffic is lower rate:

- `POST /plugins/espdisp-manager/devices/:id/status` every 30 seconds.
- `GET /plugins/espdisp-manager/devices/:id/commands` every 15 seconds.
- `GET /plugins/espdisp-manager/devices/:id/config` only on config drift or
  explicit reload.

Approximate steady-state manager request rate:

```text
requests/sec ~= devices * (1 / heartbeatSeconds + 1 / commandPollSeconds)
             ~= devices * 0.10  at 30 s heartbeat and 15 s command poll
```

Examples:

| Devices | Manager REST requests/s |
| ---: | ---: |
| 20 | 2 |
| 50 | 5 |
| 100 | 10 |

The current bottleneck is not raw request count. The plugin currently rewrites
the full registry JSON synchronously on every heartbeat, so heartbeat handling
scales with registry size and storage latency.

## Benchmark

Run the manager control-plane load test from the plugin directory:

```sh
npm run load-test -- --url http://localhost:3000 --devices 20 --duration-sec 180
```

Remote lab example:

```sh
npm run load-test -- \
  --url http://192.168.2.11:3000 \
  --devices 50 \
  --duration-sec 300 \
  --heartbeat-ms 30000 \
  --command-poll-ms 15000 \
  --config-every 10 \
  --username admin \
  --password admin
```

The script registers synthetic devices and then runs heartbeat and command-poll
loops with startup jitter. It reports request counts, errors, RPS, and latency
percentiles per operation.

If the server already has a token, use `--signalk-token <token>` or set
`SIGNALK_TOKEN`. The ESP manager device/dev token is separate; pass it with
`--device-token` or `ESPDISP_MANAGER_TOKEN`.

Run a comparison matrix with SignalK container CPU/memory sampling:

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

This creates `.tmp/load-bench-<timestamp>/` with one directory per load level:

- `load-test.log`: manager REST latency/error summary.
- `docker-stats.log`: sampled `docker stats` output from the SignalK host.
- `meta.txt`: benchmark parameters and exit status.
- `summary.txt`: combined comparison with max CPU/memory percentages.

Suggested pass/fail thresholds for the lab SignalK server:

| Metric | Target |
| --- | --- |
| Error rate | 0 for steady state |
| Heartbeat p95 | < 250 ms |
| Heartbeat p99 | < 500 ms |
| Command poll p95 | < 200 ms |
| SignalK UI/API responsiveness | No visible lag while test runs |
| Container CPU | Sustained < 70% of one core |
| Node event loop delay | p95 < 100 ms once instrumented |

The benchmark currently covers the manager REST control plane. SignalK
WebSocket fanout should be benchmarked separately with synthetic data clients
if we need to validate 100+ displays with high-frequency real sensors.

## Proper Async Mechanism

The manager should separate hot-path device traffic from durable persistence.

Recommended internal architecture:

```text
HTTP handlers
  -> validate auth/body
  -> update in-memory DeviceRegistry
  -> enqueue persistence/event work
  -> respond immediately

DeviceRegistry
  -> Map<deviceId, DeviceRecord>
  -> tracks lastSeen/status/config/firmware/commands in memory
  -> marks dirty domains: registry, commands, jobs, discovery

PersistenceWorker
  -> debounces dirty domains, e.g. flush after 1-5 s
  -> coalesces many heartbeats into one registry write
  -> writes atomic JSON snapshots
  -> retries failed writes and exposes dirty/lastFlush/error metrics

Command/EventBus
  -> internal async queue for device events
  -> consumers: persistence, audit log, UI notifications, optional metrics
```

Implementation details:

- Replace direct `store.saveRegistry()` in heartbeat with `store.markDirty('registry')`.
- Keep status heartbeat data in memory and flush snapshots periodically.
- Cache generated config hash per device profile/override version so heartbeat
  does not call `generateConfig()` twice.
- Store large volatile status separately from durable identity/config state.
- Flush immediately for operator actions that must survive restart before the
  HTTP response returns, such as firmware job creation, command creation, token
  rotation, and preset save.
- Add `/plugins/espdisp-manager/metrics` with device count, dirty domains,
  flush latency, flush failures, request counters, and event-loop delay.

## MQTT Decision

Do not add MQTT just to fix this bottleneck.

MQTT would decouple producers and consumers, but the current overload risk is
inside the plugin: synchronous JSON persistence and repeated config generation.
Adding MQTT before fixing that would add another service and failure mode while
leaving the hot path expensive.

Use the internal async queue first. It is enough for:

- device status ingestion,
- command delivery state,
- config drift detection,
- firmware job progress,
- UI refresh metrics.

MQTT becomes useful later if we want one of these:

- boat-wide event bus shared by multiple services,
- retained last-known device status outside SignalK,
- remote/federated displays across network boundaries,
- replayable telemetry stream independent of the plugin process,
- integration with Home Assistant, Node-RED, or a fleet backend.

If MQTT is added later, keep SignalK as the operator/control API and publish
derived events from the internal bus to MQTT. Do not make device management
depend on MQTT for normal local operation.
