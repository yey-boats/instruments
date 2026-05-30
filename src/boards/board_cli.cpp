#include <Arduino.h>

#include "board.h"
#include "net.h"

// Tiny CLI shim for the board abstraction. Reachable from serial / BLE
// via net::dispatchCommand. Kept here (not in the board impl) so each
// board file stays focused on hardware.

namespace board {

bool handleSerialCommand(const String &line) {
    if (line != "board" && !line.startsWith("board ")) return false;
    if (line == "board") {
        Geometry g = geometry();
        Capabilities c = capabilities();
        net::logf("[board] id=%s name=%s", id(), display_name());
        net::logf("[board] geom %ux%u rot=%u %.1f\" class=%d", g.width_px, g.height_px, g.rotation,
                  g.diagonal_tenths_in / 10.0f, (int)g.layout_class);
        net::logf("[board] caps psram=%d backlight=%d touch=%d cal=%d "
                  "beeper=%d can=%d sd=%d",
                  c.psram_required, (int)c.backlight, (int)c.touch, c.touch_calibration, c.beeper,
                  c.nmea2000_can, c.sd_card);
        net::logf("[board] backlight=%u", backlight());
        return true;
    }
    if (line.startsWith("board bright ")) {
        int v = line.substring(13).toInt();
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        set_backlight((uint8_t)v);
        net::logf("[board] backlight set %d", v);
        return true;
    }
    net::logf("[board] usage: board | board bright <0-255>");
    return true;
}

}  // namespace board
