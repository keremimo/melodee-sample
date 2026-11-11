// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The question asked before an erase or a loss (shell/ask.cpp), over the
// page, as one template without sentences: what it is about (a glyph and a
// chip), how much goes (glyphs and numbers), the undo keys when it can be
// undone; B1 CANCEL, B3 the verb in REC.
#include "melodee/pages/common.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/step_edit.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

namespace {

// The motion events of one track's pattern (all its lanes), or of one lane.
unsigned motion_moves(const Project& project, unsigned track, unsigned pattern, int lane = -1) {
    unsigned count = 0;
    const auto& m = project.motion;
    for (unsigned n = 0; n < m.count; ++n) {
        const auto& e = m.events[n];
        if (lane < 0) count += e.track == track && e.pattern == pattern;
        else count += e.same_lane(uint8_t(track), uint8_t(pattern), uint8_t(lane_param(lane)), uint8_t(lane_pad(lane)));
    }
    return count;
}

// The pattern in small under the sheet's name: a mark for each sixteenth that starts a note.
void sketch(Paint& p, const PatternSketch& k, juce::Colour c, float x, float y, float w, float h) {
    unsigned steps = std::clamp<unsigned>(k.length / sixteenth_ticks, 1, 64);
    int low = 127, high = 0;
    for (unsigned n = 0; n < steps; ++n) if (k.keys[n]) { low = std::min<int>(low, k.keys[n]); high = std::max<int>(high, k.keys[n]); }
    float sw = w / steps;
    for (unsigned n = 0; n < steps; ++n) {
        if (!k.keys[n]) continue;
        float at = high > low ? float(k.keys[n] - low) / float(high - low) : 0.5f;
        p.fill(x + n * sw + 0.5f, y + (1 - at) * (h - 5), std::max(2.0f, sw - 2), 5, c, 1.5f);
    }
}

} // namespace

void draw_ask_sheet(Paint& p, const Snapshot& s) {
    using Ask = UiState::Ask;
    const auto& t = s.track();
    unsigned track = s.ui.track % track_count, playing = t.pattern % pattern_count;
    ConfirmSheet sheet;
    juce::String verb = "ERASE";
    Icon verb_glyph = Icon::Trash;
    switch (s.ui.ask) {
    case Ask::EraseKeyEvents: {
        unsigned count = 0;
        for (const auto& e : s.pattern.events) count += e.key == s.ui.ask_target;
        sheet.chip = juce::String(key_label(t, int(s.ui.ask_target)));
        sheet.chip_colour = colour_of(s);
        sheet.name = "P" + juce::String(playing + 1);
        sheet.counts[0] = {Icon::Note, juce::String(count)};
        sheet.undo = true;
        break;
    }
    case Ask::EraseLane:
        sheet.icon = Icon::Motion;
        sheet.chip = "P" + juce::String(playing + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = juce::String(lane_name(t, int(s.ui.ask_target)));
        sheet.counts[0] = {Icon::Motion, juce::String(motion_moves(s.project, track, playing, int(s.ui.ask_target)))};
        sheet.undo = true;
        break;
    case Ask::ClearPattern: {
        unsigned pattern = unsigned(s.ui.ask_target) % pattern_count;
        sheet.chip = "P" + juce::String(pattern + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = "TRACK " + juce::String(track + 1);
        if (unsigned moves = motion_moves(s.project, track, pattern)) sheet.counts[0] = {Icon::Motion, juce::String(moves)};
        sheet.undo = true;
        verb = "CLEAR";
        break;
    }
    case Ask::ResetSettings:
        sheet.icon = Icon::Undo;
        sheet.name = "FACTORY SETTINGS";
        sheet.note = "PROJECTS STAY";
        sheet.note_colour = colour::sec;
        verb = "RESET"; verb_glyph = Icon::Check;
        break;
    case Ask::NewProject:
        sheet.icon = Icon::Plus;
        sheet.name = "NEW PROJECT";
        if (s.ui.ask_target == 1) { sheet.note = "SAVED FIRST"; sheet.note_colour = colour::play; }
        else if (s.ui.ask_target == 2) { sheet.note = "UNSAVED LOST"; sheet.note_colour = colour::rec; }
        verb = "NEW"; verb_glyph = Icon::Check;
        break;
    case Ask::TakeCardBack:
        sheet.icon = Icon::Card;
        sheet.name = "TAKE BACK";
        sheet.note = "NOT EJECTED";
        sheet.note_colour = colour::amber;
        verb = "TAKE"; verb_glyph = Icon::Check;
        break;
    case Ask::ClearLoop: {
        unsigned loop = unsigned(s.ui.ask_target) % 4;
        sheet.chip = "L" + juce::String(loop + 1);
        sheet.chip_colour = loop_colour(loop);
        sheet.name = "LOOP";
        sheet.counts[0] = {Icon::Loop, "AUDIO"};
        verb = "CLEAR";
        break;
    }
    case Ask::LooperOff:
        sheet.icon = Icon::Power;
        sheet.name = "LOOPER OFF";
        sheet.note = "LOOPS CLEARED";
        sheet.note_colour = colour::rec;
        verb = "TURN OFF"; verb_glyph = Icon::Power;
        break;
    default: return;
    }

    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    SoftKeys keys {{"CANCEL", "", verb}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.glyphs[2] = verb_glyph;
    keys.tints[2] = colour::rec;
    soft_keys(p, keys);
    p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
    p.dim_zone(zone::knobs_y, zone::knobs_h);
    confirm_sheet(p, sheet);
    if (s.ui.ask == Ask::ClearPattern) { // what goes, drawn: the pattern in small under its name
        constexpr float w = 268, h = 112, x = (zone::width - w) / 2, y = zone::picture_y + (zone::picture_h - h) / 2;
        sketch(p, s.sketches[track][unsigned(s.ui.ask_target) % pattern_count], colour_of(s), x + 14, y + 46, w - 28, 26);
    }
}

} // namespace melodee
