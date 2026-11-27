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
// The bank of patterns in view (UiState::pattern_bank): "BANK B" beside the pattern grids.
ui::HeaderItem bank_pill(const Snapshot& s);
ui::HeaderItem edit_pattern_chip(const Snapshot& s); // "P20": the pattern edited; amber while another plays
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
// Round 3 (fader readout B): the fader moved in the last 1.5 s (not on the Fader page, which says it on the page),
// and the K card it moved (shell/fader_job.h fader_twin), or -1: none shows the value, or Shift is held.
bool fader_moved(const Snapshot& s);
int fader_card(const Snapshot& s);
// midi-track A (round 2): a MIDI track's route as Stage and Edit draw it in x, y, w, h: the track, its port and the
// instrument as glyphs on one line, notes travelling along it as they go out, the instrument's name, and the last
// notes sent as a roll underneath (four seconds, newest on the right). Port and channel are the cards'.
void midi_route(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h);
ui::SoftKeys sound_keys(const Snapshot& s); // Stage's B1-B3 for the track (pages/stage_page.cpp): Pads and Keys, Sample held
juce::Colour colour_of(const Snapshot& s);              // the selected track's colour
// A mixer channel's knob shade by strip number (shell/mixer_channels.h): a track's, a submix's own, a bus's (the
// Looper's); the input ice and the master white on all three.
juce::Colour channel_shade(unsigned strip, unsigned knob);
juce::Colour load_colour(float load);                   // a load's: green, amber from 60 %, red from 85 % (of a block, a core)
// A kit pad's name: its sample's, or for pads sharing one sample (a chop),
// the last word of it and which slice: "Break 5" in full, "Brk 5" on a pad.
juce::String pad_name(const Snapshot& s, unsigned kit, bool short_form);
// Stage on a kit pad holding an instrument (an SFZ, EXS, keygroup; round 3 "instrument C", pages/instrument_map.cpp):
// ZONES (Trim's place) the whole map; the envelopes, Tune and the filters a key strip over their own drawing; the
// rest the map.
enum class StageKind : uint8_t;
void instrument_picture(ui::Paint& p, const Snapshot& s, StageKind kind);
// The input source as Record and Input Config name it: "Rear", or a track's "Track 3 Prophet" (with_tap: "Track 3 Post").
juce::String input_name(const Snapshot& s, bool with_tap);
juce::String engine_word(EngineKind kind); // "Prophet", "FM6"
// The mic or the rear inputs while a loop or MIDI track has the ADC on the other one: silent, shown IN USE.
bool source_in_use(const Snapshot& s, InputSource source);
// Record: a take of Melodee's own sound runs, so the pads play the instrument (Context::record_keys()).
bool record_keys_shown(const Snapshot& s);

// The sixteen tracks as the pads sit, pad 1 bottom left, with the track in
// hand as a strip on the right, in the picture zone: the Tracks page, and the
// screen while Pad Bank is held.
void track_grid(ui::Paint& p, const Snapshot& s);
// An engine as a glyph: a saw for the Prophet, FM's operators, a chip for SID, pads for a kit, a DIN plug for MIDI.
ui::Icon engine_icon(EngineKind kind);
// Freeze plan F3: a region's take as a wave in x, y, w, h (its level, fades and direction in it), from the takes'
// overviews (Snapshot::audio_peaks); nothing while its take's overview is not in yet.
void region_wave(ui::Paint& p, const Snapshot& s, const AudioRegion& r, float x, float y, float w, float h, juce::Colour c);

// A sound, pad or track name as names are shown (words: capitals for labels, Title Case for names): a name stored
// in capitals ("BASS II") in Title Case ("Bass II"), roman numerals and acronyms as written.
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
void frame_keys(ui::Paint& p, const Snapshot& s, const ui::SoftKeys& keys); // B1-B3 alone (Pad Bank's and 16 Levels' held keys as frame() draws them)
// A plug-in over the cards (pages/plugin_view.cpp): its name and format, then loading, why it failed, or its row of
// parameters out of how many; Edit for a PLUGIN track, the chain pages for a plug-in effect.
void draw_plugin_view(ui::Paint& p, const Snapshot& s, unsigned entry, float top);
// A plug-in's page under B1-B3 (docs/PLUGIN_PAGES_PLAN.md, picks 3 c and 4 a): the page line, then its rows of three
// as arcs over the pots, an envelope or filter on top where the page holds one; no header, no cards. Shown while the
// plug-in is open and the pots are its own (not Knob FX's): plugin_page_shown().
bool plugin_page_shown(const Snapshot& s, unsigned entry);
void draw_plugin_page(ui::Paint& p, const Snapshot& s, unsigned entry, const ui::SoftKeys& keys);
// A plug-in's own picture (plugin/plugin_screen.h) at x, y: its page, or an effect's strip.
// plugin_screen_shown(): the page in view is a plug-in's own screen (Edit on its track, Stage's EDIT), which Melodee
// draws nothing over but its MENU, notices and questions.
bool plugin_screen_shown(const Snapshot& s, uint8_t area = 0); // area 1: an effect's strip under its chain's slots
void draw_screen_picture(ui::Paint& p, const ScreenPicture& picture, float x, float y);
// An effect plug-in in a chain (pages/fx_drawing.cpp): its page line and its sound as it plays, from x, y over w x h.
void draw_plugin_sound(ui::Paint& p, const Snapshot& s, float x, float y, float w, float h);
// An effect's skin strip (docs/PLUGIN_PAGES.md, Skins) under the chain row, at x, y (320 x 92), where it has one and
// Settings > SCREEN > Plug-in screens is on.
bool plugin_strip_shown(const Snapshot& s, unsigned entry);
void draw_plugin_strip(ui::Paint& p, const Snapshot& s, float x, float y);
juce::String plugin_name(const Snapshot& s, unsigned entry); // its own name once open, else its file's

// Defaults: pads play the selected track, Minus / Plus move its octave.
void play_pad(Context& c, unsigned pad, uint8_t velocity, bool down);
void octave(Context& c, int way);
void no_wheel(Context&, int);
void no_push(Context&);
void play_soft(Context& c, int key); // B1-B3 open Stage on that section (Pads, Keys)
// The pages under a soft key that steps through several (Stage's groups, the mixer's SENDS and FX, Seq's B1):
// their names into `names`, the open one's place into `at`; 0 for a key of one page or of none.
unsigned key_pages(const Snapshot& s, unsigned key, std::array<juce::String, 6>& names, unsigned& at);
ui::SoftKeys mix_keys(const Snapshot& s); // the MIX hub's row (shell/mix_hub.h): TRACKS SENDS FX, the page's lit
void mix_soft(Context& c, int key);       // and where its keys go
void seq_hub_soft(Context& c, int key); // the SEQUENCE hub's row: B1-B3 open SEQUENCES, ROLL, MOTION
const char* motion_key(const Snapshot& s); // B3's label there: MOTION, or none when Basic leaves it out (shell/features.h)

} // namespace melodee
