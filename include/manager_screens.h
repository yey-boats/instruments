#pragma once

// Spec 19 D5 - variant application.
//
// Take a validated manager_config::RenderPlan and build an LVGL
// screen from its first ScreenPlan. Widget instances are kept in a
// small static array so the refresh fn can walk them on each
// ui_refresh tick.
//
// MVP: at most one managed screen (the first in the plan). Multi-
// screen support + plan-update-in-place are follow-ups.
//
// Apply is idempotent in the sense that calling it twice is safe -
// the second call logs "already applied" and ignores the new plan.
// Use clear() + apply() if you need to rebuild (D5 follow-up).

#include <lvgl.h>

#include "manager_config.h"

namespace manager_screens {

// Build LVGL screen + widgets from `plan.screens[0]`. Registers the
// screen with ui::register_screen as "mgr_main" so it joins the
// carousel. Returns true on first successful apply; false if a
// managed screen is already in place or the plan has no screens.
bool apply(const manager_config::RenderPlan &plan);

// Refresh hook called from the registered Screen.refresh callback.
// No-op if no managed screen is active or the current LVGL screen
// isn't ours.
void refresh();

// True iff a managed screen has been built and is reachable in the
// screen carousel.
bool is_applied();

}  // namespace manager_screens
