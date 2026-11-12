// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The question asked before an erase or a loss (shell/ask.cpp), over the
// page, as one template: the question in words ("Clear pattern 2?"), one or
// two short plain lines saying what goes and whether undo brings it back,
// what it is about as a chip at the foot, and the undo keys when it can be
// undone; B1 CANCEL, B3 the verb in REC.
#include "melodee/pages/common.h"
#include "melodee/engine/engine_info.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/update.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

namespace {



// "1 note", "24 notes".
juce::String count_of(unsigned n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }

} // namespace

void draw_ask_sheet(Paint& p, const Snapshot& s) {
    using Ask = UiState::Ask;
    const auto& t = s.track();
    unsigned track = s.ui.track % track_count, playing = t.pattern % pattern_count;
    ConfirmSheet sheet;
    juce::String verb = "ERASE", cancel = "CANCEL";
    Icon verb_glyph = Icon::Trash;
    juce::Colour accent = colour::rec; // the verb's: red for a loss
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
        unsigned moves = s.pattern.lane_points(lane_param(int(s.ui.ask_target)), lane_pad(int(s.ui.ask_target))); // the selected pattern's
        sheet.icon = Icon::Motion;
        sheet.title = "Erase this lane?";
        sheet.lines = {count_of(moves, "move", "moves") + " of " + name_case(juce::String(lane_name(t, int(s.ui.ask_target), &s.project))) + (moves == 1 ? " goes." : " go."),
                       moves == 1 ? juce::String("Undo brings it back.") : undo_line};
        sheet.chip = "P" + juce::String(playing + 1);
        sheet.chip_colour = colour_of(s);
        sheet.name = "TRACK " + juce::String(track + 1);
        sheet.undo = true;
        break;
    }
    case Ask::ClearPattern: { // Seq held + Erase + a pad
        unsigned pattern = unsigned(s.ui.ask_target) % pattern_count;
        unsigned notes = s.sketches[track][pattern].count, moves = s.sketches[track][pattern].moves;
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
    case Ask::ClearTrack: { // Pad Bank held + Erase + a pad
        unsigned which = unsigned(s.ui.ask_target) % track_count, filled = 0;
        const auto& target = s.project.tracks[which];
        for (const auto& sketch : s.sketches[which]) filled += sketch.count + sketch.moves > 0;
        juce::String what = !target.used() ? juce::String("Its ") + count_of(filled, "pattern", "patterns")
                          : juce::String(target.engine == EngineKind::Sampler ? "Its kit" : "Its sound") + (filled ? " and " + count_of(filled, "pattern", "patterns") : "");
        bool one = target.used() ? !filled : filled == 1;
        sheet.title = "Clear track " + juce::String(which + 1) + "?";
        sheet.lines = {what + (one ? " goes." : " go."), one ? juce::String("Undo brings it back.") : undo_line};
        sheet.chip = target.used() ? juce::String(engine_info(target.engine).name) : juce::String("EMPTY");
        sheet.chip_colour = track_colour(which);
        sheet.name = "TRACK " + juce::String(which + 1);
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
    case Ask::InstallUpdate: // newer release on the card (shell/update.h)
        sheet.icon = Icon::Card;
        sheet.title = "Update to " + juce::String(s.ui.update.version) + "?";
        sheet.lines = {"The project is saved first,", "then Melodee restarts."};
        sheet.chip = juce::String("INSTALLED ") + melodee_version();
        sheet.chip_colour = colour::sec;
        verb = "INSTALL"; verb_glyph = Icon::Check; cancel = "LATER";
        break;
    case Ask::Unfreeze: { // freeze plan F2c: an edit of a frozen track
        unsigned which = unsigned(s.ui.ask_target) % track_count;
        const bool deep = s.freeze[which].state == FreezeState::Deep; // freeze plan F2d: thawed first
        sheet.icon = deep ? Icon::DeepSnow : Icon::Snow;
        sheet.accent = colour::ice;
        sheet.title = "Unfreeze track " + juce::String(which + 1) + "?";
        if (deep) sheet.lines = {"Its plug-ins load again,", "then it plays live."};
        else sheet.lines = {"Its sound plays live again.", "Prints stay until what made them changes."};
        unsigned kept = s.prints_kept[which];
        sheet.chip = count_of(kept, "PRINT", "PRINTS");
        sheet.chip_colour = colour::ice;
        sheet.name = "TRACK " + juce::String(which + 1);
        verb = "UNFREEZE"; verb_glyph = Icon::Snow; accent = colour::ice;
        break;
    }
    default: return;
    }

    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    { // the key the wheel is on, on a raised chip (Push presses it)
        float column = zone::width / 3.0f, x = (s.ui.ask_pick == 2 ? 2 : 0) * column;
        p.fill(x + 8, zone::soft_y + 2, column - 16, zone::soft_h - 4, colour::raised, 4);
    }
    bool on_verb = s.ui.ask_pick == 2; // the wheel's key, barred; Push presses it
    SoftKeys keys {{cancel, "", verb}, on_verb ? 2 : 0, on_verb ? accent : colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.glyphs[2] = verb_glyph;
    keys.tints[2] = accent;
    soft_keys(p, keys);
    p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
    p.dim_zone(zone::knobs_y, zone::knobs_h);
    confirm_sheet(p, sheet);
}

} // namespace melodee
