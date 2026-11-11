// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The question asked before an erase or a loss (shell/ask.cpp), over the
// page, as one template: the question in words ("Clear pattern 2?"), one or
// two short plain lines saying what goes and whether undo brings it back,
// what it is about as a chip at the foot, and the undo keys when it can be
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

// "1 note", "24 notes".
juce::String count_of(unsigned n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }

} // namespace

void draw_ask_sheet(Paint& p, const Snapshot& s) {
    using Ask = UiState::Ask;
    const auto& t = s.track();
    unsigned track = s.ui.track % track_count, playing = t.pattern % pattern_count;
    ConfirmSheet sheet;
    juce::String verb = "ERASE";
    Icon verb_glyph = Icon::Trash;
    const juce::String undo_line = "Undo brings them back.";
    switch (s.ui.ask) {
    case Ask::EraseKeyEvents: { // Erase + a pad
        unsigned count = 0;
        for (const auto& e : s.pattern.events) count += e.key == s.ui.ask_target;
        juce::String pad(key_label(t, int(s.ui.ask_target)));
        sheet.title = "Erase " + pad + " here?";
        sheet.lines = {(count == 1 ? juce::String("Its note") : "Its " + count_of(count, "note", "notes")) + " in this pattern go" + (count == 1 ? "es." : "."),
                       count == 1 ? juce::String("Undo brings it back.") : undo_line};
        sheet.chip = "P" + juce::String(playing + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = "TRACK " + juce::String(track + 1);
        sheet.undo = true;
        break;
    }
    case Ask::EraseLane: { // Erase + a pot on Motion
        unsigned moves = motion_moves(s.project, track, playing, int(s.ui.ask_target));
        sheet.icon = Icon::Motion;
        sheet.title = "Erase this lane?";
        sheet.lines = {count_of(moves, "move", "moves") + " of " + name_case(juce::String(lane_name(t, int(s.ui.ask_target)))) + (moves == 1 ? " goes." : " go."),
                       moves == 1 ? juce::String("Undo brings it back.") : undo_line};
        sheet.chip = "P" + juce::String(playing + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = "TRACK " + juce::String(track + 1);
        sheet.undo = true;
        break;
    }
    case Ask::ClearPattern: { // Seq held + Erase + a pad
        unsigned pattern = unsigned(s.ui.ask_target) % pattern_count;
        unsigned notes = s.sketches[track][pattern].count, moves = motion_moves(s.project, track, pattern);
        juce::String what = count_of(notes, "note", "notes");
        if (moves) what += " and " + count_of(moves, "motion move", "motion moves");
        sheet.title = "Clear pattern " + juce::String(pattern + 1) + "?";
        sheet.lines = {what + " of track " + juce::String(track + 1) + (notes + moves == 1 ? " goes." : " go."),
                       notes + moves == 1 ? juce::String("Undo brings it back.") : undo_line};
        sheet.chip = "P" + juce::String(pattern + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = "TRACK " + juce::String(track + 1);
        sheet.undo = true;
        verb = "CLEAR";
        break;
    }
    case Ask::ResetSettings:
        sheet.icon = Icon::Undo;
        sheet.title = "Reset the settings?";
        sheet.lines = {"Settings go back to factory.", "Projects stay."};
        verb = "RESET"; verb_glyph = Icon::Check;
        break;
    case Ask::NewProject: {
        sheet.icon = Icon::Plus;
        sheet.title = "Start a new project?";
        juce::String name = s.project.name[0] ? juce::String::fromUTF8(s.project.name) : juce::String("This project");
        if (s.ui.ask_target == 1) sheet.lines = {name + " is saved first.", ""};
        else if (s.ui.ask_target == 2) sheet.lines = {"This project was never saved.", "Its changes are lost."};
        else sheet.lines = {name + " has nothing unsaved.", ""};
        verb = "NEW"; verb_glyph = Icon::Check;
        break;
    }
    case Ask::TakeCardBack:
        sheet.icon = Icon::Card;
        sheet.title = "Take the card back?";
        sheet.lines = {"The computer has not ejected it.", "Files being written may break."};
        verb = "TAKE"; verb_glyph = Icon::Check;
        break;
    case Ask::ClearLoop: {
        unsigned loop = unsigned(s.ui.ask_target) % 4;
        sheet.icon = Icon::Loop;
        sheet.title = "Clear loop " + juce::String(loop + 1) + "?";
        sheet.lines = {"Its audio and its layers go.", "Undo can't bring them back."};
        sheet.chip = "L" + juce::String(loop + 1);
        sheet.chip_colour = loop_colour(loop);
        verb = "CLEAR";
        break;
    }
    case Ask::LooperOff:
        sheet.icon = Icon::Power;
        sheet.title = "Turn the Looper off?";
        sheet.lines = {"Its four loops are cleared.", "Undo can't bring them back."};
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
}

} // namespace melodee
