# Waveshare Knob Remote Controller — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (round 360×360, ST77916 QSPI, rotary encoder) as a board variant that acts as a rotary-driven remote controller: an autopilot-home multilevel menu plus select-display / select-view control of other MFDs over the manager, with a small set of dedicated round views.

**Architecture:** A dedicated `knob_input` FreeRTOS task classifies encoder + button into semantic events and posts them through the existing `app::Command` queue, drained on the LVGL task. A **pure, host-tested** `knob_menu` state machine turns events into navigation + index-based `Action`s; the device glue resolves those into the exact `app::Command{SignalKPut,…}` the on-screen autopilot already uses (autopilot) and manager `screen.set` commands (remote view switch). Display bring-up is board-gated in `main.cpp` (QSPI/ST77916 vs the existing RGB/ST7701).

**Tech Stack:** PlatformIO/Arduino (ESP32-S3, IDF 4.4), LVGL 9, Arduino_GFX (ST77916 + ESP32QSPI), `madhephaestus/ESP32Encoder` (PCNT), `mathertel/OneButton`, optional `adafruit/Adafruit DRV2605 Library`, Unity (host tests), Node SignalK plugin.

**Spec:** `docs/superpowers/specs/2026-06-13-waveshare-knob-remote-design.md`

**Refinement vs spec §3.1:** We do NOT add `LayoutClass::Round`. The existing pattern (`src/boards/board_native_fake.cpp:48-61`) keeps `LayoutClass::SquareCompact` for square-aspect panels and expresses roundness via `DisplayShape::Round` + an inscribed `usable_*` inset. The knob follows that, avoiding edits to `board_common.cpp` name tables and every board's `classify()`.

**Phasing:** Phases A–G land the manager-path v1. The SignalK peer-to-peer fallback (spec §4.2) is Phase H and may ship as a follow-up plan; nothing in A–G depends on it.

---

## Phase A — Board scaffolding

### Task A1: Knob pin map header

**Files:**
- Create: `include/board_pins_waveshare_knob.h`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

// clang-format off

// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (module JC3636K518)
// 1.8" 360x360 round, ST77916 QSPI panel, CST816S touch, rotary encoder.
// ESP32-S3R8 (16 MB flash + 8 MB PSRAM). Audio second-ESP32 unused.
// Pin map verified: github.com/arendst/Tasmota/discussions/23737,
// github.com/orgs/esphome/discussions/3253.

#ifndef LCD_W
#define LCD_W   360
#endif
#ifndef LCD_H
#define LCD_H   360
#endif

// ST77916 over Quad-SPI (single-data CS/CLK + D0..D3).
#define QSPI_CS    14
#define QSPI_SCK   13
#define QSPI_D0    15
#define QSPI_D1    16
#define QSPI_D2    17
#define QSPI_D3    18
#define LCD_RST    21
#define LCD_BL     47     // backlight, LEDC PWM (active high)

// CST816S capacitive touch (I2C, addr 0x15).
#define TOUCH_SDA  11
#define TOUCH_SCL  12
#ifndef TOUCH_INT
#define TOUCH_INT  9
#endif
#define TOUCH_RST  10
#ifndef TOUCH_INT_ACTIVE_LOW
#define TOUCH_INT_ACTIVE_LOW 1
#endif

// Rotary encoder (quadrature) + push switch.
#define ENC_A      8
#define ENC_B      7
#define ENC_BTN    0      // also boot-strap; input w/ pull-up after boot

// DRV2605 haptic on the touch I2C bus (addr 0x5A).
#define HAPTIC_I2C_ADDR 0x5A

// clang-format on
```

- [ ] **Step 2: Verify it compiles in isolation**

Run: `cc -fsyntax-only -xc++ -std=c++17 -Iinclude include/board_pins_waveshare_knob.h`
Expected: no output (exit 0).

- [ ] **Step 3: Commit**

```bash
git add include/board_pins_waveshare_knob.h
git commit -m "feat(board): waveshare knob pin map header"
```

---

### Task A2: board_pins.h board-gated include

**Files:**
- Modify: `include/board_pins.h:1-6` (top of file, before the Sunton block)

The current `board_pins.h` is unconditionally the Sunton map. Gate it so the knob board pulls its own pins.

- [ ] **Step 1: Wrap the Sunton body and add the knob include**

At the very top of `include/board_pins.h`, immediately after `#pragma once`, insert:

```cpp
#pragma once

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include "board_pins_waveshare_knob.h"
#else
// clang-format off
// ... existing Sunton/Guition map unchanged below ...
```

And at the end of the existing Sunton content add the matching `#endif`. Keep the existing `// clang-format off`/`on` exactly where they are inside the `#else` branch.

- [ ] **Step 2: Build the existing board to prove no regression**

Run: `pio run -e esp32-4848s040 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add include/board_pins.h
git commit -m "feat(board): gate board_pins.h so knob pulls its own map"
```

---

### Task A3: Knob board implementation

**Files:**
- Create: `src/boards/board_waveshare_knob.cpp`

- [ ] **Step 1: Write the board impl (mirrors board_sunton_4848s040.cpp + round usable from board_native_fake.cpp)**

```cpp
#include "board.h"

#include <driver/ledc.h>
#include <math.h>
#include <stdint.h>

// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 board::* implementation.
// Round 360x360 ST77916 QSPI. Backlight LEDC PWM on GPIO 47.

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

namespace board {

namespace {

constexpr int BACKLIGHT_PIN = 47;
constexpr ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t BL_TIMER = LEDC_TIMER_0;
constexpr ledc_channel_t BL_CHANNEL = LEDC_CHANNEL_0;
constexpr int LEDC_FREQ_HZ = 5000;
uint8_t s_backlight_value = 255;
bool s_backlight_inited = false;

void ensure_backlight() {
    if (s_backlight_inited) return;
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = BL_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_8_BIT;
    timer_cfg.timer_num = BL_TIMER;
    timer_cfg.freq_hz = LEDC_FREQ_HZ;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = BACKLIGHT_PIN;
    channel_cfg.speed_mode = BL_MODE;
    channel_cfg.channel = BL_CHANNEL;
    channel_cfg.intr_type = LEDC_INTR_DISABLE;
    channel_cfg.timer_sel = BL_TIMER;
    channel_cfg.duty = 255;
    channel_cfg.hpoint = 0;
    ledc_channel_config(&channel_cfg);
    s_backlight_inited = true;
}

}  // namespace

const char *id() {
    return "waveshare_knob_1_8";
}
const char *display_name() {
    return "Waveshare ESP32-S3-Knob 1.8\" 360x360 round";
}

Geometry geometry() {
    Geometry g{};
    g.width_px = 360;
    g.height_px = 360;
    g.diagonal_tenths_in = 18;
    g.rotation = 0;
    g.square = true;
    g.shape = DisplayShape::Round;
    g.layout_class = LayoutClass::SquareCompact;  // square-aspect; round via shape+usable
    g.density_class = DensityClass::Hdpi;
    // Inscribed square inside the 360 circle: side = 360/sqrt(2) ~= 254,
    // inset = (360-254)/2 ~= 53 each side.
    const uint16_t inset = 53;
    g.usable_x = inset;
    g.usable_y = inset;
    g.usable_width = g.width_px - inset * 2;
    g.usable_height = g.height_px - inset * 2;
    return g;
}

Capabilities capabilities() {
    Capabilities c{};
    c.psram_required = true;
    c.backlight = BacklightKind::LedcPwm;
    c.touch = TouchKind::CST816;
    c.touch_calibration = false;
    c.beeper = true;  // routed to DRV2605 haptic
    c.nmea2000_can = false;
    c.sd_card = false;
    c.display_bus = DisplayBus::Qspi;
    c.touch_interrupt = TOUCH_INT >= 0;
    return c;
}

bool set_backlight(uint8_t value_0_255) {
    ensure_backlight();
    s_backlight_value = value_0_255;
    ledc_set_duty(BL_MODE, BL_CHANNEL, value_0_255);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
    return true;
}

uint8_t backlight() {
    return s_backlight_value;
}

bool set_power(bool on) {
    return set_backlight(on ? 255 : 0);
}

}  // namespace board

#endif  // BOARD_ID_WAVESHARE_KNOB_1_8
```

