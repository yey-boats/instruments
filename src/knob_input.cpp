#include "knob_input.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <OneButton.h>

#include "app_events.h"
#include "board_pins.h"
#include "knob_menu.h"
#include "net.h"
#include "storage.h"

namespace knob_input {

namespace {

// NVS namespace + keys for runtime encoder calibration. Short to satisfy the
// 15-char NVS key limit and distinct from existing namespaces ("ui", "net",
// "sk", "beep", ...).
constexpr const char *NS = "knob";

ESP32Encoder s_enc;
OneButton s_btn(ENC_BTN, /*activeLow=*/true, /*pullupActive=*/true);
int64_t s_last_count = 0;

// Runtime-tunable encoder constants. Defaults reproduce the original
// compile-time behavior (4 counts/detent, non-inverted). PCNT counts 4
// transitions per detent on full-quadrature; divide so one physical detent =
// one event.
int s_counts_per_detent = 4;
bool s_invert = false;

void post_event(knob::Event ev, bool held = false) {
    app::Command c;
    c.type = app::CommandType::Knob;
    c.i = (int32_t)ev;
    c.b[0] = held ? '1' : '0';
    c.b[1] = '\0';
    c.t_post_us = micros();
    app::post(c, 0);
}

void on_click() {
    post_event(knob::Event::Click);
}
void on_double() {
    post_event(knob::Event::DoubleClick);
}
void on_long_start() {
    post_event(knob::Event::LongPress);
}

void knob_task(void *) {
    for (;;) {
        s_btn.tick();
        // Snapshot the runtime calibration once per pass so a concurrent
        // set_* never splits a step (e.g. add to one limb, subtract on the
        // other). Read live so console tuning applies without a reboot.
        int cpd = s_counts_per_detent;
        if (cpd < 1) cpd = 1;  // defensive: never divide-by-zero / spin
        bool inv = s_invert;
        int64_t count = s_enc.getCount();
        int64_t delta = count - s_last_count;
        // Read button pin at the moment of detent emission — this is the only
        // point where the held flag matters. ENC_BTN is active-low with pull-up:
        // pressed == LOW. Avoids the stale-flag bug where a short click left
        // s_held=true via OneButton callbacks (longPressStop never fired).
        bool held = (digitalRead(ENC_BTN) == LOW);
        // invert swaps which physical rotation emits CW vs CCW: a positive
        // delta normally means CW; when inverted we emit CCW for it (and vice
        // versa). s_last_count still advances by the raw delta either way, so
        // the accounting is independent of polarity.
        knob::Event pos_ev = inv ? knob::Event::DetentCCW : knob::Event::DetentCW;
        knob::Event neg_ev = inv ? knob::Event::DetentCW : knob::Event::DetentCCW;
        while (delta >= cpd) {
            post_event(pos_ev, held);
            s_last_count += cpd;
            delta -= cpd;
        }
        while (delta <= -cpd) {
            post_event(neg_ev, held);
            s_last_count -= cpd;
            delta += cpd;
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

    // Load runtime calibration after encoder init; defaults reproduce the
    // original compile-time behavior when the keys are absent (fresh device).
    {
        storage::Namespace p(NS, true);
        int cpd = (int)p.get_u8("cpd", 4);
        if (cpd < 1) cpd = 1;
        if (cpd > 8) cpd = 8;
        s_counts_per_detent = cpd;
        s_invert = p.get_bool("invert", false);
    }

    // ENC_BTN is read directly in knob_task at detent time for the held flag;
    // ensure the pin is configured as input with pull-up for reliable reads.
    pinMode(ENC_BTN, INPUT_PULLUP);

    s_btn.attachClick(on_click);
    s_btn.attachDoubleClick(on_double);
    s_btn.attachLongPressStart(on_long_start);
    s_btn.setPressMs(400);  // long-press threshold

    xTaskCreatePinnedToCore(knob_task, "knob", 4096, nullptr, 2, nullptr, 0);
}

void set_counts_per_detent(int n) {
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    s_counts_per_detent = n;  // applied live (knob_task re-reads each pass)
    storage::Namespace p(NS, false);
    p.put_u8("cpd", (uint8_t)n);
    net::logf("[knob] counts_per_detent=%d", n);
}

int counts_per_detent() {
    return s_counts_per_detent;
}

void set_invert(bool on) {
    s_invert = on;  // applied live
    storage::Namespace p(NS, false);
    p.put_bool("invert", on);
    net::logf("[knob] invert=%s", on ? "on" : "off");
}

bool invert() {
    return s_invert;
}

}  // namespace knob_input

#else  // not the knob board

namespace knob_input {
void setup() {
}
void set_counts_per_detent(int) {
}
int counts_per_detent() {
    return 4;
}
void set_invert(bool) {
}
bool invert() {
    return false;
}
}  // namespace knob_input

#endif
