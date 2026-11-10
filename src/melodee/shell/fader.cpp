// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/fader.h"
#include "melodee/shell/context.h"
#include "melodee/shell/step_edit.h"

namespace melodee {

void fader_move(Context& c, int value, bool syncing) {
    auto& ui = c.ui;
    int before = ui.fader_at;
    ui.fader_at = int16_t(value);
    if (ui.page == PageId::StepEdit) { // nudge: by how far it went (the fader's place itself means nothing here)
        if (!syncing && before >= 0 && ui.ask == UiState::Ask::None) nudge_event(c, value - before);
        return;
    }
    c.send({Command::Type::Wheel, ui.track, 0, 0, int16_t(value * 1000 / 127)});
}

} // namespace melodee
