// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/ui_state.h"

// What a pad press does now, in the order Shell::hit() dispatches it, and so
// what the pads light as (shell/pad_lights.cpp). One chooser for both: a
// layer cannot get a job without a light. A release goes where its press
// went (Shell::hit). MIDI thread.
namespace melodee {

enum class PadContext : uint8_t {
    Sheet,        // a question or the Exit sheet over the page: the pads do nothing
    Tracks,       // Pad Bank held, or a track copy: a pad picks a track (shell/pad_bank.h)
    Name,         // the Name sheet: the pads type (shell/name_entry.h)
    LoopHeld,     // Loop held off the Looper: the loop grid (shell/looper_jobs.h)
    PatternCopy,  // a pattern copy waits: the pattern, then where (shell/seq_layer.h)
    SeqLayer,     // Seq held: the selected track's patterns
    RecordKeys,   // a take of Melodee's own sound: the instrument it records
    Mute,         // mute mode: tracks or kit pads (the Looper: its loops); Shift solos
    SamplePick,   // Sample held on a kit: the pad becomes the last pad, unheard
    LoopNow,      // Shift + pads 9-12 on the Looper: play or stop at once
    Shift,        // the printed Shift + pad jobs (on Samples, the next empty pad)
    ResamplePick, // Resample: the pad that gets the render
    PadCopy,      // Shift + Erase on a kit: the pad to copy, then where
    KeyCopy,      // Shift + Erase on a synth's play page, Roll or Step Edit: the key whose notes to copy, then where
    StepCopy,     // Shift + Erase on Steps: the step to copy, then where
    PadErase,     // Erase held on a kit: the pad, or its notes while playing
    KeyErase,     // Erase held where the pads play a synth's keys: that key's notes, asked first
    Page,         // the page's own job (pages/page.h pad_job)
};

struct PadFacts {
    bool card = false;        // SD card access holds the controls: the pads only play (Shell::pad)
    bool record_keys = false; // Context::record_keys(): a take of Melodee's own sound runs
    bool kit_pads = false;    // the page's pads play the selected kit (Shell::kit_pads)
    bool key_pads = false;    // the page's pads play a synth's (or MIDI or plug-in track's) keys (Shell::key_pads)
};

inline PadContext pad_context(const UiState& ui, const Track& track, const PadFacts& facts, unsigned pad) {
    using C = PadContext;
    if (facts.card) return C::Page;
    if (ui.exit_open || ui.ask != UiState::Ask::None) return C::Sheet;
    if (tracks_on_pads(ui)) return C::Tracks;
    if (ui.name.open()) return C::Name;
    if (ui.loop_held && ui.page != PageId::Looper) return C::LoopHeld;
    if (ui.copying == UiState::Copying::Patterns) return C::PatternCopy;
    if (ui.seq_held) return C::SeqLayer;
    if (facts.record_keys && !ui.shift) return C::RecordKeys; // Shift + pad jobs stay
    if (ui.mute_mode()) return C::Mute;
    if (ui.sample_held && track.engine == EngineKind::Sampler && !track.levels) return C::SamplePick;
    if (ui.shift && ui.page == PageId::Looper && pad % 16 >= 8 && pad % 16 < 12) return C::LoopNow;
    if (ui.shift) return C::Shift;
    if (ui.resample_pick) return C::ResamplePick;
    if (facts.kit_pads && ui.copying == UiState::Copying::Pads) return C::PadCopy;
    if (ui.copying == UiState::Copying::Keys && (facts.key_pads || ui.page == PageId::Roll || ui.page == PageId::StepEdit))
        return C::KeyCopy;
    if (ui.copying == UiState::Copying::Steps && ui.page == PageId::Steps) return C::StepCopy;
    if (facts.kit_pads && ui.erase) return C::PadErase;
    if (facts.key_pads && ui.erase) return C::KeyErase;
    return C::Page;
}

} // namespace melodee
