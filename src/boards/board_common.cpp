#include "board.h"

#include <math.h>

#if defined(ARDUINO) && !defined(YEYBOATS_NATIVE_BUILD)
#include <Arduino.h>
#endif

namespace board {

float chipTempC() {
#if defined(ARDUINO) && !defined(YEYBOATS_NATIVE_BUILD)
    // Arduino-ESP32 wraps the SoC temperature sensor and returns NAN on
    // boards/cores where it's not wired up. Pass the raw value through
    // - callers must already cope with NaN (the BatteryV/Depth/etc.
    // formatters all do).
    return temperatureRead();
#else
    return NAN;
#endif
}

const char *shape_name(DisplayShape shape) {
    switch (shape) {
    case DisplayShape::Rectangle:
        return "rectangle";
    case DisplayShape::Square:
        return "square";
    case DisplayShape::Round:
        return "round";
    }
    return "rectangle";
}

const char *density_class_name(DensityClass density) {
    switch (density) {
    case DensityClass::Mdpi:
        return "mdpi";
    case DensityClass::Hdpi:
        return "hdpi";
    }
    return "mdpi";
}

const char *layout_class_name(LayoutClass layout) {
    switch (layout) {
    case LayoutClass::SquareCompact:
        return "square-480";
    case LayoutClass::LandscapeCompact:
        return "landscape-800x480";
    case LayoutClass::LandscapeWide:
        return "landscape-1024x600";
    case LayoutClass::PortraitCompact:
        return "portrait-320x480";
    case LayoutClass::PortraitTall:
        return "portrait-tall";
    }
    return "square-480";
}

const char *touch_kind_name(TouchKind touch) {
    switch (touch) {
    case TouchKind::None:
        return "none";
    case TouchKind::GT911:
        return "GT911";
    case TouchKind::FT5x06:
        return "FT5x06";
    case TouchKind::CST816:
        return "CST816";
    case TouchKind::XPT2046:
        return "XPT2046";
    case TouchKind::BoardSpecific:
        return "board-specific";
    }
    return "none";
}

const char *display_bus_name(DisplayBus bus) {
    switch (bus) {
    case DisplayBus::RgbParallel:
        return "rgb-parallel";
    case DisplayBus::Spi:
        return "spi";
    case DisplayBus::Qspi:
        return "qspi";
    case DisplayBus::DsiBridge:
        return "dsi-bridge";
    }
    return "rgb-parallel";
}

}  // namespace board
