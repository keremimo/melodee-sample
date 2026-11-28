// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/features.h"
#include "melodee/shell/lesson.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <algorithm>
#include <array>

// The MENU: the encoder held menu_hold_ms opens it over any page. It has two halves on B2 and B3. GO's pad 16 is
// LEARN: the lessons as cards (the wheel moves, Push or B3 starts one, B1 goes back to GO). While a lesson runs the
// MENU opens on GO with that one pad, LEAVE (shell/lesson.h).
// HERE lists what the page can do that no printed key says, as cards, each with its shortcut under its name (the
// wheel moves, Push does the lit card). GO shows the sixteen places laid out as the pads (a pad opens its place;
// the wheel and Push work too). B1 goes back. A short push of the encoder keeps the page's own job: it acts when
// the encoder is let go, since a hold now means MENU. The rules here are pure, for the shell, the screen and the
// tests; the jobs themselves are in shell/menu.cpp.
namespace melodee {

class Context;

constexpr uint32_t menu_hold_ms = 300;
enum MenuView : uint8_t { menu_closed = 0, menu_here_view = 1, menu_go_view = 2, menu_learn_view = 3 }; // UiState::menu

// The encoder's push: a tap or a hold.
struct EncoderHold {
    bool down = false, fired = false;
    uint32_t at = 0;
};
inline void encoder_press(EncoderHold& e, uint32_t now) { e = EncoderHold {true, false, now}; }
// Each pass while it is down: true once, when it has been held long enough (the MENU opens).
inline bool encoder_tick(EncoderHold& e, uint32_t now) {
    if (!e.down || e.fired || now - e.at < menu_hold_ms) return false;
    e.fired = true;
    return true;
}
// Let go: true when it was a tap (the page's own Push).
inline bool encoder_release(EncoderHold& e) {
    bool tap = e.down && !e.fired;
    e.down = false;
    return tap;
}

// GO: the sixteen places, pad 1 first. `group`: 0 sound, 1 sequence, 2 mix, 3 project.
struct MenuPlace { const char* name; const char* key; uint8_t group; PageId page; };
inline const std::array<MenuPlace, 16>& menu_places() {
    static const std::array<MenuPlace, 16> places {{
        {"STAGE", "SAMPLE", 0, PageId::Stage}, {"BROWSER", "SAMPLE SELECT", 0, PageId::Samples},
        {"RECORD", "SAMPLE RECORD", 0, PageId::Record}, {"CHOP", "CHOP", 0, PageId::Chop},
        {"SEQUENCE", "SEQ", 1, PageId::Scenes}, {"ROLL", "SEQ, B2", 1, PageId::Roll}, // SEQUENCES is wider than a tile's label at the text floor
        {"STEP EDIT", "SHIFT + SEQ", 1, PageId::StepEdit}, {"MOTION", "SEQ, B3", 1, PageId::Motion},
        {"MIXER", "PAD FX + KNOB FX", 2, PageId::Mixer}, {"", "", 2, PageId::Sends}, // pad 10: free since the mixer (step 6), never shown
        {"EFFECTS", "SHIFT + KNOB FX", 2, PageId::Rack}, {"LOOPER", "LOOP + 16 LEVELS", 2, PageId::Looper},
        {"SONG", "SHIFT + PAD 12", 3, PageId::Song}, {"PROJECT", "SHIFT + PAD 16", 3, PageId::Project},
        {"SETTINGS", "SHIFT + PAD 8", 3, PageId::SettingsPage}, {"LEARN", "", 3, PageId::Count}, // LEARN: the lessons (shell/lesson.h); in one, LEAVE
    }};
    return places;
}
// The tile the cursor starts on: the open page's place, else STAGE.
inline unsigned menu_tile_of(PageId page) {
    if (page == PageId::Patterns) return 4;                                                  // Sequences' second page
    if (page == PageId::ModPage) return 7;                                                   // Motion's next pages
    if (page == PageId::Bus || page == PageId::Master || page == PageId::PadFx) return 10;   // the other chains
    if (page == PageId::Sounds || page == PageId::Plugins) return 1;                         // the other browsers
    const auto& places = menu_places();
    for (unsigned n = 0; n < places.size(); ++n) if (places[n].page == page) return n;
    return 0;
}
// The wheel on GO: across a row, then down to the next, as the tiles are drawn (pads 13-16 on top). Clamped.
inline unsigned menu_tile_turn(unsigned tile, int delta) {
    int at = int(3 - (tile % 16) / 4) * 4 + int(tile % 4); // its place in reading order
    at = std::clamp(at + delta, 0, 15);
    return unsigned(3 - at / 4) * 4 + unsigned(at % 4);
}
// The same past the tiles Basic leaves out (shell/features.h): a step at a time, staying put when none is left that way.
inline unsigned menu_tile_turn(const UiState& ui, unsigned tile, int delta) {
    unsigned at = tile % 16;
    for (int left = delta < 0 ? -delta : delta; left > 0; --left) {
        unsigned next = at;
        do {
            unsigned step = menu_tile_turn(next, delta < 0 ? -1 : 1);
            if (step == next) { next = at; break; } // the end
            next = step;
        } while (!menu_tile_shown(ui, next));
        if (next == at) break;
        at = next;
    }
    return at;
}

// HERE: a page's jobs that no printed key says.
enum class MenuJob : uint8_t {
    None, SequenceCapture, SequenceName, SequenceFollowAll, SequenceLaunch, SequenceClear,
    PatternDouble, PatternHalf, PatternClear, TrackMove, TrackCopy, TrackClear, Looper, Freeze, ModAdd, Chords, ChordFile, Stems,
    TrackName, TrackColour, SequenceColour,
};
struct MenuItem {
    MenuJob job = MenuJob::None;
    const char* name = "";
    const char* key = ""; // its shortcut, shown under the name
    bool danger = false;
};
constexpr unsigned menu_here_max = 10;
// The page's name on the MENU's header, "" for a page with nothing to list.
inline const char* menu_page_name(PageId page) {
    switch (page) {
    case PageId::Scenes: return "SEQUENCES";
    case PageId::Patterns: return "PATTERNS";
    case PageId::Roll: return "ROLL";
    case PageId::Motion: return "MOTION";
    case PageId::ModPage: return "MOD";
    case PageId::StepEdit: return "STEP EDIT";
    case PageId::Mixer: return "MIXER";
    case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: return "EFFECTS";
    case PageId::Stage: return "STAGE";
    default: return "";
    }
}
inline unsigned menu_here(const Project& project, const UiState& ui, std::array<MenuItem, menu_here_max>& out) {
    using J = MenuJob;
    const auto& track = project.tracks[ui.track % track_count];
    unsigned n = 0;
    auto add = [&](J job, const char* name, const char* key, bool danger = false) { if (n < menu_here_max) out[n++] = {job, name, key, danger}; };
    auto freeze = [&] { // a used track that is not an audio track: its PRINT sheet (Full's)
        if (freeze_shown(ui) && track.used() && !track.is_audio()) add(J::Freeze, "FREEZE", track.engine == EngineKind::Sampler ? "SAMPLE RECORD, B3" : "SAMPLE RECORD");
    };
    switch (ui.page) {
    case PageId::Scenes:
        add(J::SequenceCapture, "CAPTURE", "SHIFT + B2");
        add(J::SequenceName, "NAME", "PUSH");
        add(J::SequenceColour, "COLOUR", "SHIFT + ENCODER"); // the colour sheet (shell/track_colours.h)
        if (follow_shown(ui)) add(J::SequenceFollowAll, "FOLLOW TO ALL", "SHIFT + B3");
        add(J::SequenceLaunch, "LAUNCH", "PAD");
        add(J::SequenceClear, "CLEAR", "ERASE", true);
        break;
    case PageId::Patterns:
        add(J::SequenceLaunch, "LAUNCH SEQUENCE", "PUSH");
        add(J::SequenceColour, "SEQUENCE COLOUR", "SHIFT + B1, K2");
        add(J::PatternDouble, "DOUBLE", "SHIFT + PAD 3");
        add(J::PatternHalf, "HALF", "SHIFT + PAD 2");
        freeze();
        add(J::PatternClear, "CLEAR PATTERN", "SEQ + ERASE + PAD", true);
        break;
    case PageId::Roll: case PageId::Motion: case PageId::StepEdit:
        add(J::PatternDouble, "DOUBLE", "SHIFT + PAD 3");
        add(J::PatternHalf, "HALF", "SHIFT + PAD 2");
        freeze();
        add(J::PatternClear, "CLEAR PATTERN", "SEQ + ERASE + PAD", true);
        break;
    case PageId::ModPage: // MOD and MACRO: the lane last turned onto the modulator or macro in hand
        add(J::ModAdd, "ADD TARGET", "SHIFT + B3");
        break;
    case PageId::Mixer: case PageId::Rack: case PageId::Bus: case PageId::Master: case PageId::PadFx: case PageId::Stage:
        if (ui.page == PageId::Stage && levels_keys(track).key[0] != LevelsKey::None) { // the pads as chords (shell/levels_keys.h), and their file
            add(J::Chords, "CHORDS", "16 LEVELS + B2");
            add(J::ChordFile, "CHORD FILE", "16 LEVELS + B2, B2");
        }
        if (ui.page == PageId::Stage && track.engine == EngineKind::Sampler) { // a kit pad's sample into its stems (shell/stems_jobs.cpp)
            const auto& sound = track.pads[track.last_pad % sampler_pads];
            if (sound.used() && !project.samples[sound.sample % sample_slots].instrument()) add(J::Stems, "STEMS", "TRIM, SHIFT + B3");
        }
        add(J::TrackMove, "MOVE TRACK", "PAD BANK + B2");
        add(J::TrackCopy, "COPY TRACK", "PAD BANK + B3");
        if (track.used()) add(J::TrackName, "NAME TRACK", "MIXER, SHIFT + PUSH");
        if (track.used()) add(J::TrackColour, "TRACK COLOUR", ""); // issue 69: only here (shell/track_colours.h)
        if (looper_shown(ui)) add(J::Looper, "LOOPER", "LOOP + 16 LEVELS");
        freeze();
        add(J::TrackClear, "CLEAR TRACK", "PAD BANK + ERASE + PAD", true);
        break;
    default: break;
    }
    return n;
}

// MIDI thread (shell/menu.cpp).
void menu_open(Context& c);                        // the encoder held: HERE when the page lists something, else GO
bool menu_button(Context& c, SurfaceButton b);     // a press while it is open; false: the button's own job (it closes first)
void menu_wheel(Context& c, int delta);
void menu_pad(Context& c, unsigned pad);           // GO: that pad's place

} // namespace melodee
