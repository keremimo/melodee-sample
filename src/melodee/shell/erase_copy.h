// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/ui_state.h"

// Erase and Copy (Shift + Erase) on whatever the page in view holds.
//
// Erase tapped on its own (nothing else pressed or turned while it was
// down) erases the thing in focus: Roll's note, Steps' step under the
// cursor, Step Edit's event, the pattern on Patterns, the track on Tracks,
// the scene on Scenes, the loop on the Looper, the effect in a chain, the
// slice on Chop, the layer on LAYERS, the target on MOD, the project on
// Project (Delete: tap again). Song keeps its own (the clip, on the press).
// What can be lost asks first, as Erase + pad does.
//
// Shift + Erase copies what the page holds. Pad Bank held: tracks; Seq
// held: patterns. Then by page: Tracks the tracks, Patterns the patterns,
// Steps a step, Roll and Step Edit a key's notes, Song the clip (after
// itself), Project the project, a chain the effect (after itself), Chop the
// slice (onto the next empty pad). Else a kit's pads, or a synth's keys.
namespace melodee {

class Context;

// What Shift + Erase copies here. The first five arm a copy (the source, then
// where); the rest copy at once.
enum class CopyHere : uint8_t { None, Pads, Patterns, Tracks, Keys, Steps, Clip, Project, Effect, Slice };

// `kit_pads`: the pads play the selected kit (Shell::kit_pads); `key_pads`: they play a synth's keys (Shell::key_pads).
inline CopyHere copy_here(const Project& project, const UiState& ui, bool kit_pads, bool key_pads) {
    if (ui.pad_bank) return CopyHere::Tracks;
    if (ui.seq_held) return CopyHere::Patterns;
    const auto& t = project.tracks[ui.track % track_count];
    switch (ui.page) {
    case PageId::Tracks: return CopyHere::Tracks;
    case PageId::Patterns: return CopyHere::Patterns;
    case PageId::Steps: return t.used() ? CopyHere::Steps : CopyHere::None;
    case PageId::Roll: case PageId::StepEdit: return t.used() ? CopyHere::Keys : CopyHere::None;
    case PageId::Song:
        if (ui.song_export) return CopyHere::None;
        return project.song.clip_at(ui.track % track_count, ui.song_cursor) >= 0 ? CopyHere::Clip : CopyHere::None;
    case PageId::Project: return ui.project_list && !ui.move_open ? CopyHere::Project : CopyHere::None;
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: {
        if (ui.picker_open) return CopyHere::None;
        unsigned n = page_chain(project, ui);
        if (ui.page == PageId::PadFx && t.engine != EngineKind::Sampler) return CopyHere::None;
        return ui.fx_cursor[UiState::cursor_of(n)] < project.chain(n).count() ? CopyHere::Effect : CopyHere::None;
    }
    case PageId::Chop: return t.engine == EngineKind::Sampler && ui.slices.count ? CopyHere::Slice : CopyHere::None;
    default: break;
    }
    if (kit_pads) return CopyHere::Pads;
    if (key_pads) return CopyHere::Keys;
    return CopyHere::None;
}

// Whether Erase tapped on its own has a job on the page in view (its light; the tap itself checks there is
// something to erase and says so when there is not).
inline bool erase_tap_page(const UiState& ui) {
    switch (ui.page) {
    case PageId::Roll: case PageId::Steps: case PageId::StepEdit: case PageId::Patterns: case PageId::Tracks:
    case PageId::Scenes: case PageId::Looper: case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx:
    case PageId::Chop: case PageId::LayersPage: case PageId::ModPage:
        return true;
    case PageId::Project: return ui.project_list && !ui.move_open;
    default: return false;
    }
}

// Erase let go on its own: the thing in focus erased, asked first where it says so. False when the page has none.
bool erase_tap(Context& c);
// Shift + Erase with no copy under way: a copy armed, or done at once (copy_here()).
void copy_press(Context& c, bool kit_pads, bool key_pads);
// A pad while a key copy waits (Copying::Keys): the key to copy, then where.
void key_copy_pad(Context& c, unsigned pad);
// A pad on Steps while a step copy waits (Copying::Steps): the step to copy, then where.
void step_copy_pad(Context& c, unsigned pad);
// Erase + pad where the pads play a synth's keys: that key's notes, once B3 says so.
void key_erase_pad(Context& c, unsigned pad);

} // namespace melodee
