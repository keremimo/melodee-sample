// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/features.h"
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/stage_map.h"

namespace melodee {

bool button_has_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads, bool erase_pots, bool key_pads) {
    using B = SurfaceButton;
    if (ui.exit_open) return false; // only B1, B2 and B3 answer, and they have no lights
    if (ui.name.open()) return button == B::Erase || button == B::Play || button == B::Stop; // the Name sheet
    if (ui.page == PageId::CardAccess || ui.card.state != 0) { // SD card access: everything else waits (shell/card_access.h)
        if (ui.ask != UiState::Ask::None) return button == B::TopLeft || button == B::TopRight || button == B::Play || button == B::Stop;
        return button == B::Play || button == B::Stop || (button == B::TopLeft && (ui.card.state == 0 || ui.card.state == 2));
    }
    if (ui.ask != UiState::Ask::None) // a question: B1 Cancel, B3 Do it, and the transport
        return button == B::TopLeft || button == B::TopRight || button == B::Play || button == B::Stop;
    const auto& track = project.tracks[ui.track % track_count];
    bool kit = track.engine == EngineKind::Sampler;
    if (ui.page == PageId::Song && ui.jam_sheet && (button == B::Chop || button == B::Erase || button == B::NoteRepeat || button == B::Loop))
        return button == B::Erase && ui.jam_count; // JAMS: Erase deletes the picked jam
    if (ui.page == PageId::Song && !ui.song_export && (button == B::Chop || button == B::Erase || button == B::NoteRepeat)) { // the clip under the cursor
        int at = project.song.clip_at(ui.track % track_count, ui.song_cursor);
        if (at < 0) return button == B::Erase && ui.copying != UiState::Copying::Off;
        const auto& clip = project.song.clips[size_t(at)];
        if (button == B::Chop) return ui.song_cursor > clip.bar;
        if (button == B::NoteRepeat) return project.song.next_marker(clip.bar) > clip.end();
        return true;
    }
    switch (button) {
    case B::Chop: return kit && track.pads[track.last_pad % sampler_pads].used(); // the last pad's sample
    case B::Loop: return true;         // the last pad's loop (a kit); held, the loop grid; with 16 Levels, the Looper
    case B::SampleRecord: return kit;  // Record; elsewhere it only says it wants a kit
    case B::Erase: // pads or keys that clear, patterns with Seq held, a pot with a parameter, the fader's lane, the thing
                   // in focus (shell/erase_copy.h), or Copy
        return kit_pads || key_pads || erase_pots || ui.seq_held || ui.pad_bank || erase_tap_page(ui) || ui.page == PageId::Motion
            || ui.copying != UiState::Copying::Off;
    default: return true; // the page buttons, Pad Bank, Mute, Note Repeat, 16 Levels, transport: everywhere
    }
}

bool button_has_shift_job(const Project& project, const UiState& ui, SurfaceButton button, bool kit_pads, bool key_pads) {
    using B = SurfaceButton;
    if (ui.exit_open || ui.name.open() || ui.ask != UiState::Ask::None) return false; // the sheets keep their own lights
    if (ui.page == PageId::CardAccess || ui.card.state != 0) return false;             // SD card access: Shift does nothing
    bool kit = project.tracks[ui.track % track_count].engine == EngineKind::Sampler;
    { // a PLUGIN track's B1 pages on Stage: Shift + B1 PLUG-INS, B2 PRESETS (when it has some), B3 TRANSPORT
        const auto& tr = project.tracks[ui.track % track_count];
        if (ui.page == PageId::Stage && tr.engine == EngineKind::Plugin && stage_plugin_page(stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind)
            && (button == B::TopLeft || button == B::TopMiddle || button == B::TopRight)) {
            const bool chosen = tr.plugin < max_plugins;
            if (button == B::TopMiddle) return chosen && ui.plugin_panel.entry == tr.plugin && ui.plugin_panel.programs > 0;
            return button == B::TopLeft || chosen;
        }
    }
    switch (button) {
    case B::Sample: case B::Seq: case B::PadBank: case B::SampleSelect: case B::Mute: case B::TapTempo:
    case B::SeqRecord: case B::Play: case B::Stop: case B::Minus: case B::Plus: case B::WheelPush: case B::Shift:
        return true;
    case B::Loop: return kit;                                                    // reverse the last pad
    case B::Chop: return kit && project.tracks[ui.track % track_count].pads[project.tracks[ui.track % track_count].last_pad % sampler_pads].used(); // Note On
    case B::SixteenLevels: return project.tracks[ui.track % track_count].used(); // the 16 Levels type
    case B::TopLeft: case B::TopMiddle: { // Loop Lock, Normalize on a kit's Stage TRIM; Extract, Split on Chop; LOAD, CLEAR on LAYERS; Patterns', Step Edit's
        if (ui.page == PageId::Patterns || ui.page == PageId::StepEdit) return true; // SCENE, FOLLOW; PADS, TIE
        if (!kit) return false;
        if (ui.page == PageId::Chop || ui.page == PageId::LayersPage) return true;
        const auto& tr = project.tracks[ui.track % track_count];
        const bool instrument = stage_instrument(project, tr);
        auto kind = ui.page == PageId::Stage ? stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind : StageKind::Params;
        if (kind == StageKind::Trim && instrument) return false; // ZONES (round 3)
        return kind == StageKind::Trim || kind == StageKind::Layers;
    }
    case B::TopRight:
        if (kit && ui.page == PageId::Stage) { // STEMS on a kit pad's TRIM, where the pad holds a sample
            const auto& tr = project.tracks[ui.track % track_count];
            if (stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind == StageKind::Trim && !stage_instrument(project, tr)
                && tr.pads[tr.last_pad % sampler_pads].used()) return true;
        }
        return (ui.page == PageId::Chop && kit) || ui.page == PageId::Punch || ui.page == PageId::Patterns || ui.page == PageId::Scenes || ui.page == PageId::StepEdit || ui.page == PageId::ModPage || ui.page == PageId::Motion; // Merge; QUANTIZE on Pad FX; VARY on Patterns; ALL on Scenes; INSERT on Step Edit; ADD on MOD and MACRO; DRAW on Motion
    case B::SampleRecord: return kit;                                            // Audio Recall
    case B::Erase: // Copy: what the page holds (shell/erase_copy.h), or off
        return ui.copying != UiState::Copying::Off || copy_here(project, ui, kit_pads, key_pads) != CopyHere::None;
    case B::KnobFx: case B::PadFx: return true;                                  // FX SELECT: Melodee's chains; Flex Beat
    default: return false; // Note Repeat: none printed
    }
}

} // namespace melodee
