# Specs

This directory contains product and implementation specs for the ESP32 boat MFD.

Files:

- `00-findings.md` - current architecture/refactor findings.
- `01-visual-reference-model.md` - visual patterns from open-source marine
  projects and commercial MFD conventions.
- `02-layout-system-spec.md` - layout document, screen type, and widget
  addressing model.
- `03-screen-specs.md` - per-screen visualization and interaction specs.
- `04-layout-presets.md` - recommended default screen stacks for common users.
- `05-gesture-subsystem-spec.md` - asynchronous touch and gesture subsystem
  design.
- `06-ui-interactions.md` - user-facing interaction model and gesture behavior.
- `07-event-driven-mvc.md` - proposed event-driven MVC/MVU-lite architecture
  and migration path.
- `08-configuration-storage-sync.md` - configuration storage, source
  precedence, local/external synchronization, and conflict handling.
- `09-rendering-performance-interactivity.md` - rendering profiling,
  partial-redraw strategy, and touch/interactivity improvements.
- `10-ui-appearance-redesign-session.md` - current visual redesign direction,
  theme decisions, helper API, and rollout checklist.
- `11-layout-templates-screen-variants.md` - reusable layout template catalog,
  screen variant mapping, tap-to-detail behavior, and implementation plan.
- `12-nmea2000-and-visual-adoption.md` - direct NMEA2000, smoothing,
  autopilot, board support, beeper, and adopted visual design concepts.
- `13-board-display-portability.md` - board/display abstraction, layout
  geometry classes, generalized settings, and multi-board migration plan.

Use these specs when implementing the data-driven renderer, refining individual
screens, or adding Signal K plugin/device-management features.