- [ ] **Step 2: Commit** (builds once envs exist in Task A4)

```bash
git add src/boards/board_waveshare_knob.cpp
git commit -m "feat(board): waveshare knob board::* implementation"
```

---

### Task A4: PlatformIO envs + CI matrix

**Files:**
- Modify: `platformio.ini` (add knob envs near the other Waveshare blocks; extend the `native` filters)
- Modify: `.github/workflows/ci.yml` (add a build-matrix entry)

- [ ] **Step 1: Add the production + release knob envs to `platformio.ini`**

After the `[env:waveshare-touch-lcd-7b_1024x600]` block:

```ini
; Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (round 360x360 ST77916 QSPI + encoder).
[env:waveshare-knob-1_8]
extends = env:esp32-4848s040
build_flags =
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D BOARD_HAS_PSRAM
    -D LV_CONF_INCLUDE_SIMPLE
    -D LV_LVGL_H_INCLUDE_SIMPLE
    -I include
    -DCORE_DEBUG_LEVEL=3
    -D BOARD_ID_DEFINED
    -D BOARD_ID_WAVESHARE_KNOB_1_8
    -D LCD_W=360
    -D LCD_H=360
    -fstack-protector-strong
lib_deps =
    ${env:esp32-4848s040.lib_deps}
    madhephaestus/ESP32Encoder@^0.11.7
    mathertel/OneButton@^2.6.1
```

And after `[env:release-waveshare-touch-lcd-7b_1024x600]`:

```ini
[env:release-waveshare-knob-1_8]
extends = env:waveshare-knob-1_8
build_flags =
    ${release_common.build_flags}
    -D BOARD_ID_WAVESHARE_KNOB_1_8
    -D LCD_W=360
    -D LCD_H=360
lib_deps =
    ${env:esp32-4848s040.lib_deps}
    madhephaestus/ESP32Encoder@^0.11.7
    mathertel/OneButton@^2.6.1
```

- [ ] **Step 2: Extend the `native` env so knob_menu is host-tested**

In `[env:native]`, append `+<knob_menu.cpp>` to `build_src_filter` and add `test_knob_menu` to `test_filter`. (The test source is created in Phase B.)

- [ ] **Step 3: Build the knob env (board impl + scaffolding only; display/UI come later)**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -20`
Expected at this stage: it may FAIL at `main.cpp` display init (still RGB-only). That's expected until Task C1. If it fails *only* inside `main.cpp` display code, A1–A4 are correct. Confirm `board_waveshare_knob.cpp` itself compiled (no errors from that TU).

- [ ] **Step 4: Add CI matrix entry**

In `.github/workflows/ci.yml`, add `waveshare-knob-1_8` to the firmware build matrix list (mirror an existing `waveshare-touch-lcd-*` entry).

- [ ] **Step 5: Commit**

```bash
git add platformio.ini .github/workflows/ci.yml
git commit -m "feat(board): knob platformio envs + CI matrix + native filter"
```

---

## Phase B — `knob_menu` pure state machine (TDD core)

### Task B1: knob_menu header

**Files:**
- Create: `include/knob_menu.h`

- [ ] **Step 1: Write the interface**

```cpp
#pragma once

// Pure multilevel knob menu state machine. No LVGL, no Arduino — links
// on the native host test env. The device glue (knob_ui / main) feeds it
// Events + a read-only Inputs snapshot and turns the emitted Action into
// the existing app::Command path. Levels and gestures per
// docs/superpowers/specs/2026-06-13-waveshare-knob-remote-design.md §3.4.

#include <stdint.h>

namespace knob {

constexpr int kStepSmallDeg = 1;
constexpr int kStepBigDeg = 5;
constexpr int kModeCount = 4;  // STANDBY, COMPASS, WIND, ROUTE

enum class Event : uint8_t { None = 0, DetentCW, DetentCCW, Click, DoubleClick, LongPress };

enum class Level : uint8_t { Home = 0, ModePicker, SelectDisplay, SelectView };

enum class ActionType : uint8_t {
    NoAction = 0,
    ApSetState,      // arg_str = "standby"|"auto"|"wind"|"route"
    ApSetTargetRad,  // arg_f = absolute target heading (radians, 0..2pi)
    SwitchView,      // arg_dev_idx + arg_view_idx -> switch that display's view
};

struct Action {
    ActionType type = ActionType::NoAction;
    char arg_str[16] = {0};
    double arg_f = 0.0;
    int arg_dev_idx = -1;
    int arg_view_idx = -1;
};

// Read-only snapshot the menu reasons over.
struct Inputs {
    const char *ap_state = "";   // current AP state string ("standby"/"auto"/...)
    double ap_target_rad = 0.0;  // NaN if unknown
    double heading_rad = 0.0;    // NaN if unknown
    int display_count = 0;       // # of displays in SelectDisplay
    int view_count = 0;          // # of views of the entered display (SelectView)
};

struct Model {
    Level level = Level::Home;
    int highlight = 0;          // index within the current level's list
    int entered_display = -1;   // display drilled into (SelectView)
    double pending_target_rad = 0.0;  // local pending target; NaN until adjusted
    int last_engaged_mode = 1;  // picker idx of last non-standby mode (COMPASS)
    bool engaged = false;       // AP engaged (non-standby)?
};

void init(Model &m);

// Step the machine. `held` = button held during a detent (=> ±5 instead of ±1).
// Pure: mutates `m`, returns the side-effect Action (NoAction if none).
Action step(Model &m, const Inputs &in, Event ev, bool held);

// 0->"standby", 1->"auto", 2->"wind", 3->"route". Out-of-range -> "standby".
const char *mode_state_string(int picker_idx);
const char *level_name(Level l);

}  // namespace knob
```

- [ ] **Step 2: Commit**

```bash
git add include/knob_menu.h
git commit -m "feat(knob): knob_menu pure state-machine interface"
```

---

### Task B2: Home-level course adjust (±1 / ±5, pre-set in standby)

**Files:**
- Create: `test/test_knob_menu/test_knob_menu.cpp`
- Create: `src/knob_menu.cpp`

- [ ] **Step 1: Write failing tests for Home course adjust**

```cpp
#include <unity.h>
#include <math.h>
#include <string.h>

