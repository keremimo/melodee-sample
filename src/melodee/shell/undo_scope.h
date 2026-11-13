// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/print.h"
#include "melodee/model/project.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/history.h"
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <memory>
#include <string>

// Undo for everything a pot or a page's buttons change (parity phase 10):
// what one step covers. A pot's turns are one step per control, closed by
// another control, a button or a pad, or two seconds still (the gesture, in
// Context); a button or pad press on a page is one step of what that page
// edits (PageEdit, around the shell's press), unless the press kept an edit
// of its own. The song, the mix, a track's or a bus's or the master's chain,
// a kit pad, a track's sound, the master's performance effects, Chop's
// slices, and the selected track's pattern on Roll, Steps and Step Edit
// (Roll R0: a pad there is one step from press to release, so what is
// done while it is held, by the wheel, a pot or the fader, goes with it).
// Pure: which scope, its name; Context takes and restores them.
namespace melodee {

class Context;

struct UndoScope {
    enum class Kind : uint8_t { None, Pad, Mix, Chain, Master, Sound, Song, Chop, Pattern, Tempo };
    Kind kind = Kind::None;
    uint8_t track = 0;
    uint16_t index = 0; // Pad: the kit pad (0..127); Chain: the chain's number; Pattern: the track's pattern
    bool none() const { return kind == Kind::None; }
    bool operator==(const UndoScope& o) const { return kind == o.kind && track == o.track && index == o.index; }
    bool operator!=(const UndoScope& o) const { return !(*this == o); }
};

// What a card's value is part of; None for what is not kept (the click level, patterns' length, a page's cursor ...).
// The note cards (Steps and Roll's length, velocity, chance; Step Edit's event length and velocity): the pattern.
UndoScope binding_scope(const Project& project, const UiState& ui, const Binding& binding);
// What a press of `button` (or a pad, `pad` true) changes on the page in view: Loop, Shift + Loop and
// Shift + Chop the last kit pad on any page; else by page (Song the song, Mix and Sends the mix, the chain
// pages their chain, Knob FX / Compressor / Flex Beat / Punch the master's effects, Chop its slices, the
// sampler pages and Pads the last pad, Edit the track's sound, Roll / Steps / Step Edit the pattern).
UndoScope press_scope(const Project& project, const UiState& ui, SurfaceButton button, bool pad);
// The selected track's pattern playing now.
UndoScope pattern_scope(const Project& project, const UiState& ui);
// The edit's name, shown on undo: "A01", "Mix", "T3 FX", "Bus A FX", "Master FX", "Knob FX", "T2 sound", "Song", "Chop",
// "T1 P3", "Tempo".
std::string scope_name(const Project& project, const UiState& ui, const UndoScope& scope);
// Freeze plan F2c: whether a change in `scope` changes what a frozen track's prints are made of: its sound, a kit pad,
// a frozen pattern, its chain (Post FX prints) or a kit pad's effects. The mix, the master and the song stay live.
bool print_covers(const std::array<TrackFreeze, track_count>& freeze, const UndoScope& scope);
// What a card's value is part of for a freeze: binding_scope(), and the pattern's values no undo keeps (its length, Q,
// swing), the track's speed, Stage's sound, a plug-in's values, the arp and chords, which a print holds too.
UndoScope freeze_scope(const Project& project, const UiState& ui, const Binding& binding);
// The states a scope compares and restores (shell/history.h).
MixState mix_of(const Project& project);
SoundState sound_of(const Track& track);
MasterState master_of(const Project& project);
ChopState chop_of(const UiState& ui);
TempoState tempo_of(const Project& project);
constexpr uint32_t gesture_quiet_ms = 2000; // a control left this long: its next turn is a new step

// A press on a page as one undo step: made before the press, finish() after it.
class PageEdit {
public:
    PageEdit(Context& c, SurfaceButton button, bool pad);
    void finish();
private:
    Context& c;
    std::unique_ptr<Edit> edit;
    unsigned pushes = 0;
};

} // namespace melodee
