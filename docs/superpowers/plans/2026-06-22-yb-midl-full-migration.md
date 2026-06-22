# YB-MIDL Full Migration — Device + SignalK Manager

> Goal: complete the migration to MIDL end-to-end. The device renders MIDL
> screens from flash, accepts MIDL over the wire, and the SignalK instruments
> manager (`yey-boats-display-manager`) authors and delivers MIDL natively —
> restoring (and upgrading) the manager↔device link on the new architecture.

Builds on: the screen cutover roadmap (`2026-06-22-yb-midl-screen-cutover-roadmap.md`,
which owns the per-screen list + element extensions) and the manager migration
plan (`2026-06-19-yb-midl-manager-migration.md`). Branch `feat/midl-firmware-render`.

## Where we are
- ✅ Firmware renders MIDL multi-screen (`apply_all`), per-screen PSRAM arenas, nav.
- ✅ Pure host-tested select/find; `GET /api/midl/manifest` served.
- ❌ No way for the manager (or the on-device editor) to *deliver* a MIDL doc.
- ❌ No flash persistence — a doc doesn't survive reboot; `YEYBOATS_MIDL_ONLY`
  boots a baked demo, not a managed doc.
- ❌ Manager still authors `yeyboats.dashboard.v2` and pushes RenderPlans (the
  legacy link, gated off in MIDL-only builds — see `docs/midl/manager-device-sync.md`).

## Phase A — Device delivery + persistence (restores the link)

**A1. Delivery API (firmware).**
- `POST /api/midl/config` — body = MIDL JSON doc. Pipeline: bounded read → parse
  (PSRAM) → **validate** (structural + capability vs the embedded manifest;
  render-safety, not gating) → persist (A2) → `apply_all` on the UI task. Returns
  `200 {ok, screens:N}` or `4xx {errors:[{path,msg}]}`.
- `GET /api/midl/config` — current doc (verbatim).
- `POST /api/midl/reset?to=default|previous` — restore factory or last-known-good.
- **Auth:** accept EITHER web Basic Auth OR a valid `X-YeyBoats-Authorization`
  device token (so the manager pushes with the token it already holds). Document.
- Reuses `ConfigApplyMidl` app-event + `apply_all` (already built).

**A2. Flash persistence (firmware, LittleFS).** Per spec §3.7:
- Partition: add a small LittleFS region to `partitions.csv` (verify OTA/app fit).
- `midl_store`: `factory` (baked rodata), `current`, `last-known-good` files.
  Pure tier logic host-tested; LittleFS I/O device-only.
- Boot: load `current` (else factory). A/B: promote `current`→`last-known-good`
  after a clean render + stability window; boot-loop counter (NVS) reverts.
- `YEYBOATS_MIDL_ONLY` (and eventually the default build) boots the **flash** doc.

**A3. Raise `maxTiles`.** POD growth (`MAX_TILES_PER_SCREEN` → 8) + bump
`square-480` `maxTiles` in the catalog/manifest; re-run the POD-size guard.

**A4. Manager pull integration (firmware).** Extend `manager.cpp::fetch_config`
to accept a MIDL doc from the manager (a `midl` field or a `/devices/:id/midl`
route) and route it through the same validate→persist→apply path, reusing the
`request_config_fetch` push-live trigger. So the manager can deliver MIDL by
**push** (A1) or **pull** (A4).

## Phase B — Manager MIDL delivery (`../signalk-espdisp-manager`)

Follows the manager migration plan (Plan 4), coordinated with Phase A:
- Slices 1–4 (adapter, validate-before-post, catalog-driven editor, author MIDL):
  the manager validates a doc against the device's `/api/midl/manifest` and stores
  MIDL (v2↔MIDL adapter bridges the legacy editor during transition).
- **Slice 5 — device cutover:** on save, resolve+migrate the MIDL doc to the
  device's class/version and **deliver it** to `POST /api/midl/config` (or the
  pull route), with the `X-YeyBoats-Authorization` device token. Keep v2 behind
  the adapter until devices are all on MIDL, then remove the v2 push path.

## Phase C — Full screen fidelity
Per the screen roadmap: implement all 12 screens, adding extensions as each needs
them — **E1 button actions**, **E2 compass markers**, **E3 trend**, **E4 size**,
**E5 local bindings**, **E6 computed bindings**, **E7 HUD/wind-dial composite
elements** (catalog + painter + manifest + schema, MIDL-core dep-free), **E8
status-list**. Each new catalog element keeps the `dispatch-covers-catalog` guard
meaningful and is screenshot-validated on the bench.

## Phase D — Cutover
- Default build boots the full MIDL screen set from flash (factory default doc
  ships the legacy screens as MIDL).
- Manager delivers MIDL; v2 path removed.
- Retire legacy `screen_*.cpp` where MIDL covers them (config screens stay native).
- Soak + screenshot parity vs the legacy renders.

## Execution order (driving now)
A1 (delivery POST/GET/reset) → A3 (maxTiles) → A2 (flash) → A4 (manager pull) →
Phase B (manager push) → Phase C screens (in parallel once delivery exists) →
Phase D cutover. Each device-side step screenshot-validated; each shared MIDL
change keeps host tests + catalog guards green.

## Invariants
MIDL core (`midl/cpp`, validators) zero external deps; firmware ArduinoJson behind
it. Memory traps (PSRAM, memset-in-place, LVGL-on-UI-task, no big stack temps).
On-device validation = render-safety, not gating (security/auth out of scope per
prior decision; real web creds set in CI build later). Every device claim
screenshot-verified.