#include "knob_menu.h"

using namespace knob;

void setUp(void) {}
void tearDown(void) {}

static Inputs base_inputs() {
    Inputs in;
    in.ap_state = "standby";
    in.ap_target_rad = NAN;
    in.heading_rad = M_PI;  // 180 deg
    in.display_count = 0;
    in.view_count = 0;
    return in;
}

static void test_home_cw_small_step_presets_target_from_heading() {
    Model m; init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::DetentCW, /*held=*/false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    // seeded from heading 180, +1 deg -> 181 deg
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 181.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_cw_big_step_when_held() {
    Model m; init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::DetentCW, /*held=*/true);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetTargetRad, (int)a.type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 185.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_ccw_wraps_below_zero() {
    Model m; init(m);
    Inputs in = base_inputs();
    in.heading_rad = 0.0;
    Action a = step(m, in, Event::DetentCCW, /*held=*/false);
    // 0 - 1 deg -> 359 deg
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 359.0 * M_PI / 180.0, a.arg_f);
}

static void test_home_adjust_accumulates_into_pending() {
    Model m; init(m);
    Inputs in = base_inputs();
    step(m, in, Event::DetentCW, false);   // 181
    Action a = step(m, in, Event::DetentCW, false);  // 182
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 182.0 * M_PI / 180.0, a.arg_f);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_home_cw_small_step_presets_target_from_heading);
    RUN_TEST(test_home_cw_big_step_when_held);
    RUN_TEST(test_home_ccw_wraps_below_zero);
    RUN_TEST(test_home_adjust_accumulates_into_pending);
    return UNITY_END();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: FAIL — link error / `knob_menu.cpp` does not exist yet.

- [ ] **Step 3: Write minimal `src/knob_menu.cpp` for Home adjust**

```cpp
#include "knob_menu.h"

#include <math.h>
#include <string.h>

namespace knob {

static const char *kModeState[kModeCount] = {"standby", "auto", "wind", "route"};

const char *mode_state_string(int idx) {
    if (idx < 0 || idx >= kModeCount) return "standby";
    return kModeState[idx];
}

const char *level_name(Level l) {
    switch (l) {
        case Level::Home: return "home";
        case Level::ModePicker: return "mode";
        case Level::SelectDisplay: return "display";
        case Level::SelectView: return "view";
    }
    return "?";
}

void init(Model &m) {
    m = Model{};
    m.pending_target_rad = NAN;
}

static double wrap_2pi(double r) {
    while (r < 0) r += 2 * M_PI;
    while (r >= 2 * M_PI) r -= 2 * M_PI;
    return r;
}

static double seed_target(const Model &m, const Inputs &in) {
    if (!isnan(m.pending_target_rad)) return m.pending_target_rad;
    if (!isnan(in.ap_target_rad)) return in.ap_target_rad;
    if (!isnan(in.heading_rad)) return in.heading_rad;
    return 0.0;
}

static Action adjust(Model &m, const Inputs &in, int sign, bool held) {
    int deg = (held ? kStepBigDeg : kStepSmallDeg) * sign;
    double t = wrap_2pi(seed_target(m, in) + deg * M_PI / 180.0);
    m.pending_target_rad = t;
    Action a;
    a.type = ActionType::ApSetTargetRad;
    a.arg_f = t;
    return a;
}

Action step(Model &m, const Inputs &in, Event ev, bool held) {
    Action a;  // NoAction
    switch (m.level) {
        case Level::Home:
            if (ev == Event::DetentCW) return adjust(m, in, +1, held);
            if (ev == Event::DetentCCW) return adjust(m, in, -1, held);
            break;
        default:
            break;
    }
    return a;
}

}  // namespace knob
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add test/test_knob_menu/test_knob_menu.cpp src/knob_menu.cpp
git commit -m "feat(knob): home course adjust (+/-1, +/-5 held, wrap, pre-set)"
```

---

### Task B3: Home engage/disengage + mode picker

**Files:**
- Modify: `test/test_knob_menu/test_knob_menu.cpp`
- Modify: `src/knob_menu.cpp:step` (Home Click/LongPress + ModePicker level)

- [ ] **Step 1: Add failing tests**

Add these test functions and register them in `main()`:

```cpp
static void test_home_click_engages_last_mode_then_standby() {
    Model m; init(m);
    Inputs in = base_inputs();
    // default last_engaged_mode = 1 (COMPASS -> "auto")
    Action a1 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetState, (int)a1.type);
    TEST_ASSERT_EQUAL_STRING("auto", a1.arg_str);
    TEST_ASSERT_TRUE(m.engaged);
    // second click disengages -> standby
    Action a2 = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_STRING("standby", a2.arg_str);
    TEST_ASSERT_FALSE(m.engaged);
}

static void test_home_longpress_opens_mode_picker() {
    Model m; init(m);
    Inputs in = base_inputs();
    Action a = step(m, in, Event::LongPress, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::ModePicker, (int)m.level);
}

static void test_mode_picker_scroll_and_select_wind() {
    Model m; init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);     // -> ModePicker, highlight=last(1)
    step(m, in, Event::DetentCW, false);      // highlight 2 (WIND)
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::ApSetState, (int)a.type);
    TEST_ASSERT_EQUAL_STRING("wind", a.arg_str);
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);  // returns home
    TEST_ASSERT_TRUE(m.engaged);
    TEST_ASSERT_EQUAL_INT(2, m.last_engaged_mode);
}

static void test_mode_picker_doubleclick_cancels() {
    Model m; init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);
    Action a = step(m, in, Event::DoubleClick, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
}

static void test_mode_picker_select_standby_disengages() {
    Model m; init(m);
    Inputs in = base_inputs();
    step(m, in, Event::LongPress, false);   // highlight = 1
    step(m, in, Event::DetentCCW, false);   // highlight 0 (STANDBY)
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_STRING("standby", a.arg_str);
    TEST_ASSERT_FALSE(m.engaged);
}
```

- [ ] **Step 2: Run to verify fail**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: FAIL (new tests; Click/LongPress unhandled).

- [ ] **Step 3: Extend `step()` — add a `set_state` helper and the new cases**

Add near `adjust()`:

```cpp
static Action set_state(int picker_idx) {
    Action a;
    a.type = ActionType::ApSetState;
    strncpy(a.arg_str, mode_state_string(picker_idx), sizeof(a.arg_str) - 1);
    return a;
}

static int wrap_idx(int i, int n) {
    if (n <= 0) return 0;
    return ((i % n) + n) % n;
}
```

In `step()`, extend the `Home` case (before `break;`) and add the `ModePicker` case:

