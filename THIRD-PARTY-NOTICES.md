# Third-Party Notices

Yey Boats Instruments is distributed under the **PolyForm Noncommercial License
1.0.0** (see [`LICENSE`](LICENSE); commercial licensing: rights@yey.boats). It
incorporates and links against third-party components that remain under **their
own licenses**, listed below. This file is provided to satisfy the attribution
and notice obligations of those licenses. It is informational, not legal advice;
licenses were read from each component's published metadata / `LICENSE` file at
the versions pinned in [`platformio.ini`](platformio.ini).

License identifiers use [SPDX](https://spdx.org/licenses/) where one is declared.

## Runtime components linked into the firmware

| Component | Version (pin) | License | Upstream |
|---|---|---|---|
| ArduinoJson | `bblanchon/ArduinoJson@^7.2.0` | **MIT** | https://github.com/bblanchon/ArduinoJson |
| LVGL | `lvgl/lvgl@^9.2.0` | **MIT** | https://github.com/lvgl/lvgl |
| NimBLE-Arduino | `h2zero/NimBLE-Arduino@^1.4.2` | **Apache-2.0** | https://github.com/h2zero/NimBLE-Arduino |
| TAMC_GT911 (touch) | `TAMCTec/gt911-arduino` | **Apache-2.0** | https://github.com/TAMCTec/gt911-arduino |
| GFX Library for Arduino | `moononournation/GFX Library for Arduino@~1.4.7` | **license not explicitly stated** — see note | https://github.com/moononournation/Arduino_GFX |
| arduinoWebSockets | `links2004/WebSockets@^2.4.1` | **LGPL-2.1** | https://github.com/Links2004/arduinoWebSockets |
| JetBrains Mono (embedded glyphs, `src/fonts/font_xl_64.c`) | `v2.304` | **OFL-1.1** | https://github.com/JetBrains/JetBrainsMono |

### Platform / framework (linked, supplied by the toolchain)

| Component | License | Upstream |
|---|---|---|
| Arduino core for ESP32 (`framework = arduino`) | **LGPL-2.1** | https://github.com/espressif/arduino-esp32 |
| ESP-IDF (underlying SDK) | **Apache-2.0** | https://github.com/espressif/esp-idf |

## Build- and test-only components (not shipped in the firmware image)

| Component | License | Upstream | Used by |
|---|---|---|---|
| Unity (ThrowTheSwitch) | **MIT** | https://github.com/ThrowTheSwitch/Unity | `pio test -e native` host tests |
| `@yey-boats/midl` runtime: `ajv` | **MIT** | https://github.com/ajv-validator/ajv | MIDL TS validator (slated for removal — see MIDL repo) |
| `@yey-boats/midl` runtime: `yaml` | **ISC** | https://github.com/eemeli/yaml | MIDL YAML↔JSON authoring (non-core) |
| MIDL TS toolchain: `typescript` (Apache-2.0), `tsup` (MIT), `vitest` (MIT), `@types/node` (MIT) | as noted | — | MIDL build/test only |
| `lv_font_conv` | **MIT** | https://github.com/lvgl/lv_font_conv | regenerating `src/fonts/*.c` (see `tools/fonts/gen_fonts.sh`) |
| JetBrains Mono TTF (`tools/fonts/JetBrainsMono-Regular.ttf`) | **OFL-1.1** | https://github.com/JetBrains/JetBrainsMono | source for the embedded 64 px hero font |

> Alternate board environments in `platformio.ini` (e.g. the Waveshare knob
> variant) additionally pull `CST816S`, `ESP32Encoder`, and `OneButton`. Confirm
> and append their licenses here before shipping any image built from those envs.

## Obligations summary

**Permissive components** (MIT / Apache-2.0 / ISC — ArduinoJson, LVGL,
NimBLE-Arduino, TAMC_GT911, Unity, ajv, yaml, and the TS toolchain) require only
that their copyright and permission notices be **preserved**. Apache-2.0
components (NimBLE-Arduino, TAMC_GT911, ESP-IDF, TypeScript) additionally require
that any `NOTICE` text they ship be propagated and that modifications be stated.
None impose source-disclosure on this project's own code, and all are compatible
with distribution under PolyForm Noncommercial 1.0.0 and with a separate
commercial license.

**Weak-copyleft components (LGPL-2.1): `arduinoWebSockets` and the Arduino-ESP32
core.** The LGPL does **not** require this project's own source to be opened, but
it does require that a recipient be able to **relink** the application against a
modified version of the LGPL library, and that the LGPL components' source and
license notices be made available. For the source-available (noncommercial)
distribution this is effectively satisfied because the full source is published.
For any **commercial** distribution of closed binaries, this relink obligation
must still be honored per shipped image (e.g. by providing linkable object files
or the corresponding source). Paths to reduce this exposure: replace
`arduinoWebSockets` with a permissively-licensed WebSocket client, and adopt the
bare **ESP-IDF (Apache-2.0)** build (tracked by the `esp32-4848s040-idf5` env),
which removes the Arduino-core LGPL surface.

**Unstated license — GFX Library for Arduino.** This dependency ships no
`LICENSE` file, declares no `license` field in its library metadata, and carries
no SPDX header in its sources. An unstated license is, strictly, "all rights
reserved." The library is published on the Arduino and PlatformIO registries for
general use, and portions derive from Adafruit_GFX (BSD), but the absence of an
explicit grant is a real (if low-probability) exposure. **Action:** confirm the
intended license with upstream, or migrate the display path onto `esp_lcd`
(also tracked by the `idf5` env), which removes this dependency.

## Copyright notices

- ArduinoJson — Copyright © 2014–2026, Benoît Blanchon. MIT License. Full text:
  the component's `LICENSE.txt`.
- LVGL — Copyright © 2025 LVGL Kft. MIT License. Full text: the component's
  `LICENCE.txt`.
- NimBLE-Arduino — Copyright © h2zero and contributors. Apache License 2.0.
- TAMC_GT911 — Copyright © TAMCTec and contributors. Apache License 2.0.
- arduinoWebSockets — Copyright © Markus Sattler. GNU Lesser General Public
  License v2.1.
- GFX Library for Arduino — © Moon On Our Nation (moononournation); license not
  explicitly stated (see note above).
- JetBrains Mono — Copyright © 2020 The JetBrains Mono Project Authors. SIL Open
  Font License, Version 1.1. Full text: [`tools/fonts/JetBrainsMono-OFL.txt`](tools/fonts/JetBrainsMono-OFL.txt).
  Used to generate the embedded hero font (`src/fonts/font_xl_64.c`). The OFL
  permits embedding the font in this product; the font itself is not sold.
- Arduino core for ESP32 — Copyright © Espressif Systems and contributors. LGPL-2.1.
- ESP-IDF — Copyright © Espressif Systems. Apache License 2.0.

Full license texts are distributed with each component (in its installed package
under `.pio/libdeps/<env>/<lib>/`) and at the upstream URLs above.
