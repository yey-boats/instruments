# Manager ↔ Device screen sync (and the MIDL cutover)

**Status:** the MIDL screen cutover changes how the SignalK **instruments manager**
(`yey-boats-display-manager`) drives a device's screens. During the transition the
legacy link and the MIDL link coexist; this doc explains both, what currently
breaks the link, and how to sync them back.

## TL;DR

- The **default firmware build** still honors the manager: it fetches the
  manager's config and renders manager-driven screens (legacy v2 model). **The
  link is intact in the default build.**
- The **`YEYBOATS_MIDL_ONLY` build breaks the link on purpose**: it compiles out
  the built-in screens *and* gates off the manager screen apply
  (`src/app_events.cpp` `ApplyManagedScreens` → `#ifndef YEYBOATS_MIDL_ONLY`), and
  boots a **baked** MIDL doc instead. It is a dev/preview build, not
  manager-synced.
- The MIDL delivery path that will re-establish the link **does not exist yet**:
  the device serves only `GET /api/midl/manifest` (capability advertisement).
  There is **no `POST /api/midl/config`**, no SignalK MIDL pull, and no flash
  persistence of a MIDL doc. Those are firmware **Slice 3 + Slice 4** (see
  roadmap) and manager **Plan 4 Slice 5**.

**To sync a device to the manager right now: flash a normal build (no
`YEYBOATS_MIDL_ONLY`).** The MIDL-only build is intentionally manager-independent
until the MIDL delivery path lands.

## The legacy link (how it works in the default build)

The manager plugin runs on the SignalK host (`mythra-nav`). The device's
`manager.cpp` worker:

1. **Registers / heartbeats** to the plugin, authenticating with
   `X-YeyBoats-Authorization: Bearer <token>` (device/dev/provision token).
2. **Fetches config**: `fetch_config()` → `GET /devices/<device_id>/config`
   (`manager.cpp:1767`). A **push-live** re-fetch is triggered by
   `request_config_fetch()` (`manager.cpp:2313`), wired to the manager's
   config-push signal.
3. **Applies screens**: the fetched render plan is posted as a
   `CommandType::ApplyManagedScreens` app-event (`manager.cpp:1685`) →
   `manager_screens::apply(RenderPlan)` on the UI task → registers the
   manager-defined screens (the `yeyboats.dashboard.v2` widgets+tiles model).

So in the default build: edit a dashboard in the manager → it pushes →
the device re-fetches and re-renders. That is the link.

## What the MIDL cutover changes

The device is moving from the v2 widgets+tiles model to **MIDL documents**
(`screens → elements → layout`, rendered by the native MIDL renderer). Two
divergences result:

1. **Model**: the manager currently authors/stores `yeyboats.dashboard.v2`; the
   MIDL renderer consumes MIDL docs. The manager carries a **bidirectional
   v2 ↔ MIDL adapter** (manager Plan 4) so the two can coexist during the
   transition.
2. **Transport on the device**: manager screens arrive via
   `ApplyManagedScreens` (RenderPlan). MIDL docs will arrive via a **new MIDL
   ingress** (below) — which is not built yet.

In the `YEYBOATS_MIDL_ONLY` build both legacy paths are suppressed, so that build
shows only the baked MIDL doc and ignores the manager entirely.

## Target MIDL sync contract (what re-establishes the link)

Per the runtime spec (`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md`
§3.6–§3.8) and the manager migration plan:

```
Manager (yey-boats-display-manager)                 Device (firmware)
 ─ authors/stores a dashboard (v2 today,             ─ serves GET /api/midl/manifest
   MIDL after Plan 4 Slice 4)                          (capability advertisement) ✅ done
 ─ resolves it to the device's class +               ─ validates incoming doc vs its
   migrates to the device's MIDL version               own embedded manifest (render-safety)
 ─ validates vs the device's manifest                ─ persists to LittleFS:
 ─ delivers the resolved MIDL doc ───────────────▶     current / last-known-good / factory
   • PRIMARY: POST /api/midl/config   (Slice 4 ❌)    ─ applies on the UI task (apply_all)
   • FALLBACK: SignalK pull           (Slice 4 ❌)    ─ A/B commit + boot-loop rollback (Slice 3 ❌)
```

Auth reuses the existing device-token header (`X-YeyBoats-Authorization`), not the
web Basic Auth. The device's manifest at `/api/midl/manifest` is the contract the
manager validates against before posting (so it never sends an inadmissible doc).

## Work required to sync them back (with status)

**Firmware (this repo, branch `feat/midl-firmware-render`):**
- ✅ `GET /api/midl/manifest` — served (manager can already validate against it).
- ✅ Multi-screen render (`apply_all`) + `ConfigApplyMidl` app-event (accepts a
  doc blob) — the apply side is ready; nothing posts to it over HTTP yet.
- ❌ **Slice 4 — delivery**: add `POST /api/midl/config` (manager → device) +
  SignalK pull fallback + `POST /api/midl/reset?to=default|previous`. This is the
  endpoint the manager will push to.
- ❌ **Slice 3 — flash persistence**: LittleFS current/last-known-good/factory so
  a pushed doc survives reboot; `YEYBOATS_MIDL_ONLY` then boots the **flash** doc
  instead of the baked demo.

**Manager (`../signalk-espdisp-manager`, plan
`docs/superpowers/plans/2026-06-19-yb-midl-manager-migration.md`):**
- Plan 4 Slices 1–4: v2↔MIDL adapter, validate-before-post, catalog-driven editor,
  author MIDL natively.
- **Plan 4 Slice 5 — Device cutover**: "post MIDL to devices; remove v2 path."
  Explicitly gated on firmware accepting MIDL (this work). Once firmware Slice 4
  lands, the manager posts MIDL to `POST /api/midl/config` instead of (or
  alongside) the v2 RenderPlan.

## Operational guidance during the transition

| You want… | Do this |
|---|---|
| **Manager-driven screens (sync intact)** | Flash a **default build** (no `YEYBOATS_MIDL_ONLY`). The manager push → device re-fetch → render still works. |
| **MIDL preview on a device** | Flash `-D YEYBOATS_MIDL_ONLY=1` (boots the baked MIDL doc; **not** manager-synced) or use the `midl-render` console command on a default build. |
| **Re-establish sync on the MIDL path** | Land firmware Slice 4 (`POST /api/midl/config` + flash) and manager Plan 4 Slice 5 (post MIDL). Then the manager drives MIDL docs the same way it drove v2 plans. |

Until Slice 4 + manager Slice 5 are done, treat `YEYBOATS_MIDL_ONLY` as a
**lab/dev build** and keep manager-managed lab devices on the default build.

## Re-sync checklist (once the delivery path exists)

1. Firmware: `POST /api/midl/config` accepts a resolved MIDL doc, validates vs the
   embedded manifest, persists to LittleFS (current; promotes last-known-good on a
   clean render), applies via `apply_all`.
2. Manager: on dashboard save, resolve+migrate+validate the doc against the
   device's `/api/midl/manifest`, then `POST /api/midl/config` with the
   `X-YeyBoats-Authorization` device token (and/or write the SignalK pull path).
3. Verify: edit in the manager → device re-renders the MIDL screens; reboot →
   device loads the persisted doc from flash; `POST /api/midl/reset?to=default`
   restores the factory MIDL doc.