```cpp
        case Level::Home:
            if (ev == Event::DetentCW) return adjust(m, in, +1, held);
            if (ev == Event::DetentCCW) return adjust(m, in, -1, held);
            if (ev == Event::Click) {
                if (m.engaged) {
                    m.engaged = false;
                    return set_state(0);  // standby
                }
                m.engaged = true;
                return set_state(m.last_engaged_mode);
            }
            if (ev == Event::LongPress) {
                m.level = Level::ModePicker;
                m.highlight = m.last_engaged_mode;
                return a;
            }
            if (ev == Event::DoubleClick) {
                m.level = Level::SelectDisplay;
                m.highlight = 0;
                return a;
            }
            break;

        case Level::ModePicker:
            if (ev == Event::DetentCW) { m.highlight = wrap_idx(m.highlight + 1, kModeCount); return a; }
            if (ev == Event::DetentCCW) { m.highlight = wrap_idx(m.highlight - 1, kModeCount); return a; }
            if (ev == Event::Click) {
                m.level = Level::Home;
                if (m.highlight == 0) {
                    m.engaged = false;
                } else {
                    m.engaged = true;
                    m.last_engaged_mode = m.highlight;
                }
                return set_state(m.highlight);
            }
            if (ev == Event::DoubleClick) { m.level = Level::Home; return a; }
            break;
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: PASS (all Phase B2 + B3 tests).

- [ ] **Step 5: Commit**

```bash
git add test/test_knob_menu/test_knob_menu.cpp src/knob_menu.cpp
git commit -m "feat(knob): home engage toggle + mode picker"
```

---

### Task B4: SelectDisplay / SelectView navigation + SwitchView action

**Files:**
- Modify: `test/test_knob_menu/test_knob_menu.cpp`
- Modify: `src/knob_menu.cpp:step` (SelectDisplay + SelectView cases)

- [ ] **Step 1: Add failing tests**

```cpp
static Inputs remote_inputs() {
    Inputs in = base_inputs();
    in.display_count = 3;
    in.view_count = 4;
    return in;
}

static void test_select_display_scroll_and_drill_in() {
    Model m; init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);   // Home -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::DetentCW, false);      // highlight 1
    step(m, in, Event::DetentCW, false);      // highlight 2
    Action a = step(m, in, Event::Click, false);  // drill into display 2
    TEST_ASSERT_EQUAL_INT((int)ActionType::NoAction, (int)a.type);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);
    TEST_ASSERT_EQUAL_INT(2, m.entered_display);
    TEST_ASSERT_EQUAL_INT(0, m.highlight);
}

static void test_select_display_wraps() {
    Model m; init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);
    step(m, in, Event::DetentCCW, false);  // 0 -> wrap to 2
    TEST_ASSERT_EQUAL_INT(2, m.highlight);
}

static void test_select_view_click_emits_switch_view() {
    Model m; init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);   // SelectDisplay
    step(m, in, Event::Click, false);         // enter display 0
    step(m, in, Event::DetentCW, false);      // view highlight 1
    Action a = step(m, in, Event::Click, false);
    TEST_ASSERT_EQUAL_INT((int)ActionType::SwitchView, (int)a.type);
    TEST_ASSERT_EQUAL_INT(0, a.arg_dev_idx);
    TEST_ASSERT_EQUAL_INT(1, a.arg_view_idx);
    TEST_ASSERT_EQUAL_INT((int)Level::SelectView, (int)m.level);  // stays
}

static void test_double_click_back_chain() {
    Model m; init(m);
    Inputs in = remote_inputs();
    step(m, in, Event::DoubleClick, false);   // Home -> SelectDisplay
    step(m, in, Event::Click, false);         // -> SelectView
    step(m, in, Event::DoubleClick, false);   // -> SelectDisplay
    TEST_ASSERT_EQUAL_INT((int)Level::SelectDisplay, (int)m.level);
    step(m, in, Event::DoubleClick, false);   // -> Home
    TEST_ASSERT_EQUAL_INT((int)Level::Home, (int)m.level);
}
```

Register all four in `main()`.

- [ ] **Step 2: Run to verify fail**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: FAIL.

- [ ] **Step 3: Add the two cases to `step()`**

```cpp
        case Level::SelectDisplay:
            if (ev == Event::DetentCW) { m.highlight = wrap_idx(m.highlight + 1, in.display_count); return a; }
            if (ev == Event::DetentCCW) { m.highlight = wrap_idx(m.highlight - 1, in.display_count); return a; }
            if (ev == Event::Click) {
                m.entered_display = m.highlight;
                m.level = Level::SelectView;
                m.highlight = 0;
                return a;
            }
            if (ev == Event::DoubleClick) { m.level = Level::Home; return a; }
            break;

        case Level::SelectView:
            if (ev == Event::DetentCW) { m.highlight = wrap_idx(m.highlight + 1, in.view_count); return a; }
            if (ev == Event::DetentCCW) { m.highlight = wrap_idx(m.highlight - 1, in.view_count); return a; }
            if (ev == Event::Click) {
                a.type = ActionType::SwitchView;
                a.arg_dev_idx = m.entered_display;
                a.arg_view_idx = m.highlight;
                return a;  // stays in SelectView
            }
            if (ev == Event::DoubleClick) { m.level = Level::SelectDisplay; return a; }
            break;
```

- [ ] **Step 4: Run to verify pass**

Run: `pio test -e native -f test_knob_menu 2>&1 | tail -20`
Expected: PASS (all Phase B tests).

- [ ] **Step 5: Run the full native suite (no regressions)**

Run: `pio test -e native 2>&1 | tail -15`
Expected: all test suites PASS.

- [ ] **Step 6: Commit**

```bash
git add test/test_knob_menu/test_knob_menu.cpp src/knob_menu.cpp
git commit -m "feat(knob): select-display/select-view nav + switch-view action"
```

---

## Phase C — Display bring-up (board-gated QSPI/ST77916)

### Task C1: Board-gate display construction in main.cpp

**Files:**
- Modify: `src/main.cpp` (display init region — the `Arduino_ESP32RGBPanel` / `Arduino_RGB_Display` construction near lines 75–120 and `gfx->begin()` near line 1796)

**REQUIRED SUB-SKILL:** `firmware-memory-traps-check` — this edits `main.cpp`. Keep LVGL draw buffers in PSRAM; do not introduce large stack temporaries.

- [ ] **Step 1: Verify the GFX library exposes ST77916 + QSPI**

Run: `ls -d .pio/libdeps/waveshare-knob-1_8/"GFX Library for Arduino"/src/display/ 2>/dev/null && grep -rl "ST77916" .pio/libdeps/waveshare-knob-1_8/"GFX Library for Arduino"/src/ 2>/dev/null; grep -rl "Arduino_ESP32QSPI" .pio/libdeps/waveshare-knob-1_8/"GFX Library for Arduino"/src/ 2>/dev/null`
Expected: paths to `Arduino_ST77916.*` and `Arduino_ESP32QSPI.*`.
**If empty:** bump the knob env's `lib_deps` GFX entry to `moononournation/GFX Library for Arduino@^1.5.0`, re-run `pio pkg install -e waveshare-knob-1_8`, and repeat. Record the working version in the commit.

- [ ] **Step 2: Wrap the existing RGB construction and add the knob QSPI branch**

Find the existing global display construction in `src/main.cpp` (the `Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(...)` + `Arduino_RGB_Display *gfx = new Arduino_RGB_Display(...)`). Wrap it:

```cpp
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Waveshare knob: ST77916 over Quad-SPI. CS/SCK + D0..D3 from board pins.
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    QSPI_CS /* cs */, QSPI_SCK /* sck */, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
static Arduino_GFX *gfx =
    new Arduino_ST77916(bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);
