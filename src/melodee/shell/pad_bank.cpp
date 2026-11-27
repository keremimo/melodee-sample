// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/pad_bank.h"
#include "melodee/model/scales.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/track_copy.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/model/slice_banks.h"
#include <algorithm>

namespace melodee {
namespace {
void say_bank(Context& c) {
    const auto& t = c.track();
    unsigned bank = prog_pads(t) ? prog_bank(t) : t.bank % sampler_banks;
    c.notice(std::string("Bank ") + char('A' + bank));
}

// Seq held: the bank of patterns in view, A-H round, and what it holds.
void step_pattern_bank(Context& c, int way) {
    auto& ui = c.ui;
    ui.seq_used = true; // letting go of Seq then opens nothing
    int banks = int(pattern_banks);
    ui.pattern_bank = uint8_t((int(ui.pattern_bank % banks) + way % banks + banks) % banks);
    unsigned t = ui.track % track_count, with = 0;
    for (unsigned n = 0; n < pattern_pads; ++n) with += !c.patterns.at[t][ui.pad_pattern(n)].empty();
    unsigned first = ui.pattern_bank * pattern_pads + 1;
    c.notice(std::string("Patterns ") + char('A' + ui.pattern_bank) + ": P" + std::to_string(first) + "-" + std::to_string(first + 15) +
             (with ? "" : ", empty"));
}

// Chop: the next sixteen slices on the pads, round (A 1-16, B 17-32 ...), the bank's first slice in hand. The
// track's own bank stays as it was.
void step_slice_bank(Context& c, int way) {
    auto& ui = c.ui;
    const unsigned count = ui.slices.count;
    if (!count) { c.notice("No slices"); return; }
    const int banks = int(slice_banks(count)), was = int(slice_bank(ui.chop_slice));
    const int bank = (was + way % banks + banks) % banks;
    if (bank != was) { ui.chop_slice = uint8_t(bank * 16); ui.chop_fine = false; } // the wheel picks slices again
    const unsigned first = unsigned(bank) * 16 + 1, last = std::min(count, first + 15);
    c.notice(std::string("Bank ") + char('A' + bank) + ": slices " + std::to_string(first) + "-" + std::to_string(last));
}

// Roll on a synth: the pads an octave up or down, and the notes they now play.
void step_octave(Context& c, int way) {
    auto& t = c.track();
    t.octave = int8_t(std::clamp(t.octave + way, 0, 7));
    c.notice(std::string("Pads ") + note_name(pad_note(c.project, t, 0).key) + "–" + note_name(pad_note(c.project, t, 15).key));
}
} // namespace

void step_bank(Context& c, int way) {
    auto& t = c.track();
    int banks = drum_pads(t) ? int(drum_banks) : prog_pads(t) ? int(prog_banks) : int(sampler_banks); // Drums pads: A-D, MIDI's notes 36-99; Prog: A-C
    t.bank = uint8_t((int(t.bank % banks) + way % banks + banks) % banks);
}

void pad_bank_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) { ui.pad_bank = true; ui.pad_bank_used = false; ui.pad_bank_down = c.now; return; }
    if (!ui.pad_bank) return; // its press went elsewhere (a question took it): no step
    ui.pad_bank = false;
    if (!pad_bank_tap(ui, c.now)) return; // held (for a track, a jump, a copy, or just long): no step; only a tap steps
    if (ui.shift) ui.shift_used = true;
    if (ui.seq_held || ui.page == PageId::Patterns || ui.page == PageId::Scenes) { step_pattern_bank(c, ui.shift ? -1 : 1); return; } // the patterns' bank (Seq's layer)
    if (ui.page == PageId::Punch) { c.step_punch_bank(ui.shift ? -1 : 1); return; } // the next bank of punches
    if (ui.page == PageId::Mixer) { ui.mixer_bank = ui.mixer_bank ? 0 : 1; return; } // the tracks, or the submixes, buses, input and master
    if (ui.page == PageId::Chop) { step_slice_bank(c, ui.shift ? -1 : 1); return; } // the next sixteen slices
    if (sounds_open(ui)) { sounds_bank_step(c, ui.shift ? -1 : 1); return; } // Step Edit's sounds on the pads: their bank
    if (c.track().engine == EngineKind::Sampler || drum_pads(c.track()) || prog_pads(c.track())) { step_bank(c, ui.shift ? -1 : 1); say_bank(c); return; }
    if (ui.page == PageId::Roll) step_octave(c, ui.shift ? -1 : 1); // Roll on a synth: −/+ are its pitch
}

bool pad_bank_move(Context& c, int way) {
    auto& ui = c.ui;
    if (!ui.pad_bank || !way) return false;
    ui.pad_bank_used = true;
    if (ui.seq_held || ui.page == PageId::Patterns || ui.page == PageId::Scenes) step_pattern_bank(c, way);
    else if (ui.page == PageId::Punch) c.step_punch_bank(way);
    else if (ui.page == PageId::Mixer) ui.mixer_bank = way > 0 ? 1 : 0;
    else if (ui.page == PageId::Chop) step_slice_bank(c, way);
    else if (c.track().engine == EngineKind::Sampler || drum_pads(c.track()) || prog_pads(c.track())) { step_bank(c, way); say_bank(c); }
    else c.notice("Banks: kit, Drums and Prog tracks");
    return true;
}

bool track_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    if (!tracks_on_pads(ui)) return false;
    pad %= track_count;
    if (ui.pad_bank) ui.pad_bank_used = true;
    if (ui.page == PageId::Mixer) ui.mixer_bank = 0; // the mixer shows the track picked
    if (ui.erase && ui.copying != UiState::Copying::Tracks) { // Erase + pad: that track cleared, once B3 says so
        c.select_track(pad);
        if (ui.recording) c.notice("Clear track: not in a take");
        else if (!track_clearable(c.project.tracks[pad], c.patterns.at[pad])) c.notice("Track " + std::to_string(pad + 1) + " is empty");
        else ask(c, UiState::Ask::ClearTrack, int(pad));
        return true;
    }
    if (ui.copying != UiState::Copying::Tracks) { c.select_track(pad); offer_new_track(c); return true; }
    if (ui.copy_from < 0) { // the track to move or copy
        if (!track_clearable(c.project.tracks[pad], c.patterns.at[pad])) { c.notice("Track " + std::to_string(pad + 1) + " is empty"); return true; }
        ui.copy_from = int8_t(pad);
        c.select_track(pad);
        return true;
    }
    if (unsigned(ui.copy_from) == pad) return true; // then where it goes
    if (ui.moving) move_track(c, unsigned(ui.copy_from), pad); // the track in hand goes with it
    else copy_track(c, unsigned(ui.copy_from), pad);
    c.select_track(pad);
    ui.stop_copying();
    return true;
}

bool track_soft(Context& c, int key) {
    bool moving = c.ui.moving;
    auto did = track_key(c.ui, key);
    if (did == TrackKey::Ended) c.notice(moving ? "Move off" : "Copy off");
    if (did == TrackKey::Tracks && c.ui.page != PageId::Mixer) { c.ui.mixer_bank = 0; c.open(PageId::Mixer); } // B1 TRACKS: the mixer, on the tracks
    return did != TrackKey::Ignored;
}

} // namespace melodee
