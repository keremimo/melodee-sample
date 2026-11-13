// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// Pad Bank (MELODEE_PARITY.md, phase 1c). A tap steps the selected kit to
// its next bank, A-H round (Shift: the previous one; on Punch the other bank
// of punches); with Seq held, the bank of patterns the pads launch. Held: a pad picks that track, Minus / Plus or the wheel jump
// banks, and after a second the screen shows the sixteen tracks (the pads are
// the tracks at once); B1-B3 open the MIX hub (TRACKS, SENDS, FX); Shift + B2 MOVE moves a whole track to
// another pad, Shift + B3 COPY (or Shift + Erase, as printed)
// copies one, Erase + pad clears one (shell/track_copy.cpp). One job a tap, acted on release: no
// double tap. MIDI thread.
namespace melodee {

class Context;
constexpr uint32_t pad_bank_overlay_ms = 1000; // held this long: the tracks over the page (a tap shows nothing)

void pad_bank_button(Context& c, bool pressed);
// Minus / Plus or the wheel while Pad Bank is held: the bank jumps. False when it is not held.
bool pad_bank_move(Context& c, int way);
// The selected kit's bank, `way` banks on (round A-H).
void step_bank(Context& c, int way);
// Whether the pads are the sixteen tracks now: Pad Bank held, or a track copy under way.
inline bool tracks_on_pads(const UiState& ui) { return ui.pad_bank || ui.copying == UiState::Copying::Tracks; }
// Whether the screen shows the tracks over the page: a track copy, or Pad Bank held a second.
inline bool track_overlay_shown(const UiState& ui, uint32_t now) {
    return ui.copying == UiState::Copying::Tracks || (ui.pad_bank && now - ui.pad_bank_down >= pad_bank_overlay_ms);
}
// A pad while tracks_on_pads(): picks that track, or the move's or copy's track and
// then where it goes; with Erase held, asks to clear it. False when the pads are not the tracks.
bool track_pad(Context& c, unsigned pad);

// A track move or copy waits for its two pads: the track, then where it goes (B2 MOVE, B3 COPY).
inline void start_track_pick(UiState& ui, bool move) {
    ui.stop_copying(); // a pad or pattern copy gives way
    ui.copying = UiState::Copying::Tracks;
    ui.moving = move;
}
// B1-B3 while the pads are the tracks (Pad Bank held, or a move or copy picking its tracks): Shift + B2 MOVE and
// Shift + B3 COPY start one (B2 / B3 alone while one is under way switch it, its track kept, and again end it); B1
// steps back, from where to the track, then out. Nothing: Pad Bank held alone, B1-B3 open the MIX hub's TRACKS,
// SENDS, FX (shell/mix_hub.h). Ignored: the pads are not the tracks, the page's keys answer.
enum class TrackKey : uint8_t { Ignored, Nothing, Started, Switched, Back, Ended };
inline TrackKey track_key(UiState& ui, int key) {
    if (!tracks_on_pads(ui)) return TrackKey::Ignored;
    if (ui.pad_bank) ui.pad_bank_used = true; // its release steps no bank
    bool picking = ui.copying == UiState::Copying::Tracks;
    if (key == 0) {
        if (!picking) return TrackKey::Nothing;
        if (ui.copy_from >= 0) { ui.copy_from = -1; return TrackKey::Back; }
        ui.stop_copying();
        return TrackKey::Ended;
    }
    if (!picking && !ui.shift) return TrackKey::Nothing; // Pad Bank held alone: B1-B3 the MIX hub's (track_soft())
    bool move = key == 1;
    if (picking && ui.moving == move) { ui.stop_copying(); return TrackKey::Ended; }
    if (picking) { ui.moving = move; return TrackKey::Switched; }
    start_track_pick(ui, move);
    return TrackKey::Started;
}
// B1-B3 through track_key(), with a notice when the move or copy ends. False when the page's keys answer.
bool track_soft(Context& c, int key);

} // namespace melodee