#else
// ... existing Sunton/RGB construction unchanged ...
#endif
```

Add the include near the other Arduino_GFX includes at the top of `main.cpp`:

```cpp
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include <Arduino_GFX_Library.h>
#endif
```

(If `Arduino_GFX_Library.h` is already included unconditionally, skip the extra include.)

- [ ] **Step 3: Confirm `gfx->begin()` and the LVGL flush path are board-agnostic**

The flush callback uses `gfx->draw16bitRGBBitmap(...)` against the `Arduino_GFX *gfx` base type — unchanged. Verify the flush callback references `gfx` (the base pointer), not the RGB subtype. If it casts to `Arduino_RGB_Display`, change the cast to `Arduino_GFX`.

- [ ] **Step 4: Confirm draw buffers are PSRAM (memory trap)**

Verify the LVGL draw-buffer allocation uses `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` (or the existing PSRAM path). 360×360×2 = ~253 KB/full buffer — size partial buffers as the existing code does; do NOT place them in internal DMA SRAM.

- [ ] **Step 5: Build the knob env**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -20`
Expected: `SUCCESS`.

- [ ] **Step 6: Build the Sunton env (no regression)**

Run: `pio run -e esp32-4848s040 2>&1 | tail -5`
Expected: `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp platformio.ini
git commit -m "feat(board): board-gated ST77916 QSPI display init for the knob"
```

---

### Task C2: CST816 touch read (board-gated, optional input)

**Files:**
- Modify: `src/main.cpp` (touch init/read region near the GT911 setup, ~lines 567–637)

Touch is secondary (encoder-first). Wire CST816 so taps still register, but the menu does not depend on it.

- [ ] **Step 1: Add the CST816 lib to the knob env**

In `[env:waveshare-knob-1_8]` and `[env:release-waveshare-knob-1_8]` `lib_deps`, add `fbiego/CST816S@^1.1.1`.

- [ ] **Step 2: Board-gate touch init/read**

Wrap the GT911 init + the `touch_task` I2C read so the knob path uses CST816S on SDA 11 / SCL 12 / INT 9 / RST 10, writing the same shared `g_touch` snapshot the pointer indev reads. Keep the existing GT911 path under `#else`. Mirror the existing snapshot/mutex contract exactly (`g_touch`, `g_touch_mtx`).

- [ ] **Step 3: Build**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -10`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp platformio.ini
git commit -m "feat(board): board-gated CST816 touch read for the knob"
```

---

## Phase D — Encoder + button input task

### Task D1: knob_input event source

**Files:**
- Create: `include/knob_input.h`
- Create: `src/knob_input.cpp`
- Modify: `include/app_events.h` (add `CommandType::Knob`)

- [ ] **Step 1: Add the Knob command type**

In `include/app_events.h`, add to `enum class CommandType`:

```cpp
    Knob,  // i = knob::Event value; b[0] = '1' if button held during detent
```

- [ ] **Step 2: Write `include/knob_input.h`**

```cpp
#pragma once

// Encoder + push-button input source for the Waveshare knob. Runs a
// dedicated task that classifies quadrature detents (ESP32Encoder/PCNT)
// and button gestures (OneButton) into knob::Event values, posted to the
// UI queue as app::CommandType::Knob. Device-only; empty on other boards.

namespace knob_input {

// Start the encoder/button task. No-op unless BOARD_ID_WAVESHARE_KNOB_1_8.
void setup();

}  // namespace knob_input
```

- [ ] **Step 3: Write `src/knob_input.cpp`**

```cpp
#include "knob_input.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <OneButton.h>

#include "app_events.h"
#include "board_pins.h"
#include "knob_menu.h"

namespace knob_input {

namespace {

ESP32Encoder s_enc;
OneButton s_btn(ENC_BTN, /*activeLow=*/true, /*pullupActive=*/true);
volatile bool s_held = false;
int64_t s_last_count = 0;

void post_event(knob::Event ev) {
    app::Command c;
    c.type = app::CommandType::Knob;
    c.i = (int32_t)ev;
    c.b[0] = s_held ? '1' : '0';
    c.b[1] = '\0';
    c.t_post_us = micros();
    app::post(c, 0);
}

void on_click() { post_event(knob::Event::Click); }
void on_double() { post_event(knob::Event::DoubleClick); }
void on_long_start() { post_event(knob::Event::LongPress); }
void on_press_start() { s_held = true; }
void on_release() { s_held = false; }

void knob_task(void *) {
    // PCNT counts 4 transitions per detent on full-quadrature; divide so one
    // physical detent = one event. Adjust divisor after bench check (Step 7).
    constexpr int kCountsPerDetent = 4;
    for (;;) {
        s_btn.tick();
        int64_t count = s_enc.getCount();
        int64_t delta = count - s_last_count;
        while (delta >= kCountsPerDetent) {
            post_event(knob::Event::DetentCW);
            s_last_count += kCountsPerDetent;
            delta -= kCountsPerDetent;
        }
        while (delta <= -kCountsPerDetent) {
            post_event(knob::Event::DetentCCW);
            s_last_count -= kCountsPerDetent;
            delta += kCountsPerDetent;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void setup() {
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    s_enc.attachFullQuad(ENC_A, ENC_B);
    s_enc.clearCount();
    s_last_count = 0;

    s_btn.attachClick(on_click);
    s_btn.attachDoubleClick(on_double);
    s_btn.attachLongPressStart(on_long_start);
    s_btn.attachPressStart(on_press_start);
    s_btn.attachLongPressStop(on_release);
    s_btn.attachDuringLongPress(nullptr);
    s_btn.setPressMs(400);  // long-press threshold

    xTaskCreatePinnedToCore(knob_task, "knob", 4096, nullptr, 2, nullptr, 0);
}

}  // namespace knob_input

#else  // not the knob board

namespace knob_input {
void setup() {}
}  // namespace knob_input

#endif
```

Note: `s_held` is set on press-start and cleared on release, so a detent emitted while the button is down carries `held=true` (the ±5 path). OneButton still fires `LongPress` for a stationary long hold (mode picker); a hold-and-rotate produces detents with `s_held=true` without necessarily firing long-press — acceptable, the menu reads `held` per detent.

- [ ] **Step 4: Build**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -15`
Expected: `SUCCESS`.

- [ ] **Step 5: Commit**

```bash
git add include/app_events.h include/knob_input.h src/knob_input.cpp
git commit -m "feat(knob): encoder + button input task emitting Knob events"
```

---

## Phase E — Menu UI + action dispatch wiring

### Task E1: Remote target model (device + view lists)

**Files:**
- Create: `include/knob_remote.h`
- Create: `src/knob_remote.cpp`

This is the device-side store the menu's `Inputs.display_count` / `view_count` read from, and the resolver that turns a `SwitchView{dev_idx, view_idx}` into a manager command. Manager fetch is filled in Phase F; here it owns the in-RAM lists + a local entry for the knob itself.

- [ ] **Step 1: Write `include/knob_remote.h`**

```cpp
#pragma once

