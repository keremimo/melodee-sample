// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"

// An empty track picked with a pad (Tracks, Pad Bank held, Song's lanes) or
// opened with Push on Tracks: the New track sheet over the page, nothing else
// on it. The wheel moves from card to card, Push makes the track, B1 BACK
// leaves it empty. A kit or a MIDI track is made at once and the page stays;
// PLUGIN goes to the card's plug-ins and SAMPLES to the Samples browser, and
// the plug-in picked or the first sample loaded comes back to that page.
namespace melodee {

class Context;

enum class NewChoice : uint8_t { Plugin, Kit, Samples, Record, MidiOut, MidiUsb, MidiAllUsb, Count };
constexpr unsigned new_choice_count = unsigned(NewChoice::Count);

// The selected track empty: the sheet opens over the page in view.
void offer_new_track(Context& c);
// A press while the sheet is open: true when it was taken. Play, Stop and
// Pad Bank still do their jobs (Pad Bank + pad picks another track).
bool new_track_button(Context& c, SurfaceButton button);
void new_track_wheel(UiState& ui, int delta);
// What the card lit makes, on the selected track.
void make_new_track(Context& c, NewChoice choice);
// A sample loaded onto the track SAMPLES made: back to the page it was picked on.
void new_track_landed(Context& c);

} // namespace melodee
