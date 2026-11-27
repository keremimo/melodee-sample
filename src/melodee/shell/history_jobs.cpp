// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/history_jobs.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/history_list.h"
#include <algorithm>
#include <string>

namespace melodee {

void history_open(Context& c) {
    auto& ui = c.ui;
    if (ui.history || ui.menu || ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open || ui.loading || card_owns_controls(ui) || ui.lesson) return;
    if (!c.edits.can_undo() && !c.edits.can_redo()) { c.notice("Nothing to undo"); return; }
    ui.history = true;
    ui.history_row = uint16_t(history_now(unsigned(c.edits.redos())));
}

void history_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    unsigned rows = history_rows(unsigned(c.edits.redos()), unsigned(c.edits.size()));
    ui.history_row = uint16_t(std::clamp(int(ui.history_row) + delta, 0, int(rows) - 1));
}

// Undo or redo as many steps as it takes, one at a time, as Shift + Minus / Plus would: a step that waits (a track
// move under a take) or asks first (a frozen track) stops it there, with its own notice or question.
void history_go(Context& c) {
    auto& ui = c.ui;
    ui.history = false;
    const int step = history_step(ui.history_row, unsigned(c.edits.redos()));
    int done = 0;
    for (int n = 0; n < std::abs(step); ++n) {
        const size_t before = c.edits.redos();
        if (step < 0) c.undo(); else c.redo();
        if (c.edits.redos() == before) break; // it waits or asks first: it says why
        ++done;
    }
    if (done > 1) c.notice(std::string(step < 0 ? "Undo " : "Redo ") + std::to_string(done) + " steps");
}

bool history_button(Context& c, SurfaceButton b) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (!ui.history) return false;
    switch (b) {
    case B::TopLeft: ui.history = false; return true;        // BACK
    case B::TopMiddle: return true;                          // no job
    case B::TopRight: history_go(c); return true;            // UNDO n / REDO n
    case B::Minus: history_wheel(c, -1); return true;        // up the list
    case B::Plus: history_wheel(c, 1); return true;          // down the list
    case B::Play: case B::Stop: case B::Shift: case B::WheelPush: return false; // the transport and Shift, the list left open; Push acts when let go
    default: ui.history = false; return false;               // any other button: its own job, the list closed
    }
}

} // namespace melodee
