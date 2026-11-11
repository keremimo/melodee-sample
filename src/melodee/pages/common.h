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
// A card's label, value and unit for one Binding (also the fader's card).
void describe_card(const Snapshot& s, const struct Binding& b, ui::KnobCard& c);
// What the fader does here (shell/fader_job.h), for the screen: "A01 VOLUME",
// "LEVEL", "NUDGE", "OFF"; its value as a card (label set to that), and
// whether it still has to reach the value (takeover) and where it sits.
struct FaderShown {
    juce::String what;
    ui::KnobCard card;
    bool value = false;   // there is a value (not off, a nudge or an empty pad)
    bool off = false;     // the track's target is off and the page has no job for it
    bool waiting = false; // takeover: the fader has not met the value yet
};
FaderShown fader_shown(const Snapshot& s);
ui::SoftKeys play_keys(const Snapshot& s, int active); // STAGE KEYS MIX
juce::Colour colour_of(const Snapshot& s);              // the selected track's colour
// A kit pad's name: its sample's, or for pads sharing one sample (a chop),
// the last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form);
void instrument_picture(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h); // an SFZ on the pad: its keys and velocities
// The input source as Record and Input Config name it: "Rear", or a track's "Track 3 Prophet" (with_tap: "Track 3 Post").
juce::String input_name(const Snapshot& s, bool with_tap);
juce::String engine_word(EngineKind kind); // "Prophet", "FM6"
// Record: a take of Melodee's own sound runs, so the pads play the instrument (Context::record_keys()).
bool record_keys_shown(const Snapshot& s);

// The tracks as the pads sit, pad 1 bottom left, in the picture zone, eight
// at a time (the half with the selected track): Stage's picture, and the
// screen while Pad Bank is held. The pill names the half in the header.
void track_grid(ui::Paint& p, const Snapshot& s);
void track_grid(ui::Paint& p, const Snapshot& s, unsigned half); // the Tracks screen: the half its B1 / B2 chose
unsigned track_half(const Snapshot& s);
ui::HeaderItem track_half_pill(const Snapshot& s);

// A name in capitals ("BEAT REPEAT") for a narrow cell: as it is when it
// fits `w` in `style`, else in title case ("Beat Repeat"), which is narrower.
juce::String cell_name(const ui::Paint& p, const juce::String& name, const ui::Paint::Text& style, float w);

// Draws soft keys, header and knob strip; the page draws the picture.
void frame(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys, const ui::Header& header);
// A plug-in over the cards (pages/plugin_view.cpp): its name and format, then loading, why it failed, or its row of
// parameters out of how many; Edit for a PLUGIN track, the chain pages for a plug-in effect.
void draw_plugin_view(ui::Paint& p, const Snapshot& s, unsigned entry, juce::Colour c, float top);
juce::String plugin_name(const Snapshot& s, unsigned entry); // its own name once open, else its file's

// Defaults: pads play the selected track, Minus / Plus move its octave.
void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down);
void octave(Context& c, int way);
void no_wheel(Context&, int);
void no_push(Context&);
void play_soft(Context& c, int key); // B1-B3 open Stage, Keys, Mix

} // namespace melodee
