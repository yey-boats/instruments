# Knob: testing & simulation

How the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** (`waveshare-knob-1_8`, 360×360
round rotary remote) is verified. The knob is a remote controller — encoder +
push button driving an autopilot-home menu and a select-display / select-view
control of other MFDs over the `espdisp-manager` plugin.

This page is written for the case where the **hardware is not in hand yet**. It
draws a hard line between what is already proven in software (and reproducible on
any host with no device) and what can only be confirmed once the physical knob
arrives — with the exact procedure for each.

> See also: [Deploy & use the remote knob](remote-knob.md),
> the design spec
> [`superpowers/specs/2026-06-13-waveshare-knob-remote-design.md`](superpowers/specs/2026-06-13-waveshare-knob-remote-design.md),
> and the implementation plan
> [`superpowers/plans/2026-06-13-waveshare-knob-remote.md`](superpowers/plans/2026-06-13-waveshare-knob-remote.md).

## Software verification (no hardware needed)

Everything in this section runs on a developer host — no ESP32, no panel, no
SignalK server hardware. It exercises the **real** menu state machine, the
**real** SignalK-PUT formatter, the **real** view/overlay rendering code, and the
**real** plugin endpoints the firmware depends on.

| What | Command | What it proves |
|------|---------|----------------|
| Knob menu state machine + PUT formatter + gesture journeys | `pio test -e native -f test_knob_menu` | 24 host tests: every menu level/gesture (±1/±5, wrap, back-nav), the pure `knob::signalk_put_for` SignalK-PUT formatter, and multi-step *gesture-journey* tests that simulate realistic knob operation and assert the emitted PUT path/value at each step. |
| Full host suite (no regression) | `pio test -e native` | 218 tests — parser, layout, autopilot, manager config, and the knob menu — all green on the host. |
| Round-view + menu-overlay renders | `make sim` | Renders the four round views and the three menu-overlay states headlessly at 360×360 through the real view/overlay code, with bounds assertions. Outputs to `docs/sim-shots/`. |
| Plugin device-projection + wire contract | `cd signalk/plugins/signalk-espdisp-manager && npm test` | `device-projections.test.js` covers `GET /devices/summary` and `GET /devices/:id/views`; `knob-contract.test.js` pins the exact JSON keys the firmware parser reads, plus an end-to-end route test. |
| Control-protocol C++ conformance (the shared controller path) | `pio test -e native -f test_proto` | Round-trips the protocol fixtures through the generated C++ records, the version-compat matrix (same-major accept / major-mismatch reject / unknown-field ignore), auth accept/deny, and the session table (attach / heartbeat / reap / last-writer-wins). This is the **same code the knob runs to drive a display**. |
| Control-protocol JS lib | `cd proto/js && npm test` | `@espdisp/proto` ajv validators + version/auth helpers over the same `proto/fixtures/*.json` — keeps the C++ and JS libs in lockstep. |
| Plugin protocol control | `cd signalk/plugins/signalk-espdisp-manager && npm test` (`proto-control.test.js`) | Validates the plugin's outbound `attach`/`switch`/`detach` against the schema and round-trips them against a mock `/api/p2p/*` device. |
| Control-frame renders | `make sim` | Renders the per-controller colored frame (1 and 3 stacked controllers) at 480×480 square and 360 round headlessly through the real overlay code, with bounds assertions → `docs/sim-shots/control-frame-*.png`. |
| Firmware build (knob) | `pio run -e waveshare-knob-1_8` | The knob firmware compiles for the ESP32-S3 target. |
| Firmware build (no regression) | `pio run -e esp32-4848s040` | The production Sunton firmware still builds — adding the knob variant did not regress it. |
| Firmware build (harness) | `pio run -e harness-s3-devkitc` | The headless ESP32-S3-DevKitC-1 control-protocol harness compiles — the on-hardware controller verifier (see below). |

### Evidence: round views

The four dedicated round views, rendered at 360×360 by the `make sim` harness
(`sim-knob` env) through the real `ui/knob_ui.cpp` painters:

<p align="center">
  <img src="sim-shots/knob-gallery.png" alt="Knob round views: Autopilot HUD, Compass, Wind angle, Big number" width="480">
</p>

### Evidence: menu overlays

The three menu-overlay states — mode picker, Select Display (which also
demonstrates list paging / windowing), and Select View — driven through the real
`knob_ui::apply_event` dispatch core by a synthetic gesture sequence:

<p align="center">
  <img src="sim-shots/knob-menu-gallery.png" alt="Knob menu overlays: mode picker, Select Display, Select View" width="480">
</p>

## Running it yourself

From the repo root:

```sh
# Knob menu state machine + PUT formatter + gesture journeys (24 tests)
pio test -e native -f test_knob_menu

# Full host suite — no regression (218 tests)
pio test -e native

# Render the round views + menu overlays headlessly to docs/sim-shots/
make sim

# Plugin device-projection + firmware wire-contract tests
cd signalk/plugins/signalk-espdisp-manager && npm test

# Control-protocol C++ conformance (the shared controller path the knob runs)
pio test -e native -f test_proto

# Control-protocol JS lib + plugin proto-control
cd proto/js && npm test

# Build the knob firmware
pio run -e waveshare-knob-1_8
```

## Hardware bring-up checklist (when the device arrives)

The items below are **not** covered by the software verification above — each
needs the physical knob. They map to the open risks in the design spec §8 and the
implementation plan's Phase E/F. Work top to bottom; do not make a stability
claim until the soak step passes.

The single most important mitigation already shipped: the encoder feel
(counts-per-detent and direction) is **runtime-tunable from the console**, so you
calibrate it on first power-up **without reflashing** (see step 3).

