#include "beeper.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "net.h"
#include "storage.h"

namespace beeper {

namespace {

constexpr const char *NS = "beep";

bool s_audible = true;

// Stub backend: no GPIO toggle. When a board with a beeper lands we
// add a tone(pin, freq) or ledc PWM here, behind board::capabilities().
struct AlarmRequest {
    uint32_t on_ms;
    uint32_t off_ms;
    uint16_t repeat;
    bool active;
};
volatile AlarmRequest s_alarm = {};
TaskHandle_t s_task = nullptr;
volatile uint32_t s_short_pending = 0;

void hw_beep_on() {
    // Backend hook. Sunton 4848S040 has no beeper - log only.
}
void hw_beep_off() {
    // ditto
}

void worker(void *) {
    for (;;) {
        // Short tap?
        if (s_short_pending) {
            uint32_t ms = s_short_pending;
            s_short_pending = 0;
            hw_beep_on();
            vTaskDelay(pdMS_TO_TICKS(ms));
            hw_beep_off();
        }
        // Continuous pattern?
        if (s_alarm.active) {
            uint16_t remaining = s_alarm.repeat;
            uint32_t on = s_alarm.on_ms;
            uint32_t off = s_alarm.off_ms;
            while (s_alarm.active && (remaining > 0 || s_alarm.repeat == 0)) {
                hw_beep_on();
                vTaskDelay(pdMS_TO_TICKS(on));
                hw_beep_off();
                vTaskDelay(pdMS_TO_TICKS(off));
                if (s_alarm.repeat > 0) remaining--;
            }
            // mark done
            s_alarm.active = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void load_prefs() {
    storage::Namespace p(NS, true);
    s_audible = p.get_u8("audible", 1) != 0;
}

void save_prefs() {
    storage::Namespace p(NS, false);
    p.put_u8("audible", s_audible ? 1 : 0);
}

}  // namespace

bool available() {
    return board::capabilities().beeper;
}
bool audible_alarms_enabled() {
    return s_audible;
}

void set_audible_alarms(bool enabled) {
    s_audible = enabled;
    save_prefs();
    net::logf("[beeper] audible_alarms=%s", enabled ? "on" : "off");
}

void beep_short(uint32_t duration_ms) {
    if (!s_audible) return;
    if (!available()) {
        net::logf("[beeper] short %lu ms (no hw)", (unsigned long)duration_ms);
        return;
    }
    s_short_pending = duration_ms ? duration_ms : 50;
}

void alarm_pattern(uint32_t on_ms, uint32_t off_ms, uint16_t repeat) {
    if (!s_audible) return;
    if (!available()) {
        net::logf("[beeper] alarm on=%lu off=%lu rep=%u (no hw)", (unsigned long)on_ms,
                  (unsigned long)off_ms, (unsigned)repeat);
        return;
    }
    s_alarm.on_ms = on_ms;
    s_alarm.off_ms = off_ms;
    s_alarm.repeat = repeat;
    s_alarm.active = true;
}

void alarm_stop() {
    s_alarm.active = false;
}

void setup() {
    load_prefs();
    net::logf("[beeper] available=%d audible=%d", available(), s_audible);
    if (!s_task) {
        xTaskCreatePinnedToCore(worker, "beeper", 2048, nullptr, tskIDLE_PRIORITY + 1, &s_task, 0);
    }
}

bool handleSerialCommand(const String &line) {
    if (line == "beep") {
        beep_short(50);
        return true;
    }
    if (line.startsWith("beep ")) {
        uint32_t ms = (uint32_t)line.substring(5).toInt();
        beep_short(ms);
        return true;
    }
    if (line.startsWith("beep-alarm ")) {
        String rest = line.substring(11);
        rest.trim();
        int sp1 = rest.indexOf(' ');
        int sp2 = sp1 >= 0 ? rest.indexOf(' ', sp1 + 1) : -1;
        if (sp1 < 0 || sp2 < 0) {
            net::logf("[beeper] usage: beep-alarm <on_ms> <off_ms> <count>");
            return true;
        }
        uint32_t on = rest.substring(0, sp1).toInt();
        uint32_t off = rest.substring(sp1 + 1, sp2).toInt();
        uint16_t rep = (uint16_t)rest.substring(sp2 + 1).toInt();
        alarm_pattern(on, off, rep);
        return true;
    }
    if (line == "beep-stop") {
        alarm_stop();
        net::logf("[beeper] stop");
        return true;
    }
    if (line.startsWith("audible-alarms ")) {
        String v = line.substring(15);
        v.trim();
        set_audible_alarms(v == "on" || v == "1" || v == "true");
        return true;
    }
    return false;
}

}  // namespace beeper