#include <stddef.h>

// Device + view registry the knob menu navigates. Entry 0 is always the
// knob itself (local dedicated views); entries 1..N are remote displays
// discovered via the manager (Phase F). Switching resolves an index pair
// to either a local screen change or a manager screen.set command.

namespace knob_remote {

constexpr size_t kMaxDisplays = 12;
constexpr size_t kMaxViews = 12;

struct DisplayEntry {
    char id[40];        // device id ("" for the local knob -> use local)
    char name[40];      // human label
    bool is_local;
    int view_count;
    char view_id[kMaxViews][24];
    char view_title[kMaxViews][24];
    int current_view;   // index of the display's active view (-1 unknown)
};

void setup();  // seeds entry 0 (the knob's own dedicated views)

int display_count();
const DisplayEntry *display_at(int idx);  // nullptr if out of range

// Resolve a menu SwitchView action. Local -> ui::show_by_id on the knob;
// remote -> queue a manager screen.set command. Returns true if dispatched.
bool switch_view(int dev_idx, int view_idx);

}  // namespace knob_remote
```

- [ ] **Step 2: Write `src/knob_remote.cpp` (local entry + local switch; remote stubbed until Phase F)**

```cpp
#include "knob_remote.h"

#include <string.h>

#include "ui_screens.h"

namespace knob_remote {

namespace {
DisplayEntry s_displays[kMaxDisplays];
int s_count = 0;

void add_local() {
    DisplayEntry &e = s_displays[0];
    memset(&e, 0, sizeof(e));
    strncpy(e.id, "", sizeof(e.id) - 1);
    strncpy(e.name, "This knob", sizeof(e.name) - 1);
    e.is_local = true;
    const char *vids[4] = {"ap_hud", "knob_compass", "knob_wind", "knob_big"};
    const char *vtitles[4] = {"Autopilot", "Compass", "Wind", "Big number"};
    for (int i = 0; i < 4; ++i) {
        strncpy(e.view_id[i], vids[i], sizeof(e.view_id[i]) - 1);
        strncpy(e.view_title[i], vtitles[i], sizeof(e.view_title[i]) - 1);
    }
    e.view_count = 4;
    e.current_view = 0;
    s_count = 1;
}
}  // namespace

void setup() {
    add_local();
}

int display_count() { return s_count; }

const DisplayEntry *display_at(int idx) {
    if (idx < 0 || idx >= s_count) return nullptr;
    return &s_displays[idx];
}

bool switch_view(int dev_idx, int view_idx) {
    const DisplayEntry *e = display_at(dev_idx);
    if (!e || view_idx < 0 || view_idx >= e->view_count) return false;
    if (e->is_local) {
        s_displays[dev_idx].current_view = view_idx;
        return ui::show_by_id(e->view_id[view_idx]);
    }
    // Remote: filled in Phase F (manager screen.set).
    return false;
}

}  // namespace knob_remote
```

- [ ] **Step 3: Build**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -10`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add include/knob_remote.h src/knob_remote.cpp
git commit -m "feat(knob): device/view registry with local dedicated views"
```

---

### Task E2: knob_ui — round views + menu overlay + Knob dispatch

**Files:**
- Create: `include/knob_ui.h`
- Create: `src/ui/knob_ui.cpp`
- Modify: `include/screens.h` (declare the four knob view namespaces)
- Modify: `src/app_events.cpp` (handle `CommandType::Knob` in `pump()`)
- Modify: `src/main.cpp` (register knob screens + call `knob_input::setup()` / `knob_remote::setup()` under the board gate)

This task is UI-heavy; follow the existing screen pattern in `src/ui/screen_autopilot.cpp` (build/refresh, dirty-value caches, `theme` colours, `lv_obj_align`). All four views are round-first: keep content inside the inscribed `usable_*` rect from `board::geometry()`.

- [ ] **Step 1: Declare knob view namespaces in `include/screens.h`**

Add inside `namespace ui { … }`:

```cpp
namespace ap_hud { lv_obj_t *build(lv_obj_t *parent); void refresh(); }
namespace knob_compass { lv_obj_t *build(lv_obj_t *parent); void refresh(); }
namespace knob_wind { lv_obj_t *build(lv_obj_t *parent); void refresh(); }
namespace knob_big { lv_obj_t *build(lv_obj_t *parent); void refresh(); }
namespace knob_menu_overlay {
lv_obj_t *build(lv_obj_t *parent);
void refresh();
void show(bool on);            // toggle the menu overlay on lv_layer_top()
}  // namespace knob_menu_overlay
```

- [ ] **Step 2: Write `include/knob_ui.h`**

```cpp
#pragma once

// Glue between the knob input events and the pure knob_menu state machine.
// Lives on the LVGL/UI task. apply_event() steps the machine, performs the
// resulting Action (autopilot PUT or remote view switch), and updates the
// menu overlay. Device-only.

namespace knob_ui {

void setup();                 // build overlay, init model + remote registry
void apply_event(int ev, bool held);  // ev = knob::Event; called from pump()

}  // namespace knob_ui
```

- [ ] **Step 3: Write `src/ui/knob_ui.cpp` — the dispatch core**

```cpp
#include "knob_ui.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

#include <math.h>
#include <string.h>

#include "app_events.h"
#include "knob_menu.h"
#include "knob_remote.h"
#include "net.h"
#include "screens.h"
#include "signalk_parser.h"

namespace knob_ui {

namespace {

knob::Model s_model;

knob::Inputs snapshot_inputs() {
    knob::Inputs in;
    static char state_buf[16];
    sk::Data d;
    sk::copyData(d);
    strncpy(state_buf, d.apState[0] ? d.apState : "", sizeof(state_buf) - 1);
    in.ap_state = state_buf;
    in.ap_target_rad = d.apTargetHdg;
    in.heading_rad = d.headingTrue;
    in.display_count = knob_remote::display_count();
    const knob_remote::DisplayEntry *e =
        s_model.level == knob::Level::SelectView ? knob_remote::display_at(s_model.entered_display)
                                                 : nullptr;
    in.view_count = e ? e->view_count : 0;
    return in;
}

void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[knob] state -> %s", state);
}

void put_target(double rad) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/target/headingTrue", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "%.4f", rad);
    app::post_net(cmd, 50);
}

void perform(const knob::Action &a) {
    switch (a.type) {
        case knob::ActionType::ApSetState: put_state(a.arg_str); break;
        case knob::ActionType::ApSetTargetRad: put_target(a.arg_f); break;
        case knob::ActionType::SwitchView:
            knob_remote::switch_view(a.arg_dev_idx, a.arg_view_idx);
            break;
        case knob::ActionType::NoAction: break;
    }
}

}  // namespace

void setup() {
    knob::init(s_model);
    knob_remote::setup();
    ui::knob_menu_overlay::build(nullptr);
    ui::knob_menu_overlay::show(false);
}

