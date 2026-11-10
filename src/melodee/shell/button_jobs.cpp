// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/stage_map.h"

namespace melodee {

bool button_has_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads, bool erase_pots) {
    using B = SurfaceButton;
    if (ui.exit_open) return false; // only B1 and B3 answer, and they have no lights
    if (ui.name.open()) return button == B::Erase || button == B::Play || button == B::Stop; // the Name sheet
    if (ui.page == PageId::CardAccess || ui.card.state != 0) { // SD card access: everything else waits (shell/card_access.h)
        if (ui.ask != UiState::Ask::None) return button == B::TopLeft || button == B::TopRight || button == B::Play || button == B::Stop;
        return button == B::Play || button == B::Stop || (button == B::TopLeft && (ui.card.state == 0 || ui.card.state == 2));
    }
    if (ui.ask != UiState::Ask::None) // a question: B1 Cancel, B3 Do it, and the transport
        return button == B::TopLeft || button == B::TopRight || button == B::Play || button == B::Stop;
    const auto& track = project.tracks[ui.track % track_count];
    bool kit = track.engine == EngineKind::Sampler;
    switch (button) {
    case B::Chop: return kit && track.pads[track.last_pad % sampler_pads].used(); // the last pad's sample
    case B::Loop: return true;         // the last pad's loop (a kit); twice or held, the Looper
    case B::SampleRecord: return kit;  // Record; elsewhere it only says it wants a kit
    case B::Erase: // pads that clear, steps, chop markers, a pot with a parameter, or Copy on kit pads and Patterns
        return kit_pads || erase_pots || ui.page == PageId::Patterns || ui.page == PageId::Steps || ui.page == PageId::StepEdit || ui.page == PageId::Chop
            || ui.page == PageId::Motion || ui.copying != UiState::Copying::Off;
    default: return true; // the page buttons, Pad Bank, Mute, Note Repeat, 16 Levels, transport: everywhere
    }
}

bool button_has_shift_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads) {
    using B = SurfaceButton;
    if (ui.exit_open || ui.name.open() || ui.ask != UiState::Ask::None) return false; // the sheets keep their own lights
    if (ui.page == PageId::CardAccess || ui.card.state != 0) return false;             // SD card access: Shift does nothing
    bool kit = project.tracks[ui.track % track_count].engine == EngineKind::Sampler;
    switch (button) {
    case B::Sample: case B::Seq: case B::PadBank: case B::SampleSelect: case B::Mute: case B::TapTempo:
    case B::SeqRecord: case B::Play: case B::Stop: case B::Minus: case B::Plus: case B::WheelPush: case B::Shift:
        return true;
    case B::Loop: return kit;                                                    // reverse the last pad
    case B::Chop: return kit && project.tracks[ui.track % track_count].pads[project.tracks[ui.track % track_count].last_pad % sampler_pads].used(); // Note On
    case B::SixteenLevels: return project.tracks[ui.track % track_count].used(); // the 16 Levels type
    case B::TopLeft: case B::TopMiddle: // Loop Lock, Normalize on a kit's Stage TRIM; Extract, Split on Chop
        return kit && (ui.page == PageId::Chop || (ui.page == PageId::Stage && stage_page_now(ui, project.tracks[ui.track % track_count]).kind == StageKind::Trim));
    case B::TopRight: return ui.page == PageId::Chop && kit;                     // Merge
    case B::SampleRecord: return kit;                                            // Audio Recall
    case B::Erase: return kit_pads || ui.pad_bank || ui.page == PageId::Patterns || ui.copying != UiState::Copying::Off; // Copy (tracks with Pad Bank held)
    case B::KnobFx: case B::PadFx: return true;                                  // FX SELECT: Melodee's chains; Flex Beat
    default: return false; // Note Repeat: none printed
    }
}

} // namespace melodee
