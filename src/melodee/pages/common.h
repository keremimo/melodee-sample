// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/pages/page.h"
#include "melodee/ui/chrome.h"
#include "melodee/ui/knobs.h"
#include <array>

// Pieces most pages share: the standard header, the track tag, the knob
// cards built from the page's Bindings, the Play group's soft keys, and the
// default control behaviours.
namespace melodee {

ui::Header header_of(const Snapshot& s);
ui::HeaderItem tag_of(const Snapshot& s, unsigned track);
std::array<ui::KnobCard, 3> cards_of(const Snapshot& s);
ui::SoftKeys play_keys(const Snapshot& s, int active); // STAGE KEYS MIX
juce::Colour colour_of(const Snapshot& s);              // the selected track's colour
// A kit pad's name: its sample's, or for pads sharing one sample (a chop),
// the last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form);

// The sixteen tracks as the pads sit, pad 1 bottom left, in the picture
// zone: Stage's picture, and the screen while Pad Bank is held.
void track_grid(ui::Paint& p, const Snapshot& s);

// Draws soft keys, header and knob strip; the page draws the picture.
void frame(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys, const ui::Header& header);

// Defaults: pads play the selected track, Minus / Plus move its octave.
void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down);
void octave(Context& c, int way);
void no_wheel(Context&, int);
void no_push(Context&);
void play_soft(Context& c, int key); // B1-B3 open Stage, Keys, Mix

} // namespace melodee
