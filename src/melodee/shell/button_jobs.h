// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"

// Like the stock MPC Sample, a button that does something on the page in
// view glows faintly, one in use is bright, and one that would do nothing
// here is dark. This says which do something; lights.cpp picks the level.
namespace melodee {

// Whether `button` has a job on ui.page for the selected track. `kit_pads`:
// the pads play the track's kit one pad each (Shell::kit_pads()); `bindings`
// has an Erase + pot job (a parameter on a pot).
bool button_has_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads, bool erase_pots);

} // namespace melodee
