# Touch Interrupt Testing

Status: proposed hardware validation spec.

## Goal

Validate whether a board can run GT911 touch input without idle polling. The
test proves two things:

- the GT911 `INT` line is routed to an ESP32 GPIO and wakes the touch task
- firmware falls back cleanly to timed polling when no interrupt pin exists

## Scope

This spec covers the GT911 path used by the ESP32-4848S040 class boards. Other
touch controllers may expose a similar interrupt pin, but their status registers
and clear sequence are controller-specific.

## Firmware Behavior

The touch task has two modes:

- `poll`: `TOUCH_INT < 0`; read GT911 every 16 ms, matching the original code.
- `irq`: `TOUCH_INT >= 0`; block while idle until the GPIO interrupt fires,
  then read GT911 over I2C.

In `irq` mode the ISR must only wake the touch task. It must not access I2C,
LVGL, logging, or heap allocation.

After an interrupt wake, the touch task reads GT911 status register `0x814E`,
reads the first point from `0x8150` when present, and clears `0x814E` to `0`.
While a finger remains down, the task may continue timed reads at the normal
16 ms cadence so movement and release are not missed on boards/controllers that
only interrupt on initial contact. Once released, it returns to blocking on the
interrupt.

## Board Setup

For a board with GT911 INT wired:

```cpp
#define TOUCH_INT <gpio>
#define TOUCH_INT_ACTIVE_LOW 1
```

The same values may be supplied by a board-specific header or by build flags,
for example `-DTOUCH_INT=2 -DTOUCH_INT_ACTIVE_LOW=1`.

Use `TOUCH_INT_ACTIVE_LOW 0` only if the board/controller configuration is known
to pulse active-high.

For the current Sunton/Guition ESP32-4848S040 definition, keep:

```cpp
#define TOUCH_INT -1
```

because the INT line is not known to be routed.

## Test Procedure

### Polling Fallback

Build and boot with `TOUCH_INT -1`.

Acceptance:

- boot log contains `GT911 probe: ACK`
- boot log contains `touch input mode: poll`
- `/api/state.touch.mode` is `poll`
- `/api/state.touch.irq` remains `0`
- taps, drags, calibration, and swipes still work

### Interrupt-Wired Board

Build and boot with `TOUCH_INT` set to the routed GPIO.

Acceptance:

- boot log contains `touch input mode: irq gpio=<gpio>`
- `/api/state.touch.mode` is `irq`
- `/api/state.touch.irq` increments when the panel is touched
- `/api/state.touch.i2c_ok` does not climb continuously while the panel is idle
- first tap after several idle seconds is detected
- drag movement updates coordinates
- release changes `/api/state.touch.pressed` to `0`
- swipes still post the same navigation actions as polling mode

### Negative Wiring Test

Build with an intentionally wrong `TOUCH_INT` GPIO on a development board.

Expected result:

- firmware boots
- `/api/state.touch.mode` is `irq`
- `/api/state.touch.irq` does not increment when touched
- touch does not respond reliably

This confirms the test distinguishes a real routed interrupt from a merely
compiled interrupt mode.

## Notes

Interrupt mode removes idle polling. It does not remove I2C reads; the interrupt
only tells the host when GT911 has data ready. Coordinate transfer still happens
over I2C in the touch task.