1. **Flash over USB.**
   ```sh
   pio run -e waveshare-knob-1_8 -t upload
   ```
   The device should boot to the **Autopilot HUD**.

2. **Watch the serial console.**
   ```sh
   make monitor ENV=waveshare-knob-1_8
   # or: pio device monitor
   ```
   Confirm a clean boot with the encoder at rest — no boot-strapping issue from
   the encoder button on GPIO 0 held low at reset (spec §8.2).

3. **Verify and calibrate the encoder — no reflash.** Turn the knob one detent
   and confirm the firmware emits exactly **one** scroll event (not several, not
   zero). If the count-per-detent is wrong or the direction is reversed, fix it
   live from the console (serial **or** BLE — both route through the dispatch
   funnel):
   ```text
   knob status              # print counts_per_detent + invert
   knob counts <1-8>        # set encoder counts per detent (persisted in NVS)
   knob invert <0|1>        # swap rotation direction (persisted in NVS)
   ```
   Values persist across reboots in NVS, so once tuned the knob stays tuned.
   *Closes the design-risk:* encoder counts-per-detent + physical direction were
   the chief un-bench-tunable unknowns (spec §8, plan E3/F2) — this command turns
   them into a power-on calibration instead of a reflash cycle.

4. **QSPI display: color and orientation.** Confirm the ST77916 QSPI panel shows
   the Autopilot HUD with correct colors and the round view oriented upright (no
   swapped red/blue, no mirrored/rotated frame). *Closes:* ST77916 QSPI
   pin-mapping / display color correctness (spec §8.1, plan Phase C). Compare
   against the `docs/sim-shots/knob-*.png` renders, which are the intended
   appearance.

5. **CST816 touch axis / rotation.** Touch is secondary (navigation is
   encoder-first), but verify any tap target lands where expected — the touch
   axes/rotation match the round display orientation, not mirrored or swapped.
   *Closes:* CST816 touch axis/rotation correctness (spec §8.3).

6. **Autopilot PUT reaches a real SignalK / AP.** From the Autopilot HUD, scroll
   to change the target and click to engage; confirm the corresponding
   `steering.autopilot.target.headingTrue` / `steering.autopilot.state` PUT
   actually lands on the SignalK server and the autopilot responds. (The exact
   PUT path/value is already asserted in the host gesture-journey tests; this step
   confirms the wire path end-to-end on real hardware.) *Closes:* autopilot PUTs
   reaching a real SignalK/AP.

7. **Remote display switch.** With a second MFD on the network, double-click into
   **Select Display → Select View**, pick a view, and confirm the **other**
   display switches to it instantly over the [control
   protocol](control-protocol.md) (IP attach → switch → detach), and that the
   display shows the knob's colored frame while controlled.
   *Closes:* a real second display switching view from the knob.

8. **Soak before any stability claim.**
   ```sh
   tools/espdisp.py soak --remote <user@server> --device-ip <knob-ip>
   ```
   This records reboots / stalls / heap to JSONL and prints a PASS/FAIL verdict;
   run it from a host on the device's subnet. Do not call the knob "stable" until
   this passes. *Closes:* stability soak.

## The control-protocol harness (on-hardware verifier)

Because the knob's own controller path is the **shared** control-protocol C++
library, it is verified on hardware by a headless stand-in: the
**`harness-s3-devkitc`** build on a bare ESP32-S3-DevKitC-1 (no display). It runs
the same protocol library and loops `discover → attach → switch every view →
heartbeat → detach` against a real Sunton target, so the protocol — including the
NimBLE-central risk — gets real two-node coverage without the knob.

The full bring-up procedure (build + flash, the two-node IP run, the colored-frame
check, last-writer-wins, and the BLE-fallback / NimBLE soak) is documented in
[espdisp Control Protocol → Testing with the harness](control-protocol.md#11-testing-with-the-esp32-s3-devkitc-1-harness):

1. **Harness ↔ display (IP).** Flash `harness-s3-devkitc` (WiFi + target in
   `secrets.h`) and a Sunton `esp32-4848s040` target; watch the harness serial for
   the per-cycle `attached` / `switch <id> -> PASS` / `cycle done` log, and the
   pink (`#e91e63`) frame appearing/clearing on the display.
2. **Frame + last-writer-wins.** Confirm `GET /api/p2p/state` reflects the harness
   session; run a second controller with a different color → stacked frames.
3. **BLE fallback + NimBLE soak.** With IP disabled, confirm the controller
   BLE-scans, connects once, switches, and disconnects; run the NimBLE soak with
   the central role enabled and assert no heap starvation.

### Prerequisites for steps 6–7

To exercise the autopilot PUT and remote-view-switch on real hardware, the knob
must be provisioned onto WiFi, pointed at SignalK, given a device id, and given a
manager token — see
[Deploy & use the remote knob → Provision the knob](remote-knob.md#3-provision-the-knob).
The full command set (`wifi`, `sk`, `id`, `manager-token`, `manager-register`) is
documented there.

## Related docs

- [espdisp Control Protocol](control-protocol.md) — the protocol the knob/harness
  use to control displays, and the full harness verification procedure.
- [Deploy & use the remote knob](remote-knob.md) — flashing, provisioning,
  gestures, driving other displays.
- [Design spec](superpowers/specs/2026-06-13-waveshare-knob-remote-design.md) —
  interaction model and §8 open risks.
- [Implementation plan](superpowers/plans/2026-06-13-waveshare-knob-remote.md) —
  phase-by-phase build (Phase C display bring-up, Phase E/F verification).
- [User Guide — Managing Displays from SignalK](user-guide-signalk.md) — soak /
  health tooling.
</content>
</invoke>