void apply_event(int ev, bool held) {
    knob::Inputs in = snapshot_inputs();
    knob::Action a = knob::step(s_model, in, (knob::Event)ev, held);
    perform(a);
    // Overlay visible whenever we're not on the autopilot Home level.
    ui::knob_menu_overlay::show(s_model.level != knob::Level::Home);
    ui::knob_menu_overlay::refresh();
}

}  // namespace knob_ui

#else
namespace knob_ui {
void setup() {}
void apply_event(int, bool) {}
}  // namespace knob_ui
#endif
```

- [ ] **Step 4: Handle `CommandType::Knob` in `src/app_events.cpp:pump()`**

Add a case in the `pump()` switch:

```cpp
        case CommandType::Knob:
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
            knob_ui::apply_event(cmd.i, cmd.b[0] == '1');
#endif
            break;
```

Add `#include "knob_ui.h"` at the top of `app_events.cpp`.

- [ ] **Step 5: Build the menu-overlay + four round views**

Create the bodies for `ap_hud`, `knob_compass`, `knob_wind`, `knob_big`, and `knob_menu_overlay` inside `src/ui/knob_ui.cpp` (or a sibling `src/ui/knob_views.cpp` — keep files focused). Follow `screen_autopilot.cpp` conventions:
  - `ap_hud`: mode badge + big target° + HDG + Δ, centred in the circle (reuse the autopilot refresh math; read `sk::Data`).
  - `knob_compass`: heading ring + HDG/COG number.
  - `knob_wind`: apparent wind angle dial + AWS number.
  - `knob_big`: one large value (depth; tap to toggle SOG).
  - `knob_menu_overlay`: a list on `lv_layer_top()` showing the current level. `refresh()` reads `knob_ui` model state via a small getter (add `const knob::Model &model();` to `knob_ui.h`), renders the level title + highlighted list (mode picker: 4 modes; SelectDisplay: `knob_remote` names with `#N`, highlighting `current_view`/active; SelectView: the entered display's view titles). `show(bool)` toggles `LV_OBJ_FLAG_HIDDEN`.

Acceptance for this step: it builds and the overlay shows/hides. Visual polish is iterated in Phase G against `make sim`.

- [ ] **Step 6: Register screens + start input under the board gate in `main.cpp`**

In the screen-registration block, add (knob board only):

```cpp
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    ui::register_screen_lazy("ap_hud", "Autopilot", ui::ap_hud::build, ui::ap_hud::refresh, false);
    ui::register_screen_lazy("knob_compass", "Compass", ui::knob_compass::build, ui::knob_compass::refresh, false);
    ui::register_screen_lazy("knob_wind", "Wind", ui::knob_wind::build, ui::knob_wind::refresh, false);
    ui::register_screen_lazy("knob_big", "Big", ui::knob_big::build, ui::knob_big::refresh, false);
    knob_ui::setup();
    knob_input::setup();
    ui::show_by_id("ap_hud");
#else
    // ... existing screen registration ...
#endif
```

Add includes `#include "knob_ui.h"` and `#include "knob_input.h"` to `main.cpp`. Ensure the non-knob registration path is otherwise unchanged.

- [ ] **Step 7: Build both envs**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -15 && pio run -e esp32-4848s040 2>&1 | tail -5`
Expected: both `SUCCESS`.

- [ ] **Step 8: Commit**

```bash
git add include/knob_ui.h src/ui/knob_ui.cpp include/screens.h src/app_events.cpp src/main.cpp
git commit -m "feat(knob): menu UI overlay, round views, and Knob event dispatch"
```

---

### Task E3: Bench verification on hardware

**Files:** none (manual verification)

- [ ] **Step 1: Flash the knob over USB**

Run: `pio run -e waveshare-knob-1_8 -t upload 2>&1 | tail -10`
Expected: upload OK; device boots to the Autopilot HUD.

- [ ] **Step 2: Verify gestures via serial log**

Run: `pio device monitor -e waveshare-knob-1_8` and exercise: one detent CW = one `target ->` log line (not 4); hold + rotate = ±5; click toggles state standby↔auto; long-press opens the mode picker; double-click opens Select Display; double-click again returns home.
**If one detent emits 4 events:** set `kCountsPerDetent` accordingly (try the `attachHalfQuad` variant or divisor 2) in `knob_input.cpp` and re-flash.

- [ ] **Step 3: Verify autopilot reaches the sim**

With the lab SignalK + KDCube sim running, confirm a knob heading change and a mode change show up on the bridge (the same `steering.autopilot.*` path the on-screen autopilot uses).

- [ ] **Step 4: Soak before any stability claim**

Run: `python3 tools/yeydisp.py soak --remote compulab@mythra-nav` (per the soak rig). Expected: PASS.

- [ ] **Step 5: Commit any tuning**

```bash
git add src/knob_input.cpp
git commit -m "fix(knob): tune encoder counts-per-detent from bench"
```

---

## Phase F — Manager remote enumeration + switch

### Task F1: Plugin read endpoints (`GET /devices`, `GET /devices/:id/views`)

**Files:**
- Modify: `signalk/plugins/signalk-espdisp-manager/index.js` (route registration)
- Modify: `signalk/plugins/signalk-espdisp-manager/lib/manager.js` (projection helpers)
- Modify/Create: `signalk/plugins/signalk-espdisp-manager/test/*` (a test for the projections)

- [ ] **Step 1: Read the existing device registry + command route**

Read `lib/manager.js` around the device registry (`store.registry.devices`) and the existing `POST /devices/:id/command` handler and `index.js` route table to match the response/projection style.

- [ ] **Step 2: Add a `GET /devices` summary route**

Returns `[{ id, name, role, online, currentScreen }]` projected from the registry. Mirror the auth/middleware the other device routes use.

- [ ] **Step 3: Add a `GET /devices/:id/views` route**

Returns `{ views: [{ id, title }], current }` derived from the device's stored config/profile screen list. If a device has no stored screen list, fall back to the standard known view ids.

- [ ] **Step 4: Add a projection test**

Mirror an existing test in `signalk/plugins/signalk-espdisp-manager/test/`. Seed a fake registry, call the projection helpers, assert the shapes.

- [ ] **Step 5: Run the plugin tests**

Run: `cd signalk/plugins/signalk-espdisp-manager && npm test 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add signalk/plugins/signalk-espdisp-manager
git commit -m "feat(plugin): GET /devices + GET /devices/:id/views projections"
```

---

### Task F2: Firmware fetch of remote displays + remote switch

**Files:**
- Modify: `src/knob_remote.cpp` (fill remote entries from the manager; implement remote `switch_view`)
- Modify: `src/manager.cpp` (add a client call to GET the device + view lists if not already exposed)

- [ ] **Step 1: Find the manager client GET/POST helpers**

Read `src/manager.cpp` for the existing HTTP client (base URL, token header, JSON parse into PSRAM). Reuse it; do not hand-roll a second HTTPClient.

- [ ] **Step 2: Populate remote entries**

Add a `knob_remote::refresh_from_manager()` that GETs `/devices` and, lazily on drill-in, `/devices/:id/views`, filling `s_displays[1..N]` (cap at `kMaxDisplays`). Call it on a slow cadence (e.g. piggyback the manager poll) — never from the LVGL task’s hot path; post results via the existing manager worker → store into the registry under the same lock discipline the manager uses.

- [ ] **Step 3: Implement remote `switch_view`**

For a non-local entry, queue the existing manager command (`screen.set` with the resolved `view_id`) to that device id — reuse the manager client’s command-post path. The target device already executes `screen.set` (`src/manager.cpp:1296`) and applies instantly via `configPush`.

- [ ] **Step 4: Build + flash + verify a real second display switches view**

Run: `pio run -e waveshare-knob-1_8 -t upload 2>&1 | tail -10`
Then from the knob: Select Display → pick a remote MFD → Select View → pick a view → confirm the remote display switches.

- [ ] **Step 5: Commit**

```bash
git add src/knob_remote.cpp src/manager.cpp
git commit -m "feat(knob): enumerate + remote-switch displays via the manager"
```

---

## Phase G — Documentation, screenshots, plugin/deploy guide

### Task G1: Round knob views in the `make sim` gallery

**Files:**
- Modify: `sim/sim_main.cpp` (add a 360×360 round profile rendering the knob views)
- Modify: `tools/sim_render.sh` (add the knob resolution invocation)
- Modify: `platformio.ini` `[env:sim]` `build_src_filter` (include the knob view sources)

- [ ] **Step 1: Add a 360 round render path**

Follow the existing sim harness pattern (it already renders real layouts at arbitrary `-DSIM_LCD_W/H`). Render `ap_hud`, `knob_compass`, `knob_wind`, `knob_big` at 360×360 and snapshot PNGs into `docs/sim-shots/` (or the existing gallery dir).

- [ ] **Step 2: Generate the shots**

Run: `make sim 2>&1 | tail -20` (or the documented per-resolution script)
Expected: PNGs written; no overlap/bounds assertion failures.

- [ ] **Step 3: Commit**

```bash
git add sim/ tools/sim_render.sh platformio.ini docs/sim-shots/
git commit -m "feat(sim): render round 360 knob views for the gallery"
```

---

### Task G2: README — supported board + Remote Knob section + screenshots

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add the knob to the supported-boards table**

Row: board name, env `waveshare-knob-1_8`, 360×360 round, ST77916 QSPI, encoder input.

- [ ] **Step 2: Add a "Remote Knob" section**

Cover: what it is (rotary remote + dedicated round views), the gesture cheat-sheet (scroll=±1°, hold+scroll=±5°, click=engage, long-press=mode, double-click=menu), the menu map (Autopilot home → Select Display → Select View), and embed the Phase-G1 screenshots.

- [ ] **Step 3: Commit**

```bash
git add README.md docs/
git commit -m "docs(readme): supported knob board + Remote Knob section + shots"
```

---

### Task G3: Plugin install + knob deploy guide + user guide

**Files:**
- Modify: `docs/user-guide-signalk.md` (knob usage walkthrough)
- Modify: `docs/signalk-espdisp-manager.md` (install section)
- Create: `docs/remote-knob.md` (deploy & use), cross-linked from README

- [ ] **Step 1: "Install the SignalK plugin" section**

In `docs/signalk-espdisp-manager.md` (or a new "Install" heading): install `signalk-espdisp-manager` from `signalk/plugins/`, enable it in the SignalK admin, first-run admin user note (mirror the existing demo auth note in CLAUDE.md).

- [ ] **Step 2: "Deploy & use the remote knob" (`docs/remote-knob.md`)**

Steps: flash `waveshare-knob-1_8` (`make`/`pio run -e waveshare-knob-1_8 -t upload`), provision/identity (`manager-token espdisp-dev`, `id <name>`), confirm it appears in the manager device list, pair/use the menu to drive other displays, and the autopilot control walkthrough. Include the gesture cheat-sheet.

- [ ] **Step 3: User guide walkthrough**

In `docs/user-guide-signalk.md`, add a knob section: the four dedicated views and how the menu controls autopilot + other displays.

- [ ] **Step 4: Lint docs / commit**

Run: `make pre-commit 2>&1 | tail -10`
Expected: PASS.

```bash
git add docs/ README.md
git commit -m "docs: SignalK plugin install + remote-knob deploy & usage guide"
```

---

### Task G4: Final verification + push

- [ ] **Step 1: Full host test suite**

Run: `pio test -e native 2>&1 | tail -15`
Expected: all PASS (including `test_knob_menu`).

- [ ] **Step 2: Build all affected envs**

Run: `pio run -e waveshare-knob-1_8 2>&1 | tail -5 && pio run -e esp32-4848s040 2>&1 | tail -5 && pio run -e sim 2>&1 | tail -5`
Expected: all `SUCCESS`.

- [ ] **Step 3: Lint**

Run: `make pre-commit 2>&1 | tail -10`
Expected: PASS (clang-format clean; remember the `clang-format off` guards in headers).

- [ ] **Step 4: Push**

```bash
git push origin main
```

---

## Phase H — SignalK peer-to-peer fallback (follow-up; optional for v1)

Scoped per spec §4.2. Land only after A–G are stable. Summary of tasks (expand into a separate plan when scheduled):
- Firmware: subscribe to `network.espdisp.remoteControl {target, screen}` in `src/signalk.cpp`; when `target == device_id`, run the existing `screen.set` path.
- Firmware: publish `network.espdisp.devices.<id>` (name, current screen, view list) so peers can enumerate without the manager.
- Knob: when the manager is unreachable, populate `knob_remote` from the SignalK `network.espdisp.devices.*` subtree and publish `remoteControl` deltas instead of manager commands.
- Tests: parser tests for the new SK paths in `test/test_parser`.

---

## Self-review notes

- **Spec coverage:** §2 HW → A1; §3.1 board → A1–A4 (with the documented `LayoutClass` refinement); §3.2 display → C1; §3.3 input → D1; §3.4 menu → B1–B4 (pure, host-tested); §3.5 dedicated views → E2; §4.1 manager remote → F1–F2; §4.2 SK fallback → H; §5 docs → G1–G3; §6 testing → B + E3 + G4; §7 memory traps → C1 (skill-gated) + B (PODs zeroed via `init`); §8 risks → C1 Step 1 (GFX version), D1 (counts-per-detent), F1 (view-list source).
- **Type consistency:** `knob::Event`, `knob::Level`, `knob::ActionType`, `knob::Action{arg_str,arg_f,arg_dev_idx,arg_view_idx}`, `knob::Inputs`, `knob::Model` are defined once in B1 and used unchanged through B/E. `knob_remote::DisplayEntry` defined in E1, used in E2/F2. `app::CommandType::Knob` added in D1, consumed in E2.
- **No placeholders:** host-testable core (A, B) is complete real code; firmware glue (C–F) gives real code for the dispatch path and explicit acceptance/commands; the LVGL view *painting* in E2 Step 5 is intentionally specified as "follow screen_autopilot.cpp pattern" because pixel layout is iterated visually against `make sim` in G1 — the wiring it depends on is fully specified.
