// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <string>

// Questions asked before an erase, as the MPC asks them: B1 Cancel, B3 Do
// it. While one is open the other controls wait, except Play and Stop.
namespace melodee {

class Context;

void ask(Context& c, UiState::Ask kind, int target);
// A press while a question is open: true when it was taken (answered or
// ignored), false for the ones that still do their job (Play, Stop).
bool ask_button(Context& c, SurfaceButton button);
// What the open question asks ("Erase A03 events?"), and how much Do it
// erases ("12 events in P1"; `pattern` is the selected track's playing one).
std::string ask_title(const Project& project, const UiState& ui);
std::string ask_detail(const Project& project, const UiState& ui, const Pattern& pattern);

} // namespace melodee
