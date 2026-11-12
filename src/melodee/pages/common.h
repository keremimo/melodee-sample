// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/effect_info.h"
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
// midi-track A (round 2): a MIDI track's route as Stage and Edit draw it in x, y, w, h: the track, its port and the
// instrument as glyphs on one line, notes travelling along it as they go out, the instrument's name, and the last
// notes sent as a roll underneath (four seconds, newest on the right). Port and channel are the cards'.
void midi_route(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h);
ui::SoftKeys play_keys(const Snapshot& s, int active); // STAGE TRACKS KEYS (PADS on a kit)
juce::Colour colour_of(const Snapshot& s);              // the selected track's colour
juce::Colour load_colour(float load);                   // a load's: green, amber from 60 %, red from 85 % (of a block, a core)
// A kit pad's name: its sample's, or for pads sharing one sample (a chop),
// the last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form);
void instrument_picture(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h); // an SFZ on the pad: its keys and velocities
// The input source as Record and Input Config name it: "Rear", or a track's "Track 3 Prophet" (with_tap: "Track 3 Post").
juce::String input_name(const Snapshot& s, bool with_tap);
juce::String engine_word(EngineKind kind); // "Prophet", "FM6"
// Record: a take of Melodee's own sound runs, so the pads play the instrument (Context::record_keys()).
bool record_keys_shown(const Snapshot& s);

// The sixteen tracks as the pads sit, pad 1 bottom left, with the track in
// hand as a strip on the right, in the picture zone: the Tracks page, and the
// screen while Pad Bank is held.
void track_grid(ui::Paint& p, const Snapshot& s);
// An engine as a glyph: a saw for the Prophet, FM's operators, a chip for SID, pads for a kit, a DIN plug for MIDI.
ui::Icon engine_icon(EngineKind kind);

// A sound, pad or track name as names are shown (words: capitals for labels, Title Case for names): a name stored
// in capitals ("FUNK BASS II") in Title Case ("Funk Bass II"), roman numerals and acronyms as written.
juce::String name_case(const juce::String& name);
// A name in capitals ("BEAT REPEAT") for a narrow cell: as it is when it
// fits `w` in `style`, else in title case ("Beat Repeat"), which is narrower.
juce::String cell_name(const ui::Paint& p, const juce::String& name, const ui::Paint::Text& style, float w);
// A grid of such cells shares one case: capitals when every name fits `w` in `style`, else Title Case for all of
// them, so one grid never mixes "SHUFFLE" with "Pitch Down". Then cell_name(..., capitals) for each cell.
bool cells_in_capitals(const ui::Paint& p, const juce::StringArray& names, const ui::Paint::Text& style, float w);
juce::String cell_name(const juce::String& name, bool capitals);

// Effects (pages/fx_drawing.cpp): a family as its glyph; the six-slot strip of a chain (each slot its family's
// glyph and name, the one in hand lit, empty ones dots and the next a plus), Stage's FX and FX Select's alike;
// the effect in hand under it, its name and family and what it does to sound drawn from its own values: a tail
// for space, taps for time, a wave for motion, a curve for dirt, a response for tone, the gain curve for dynamics.
ui::Icon family_icon(Family f);
void fx_strip(ui::Paint& p, const Chain& chain, unsigned cursor, juce::Colour c, float y);
void fx_in_hand(ui::Paint& p, const Snapshot& s, const Chain& chain, unsigned cursor, juce::Colour c, float y, float bottom);
void effect_drawing(ui::Paint& p, const EffectSlot& slot, float x, float y, float w, float h, juce::Colour c);

// Draws soft keys, header and knob strip; the page draws the picture.
void frame(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys, const ui::Header& header);
// A plug-in over the cards (pages/plugin_view.cpp): its name and format, then loading, why it failed, or its row of
// parameters out of how many; Edit for a PLUGIN track, the chain pages for a plug-in effect.
void draw_plugin_view(ui::Paint& p, const Snapshot& s, unsigned entry, float top);
juce::String plugin_name(const Snapshot& s, unsigned entry); // its own name once open, else its file's

// Defaults: pads play the selected track, Minus / Plus move its octave.
void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down);
void octave(Context& c, int way);
void no_wheel(Context&, int);
void no_push(Context&);
void play_soft(Context& c, int key); // B1-B3 open Stage, Tracks, Keys (Pads)

} // namespace melodee
