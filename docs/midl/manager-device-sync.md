# Manager ↔ Device screen sync

**Superseded by [midl-only.md](midl-only.md)** (owner decision, 2026-11-15):
the display renders MIDL screens only and Display Manager delivers MIDL
records only. This page is kept as a short map of where things live.

## Current state (firmware)

- Default build = MIDL records (`YEYBOATS_MIDL_RECORDS`, `include/build_config.h`).
  Boot renders the record owning `index.defaultScreen` from the record store:
  embedded built-in records (`midl/library/builtin`, per display class) plus
  manager/local records persisted on LittleFS under `/midl/`.
- Delivery:
  - **push** (default): `PUT /api/midl/records/<id>`, `DELETE …`,
    `PUT /api/midl/index`, `POST /api/midl/reset?to=factory`, authenticated with
    the manager device token (`X-YeyBoats-Authorization: Bearer …`).
    `POST /api/midl/config` still exists as an alias for a local record.
  - **pull**: the `midl` block of `GET /devices/<id>/config`
    (`index` + `records[{id,title,hash,url}]`); changed records are fetched,
    unlisted manager records deleted.
- Reporting: heartbeat `ui.screen/record/screens` + `midl{version,class,
  indexHash,records}`; identity `capabilities.midl`.
- Code: `src/midl_records.cpp` (pure store logic, host-tested),
  `src/midl_store.cpp` (persistence + API), `src/midl_store_ui.cpp`
  (activation/navigation, UI task), `src/web.cpp` (push API),
  `src/manager.cpp` (`pull_midl_records`, heartbeat).
- `-D YEYBOATS_LEGACY_SCREENS=1` still builds the old C++ screens, managed
  screens (`layout`/`widgets`) and the RAM-only `/api/midl/config`; the removal
  is a follow-up PR. The old `YEYBOATS_MIDL_ONLY` flag is gone (MIDL is the
  default now).
