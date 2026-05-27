# 05 Settings UI

**Goal**: every settings control changes device state and persists
across reboot.

## Steps

1. Swipe up from dashboard → settings drawer. ⬜
2. **Brightness** slider — drag to mid. Backlight dims. Drag to
   100 %, backlight returns. ⬜
3. **Demo mode** toggle — turn off; live SignalK values stop being
   replaced. Turn on; demo pattern resumes (only relevant when no
   data source is live). ⬜
4. **Theme** picker — cycle through Day / Night / Auto. Background
   and accent colors change instantly. ⬜
5. Power-cycle the device. Settings retained: brightness level
   matches, theme matches, demo toggle matches. ⬜

## Pass criteria

All UI controls have visible effects and persist.

## If a control has no effect

- The config_runtime subsystem (spec 08) may not have flushed; wait
  ~2 s and check `[config] saved <key>` in serial log.
- Brightness with no effect → ensure `board::set_backlight` is wired
  to the actual pin (`board bright 128` from CLI should also work).
