// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/button_jobs.h"

namespace melodee {

bool button_has_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads, bool erase_pots) {
    using B = SurfaceButton;
    if (ui.exit_open) return false; // only B1 and B3 answer, and they have no lights
    if (ui.name.open()) return button == B::Erase || button == B::Play || button == B::Stop; // the Name sheet
    const auto& track = project.tracks[ui.track % track_count];
    bool kit = track.engine == EngineKind::Sampler;
    switch (button) {
    case B::Chop: return kit && track.pads[track.last_pad % sampler_pads].used(); // the last pad's sample
    case B::Loop: return kit;          // the last pad's loop (its Play page until phase 4); Shift reverse
    case B::SampleRecord: return kit;  // Record; elsewhere it only says it wants a kit
    case B::Erase: // pads that clear, steps, chop markers, a pot with a parameter, or Copy on kit pads and Patterns
        return kit_pads || erase_pots || ui.page == PageId::Patterns || ui.page == PageId::Steps || ui.page == PageId::Chop
            || ui.page == PageId::Motion || ui.copying != UiState::Copying::Off;
    default: return true; // the page buttons, Pad Bank, Mute, Note Repeat, 16 Levels, transport: everywhere
    }
}

} // namespace melodee
