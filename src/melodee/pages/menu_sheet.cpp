// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The MENU over the page (shell/menu.h), opened by holding the encoder: B1 BACK, B2 HERE, B3 GO.
// HERE: the page's jobs as cards two to a row (ui::card_sheet), each with its shortcut under its name; the
// wheel moves, Push does the lit one. GO: the sixteen places as the pads sit (pad 1 bottom left), each a glyph,
// its pad's number and its name, in four groups by colour (sound, sequence, mix, project); the tile the wheel
// is on is raised and its shortcut is on the foot line. A pad opens its place.
#include "melodee/pages/common.h"
#include "melodee/shell/menu.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

namespace {

Icon place_icon(unsigned tile) {
    static const Icon icons[16] {Icon::Wav, Icon::Folder, Icon::Rec, Icon::Scissors, Icon::Pattern, Icon::Note, Icon::Bars, Icon::Motion,
                                 Icon::Sliders, Icon::Route, Icon::Tone, Icon::Loop, Icon::Flag, Icon::Save, Icon::Gear, Icon::Metro};
    return icons[tile % 16];
}
Icon job_icon(MenuJob job) {
    using J = MenuJob;
    switch (job) {
    case J::SequenceCapture: return Icon::Copy;
    case J::SequenceName: case J::TrackName: return Icon::Keyboard;
    case J::SequenceFollowAll: return Icon::ArrowRight;
    case J::SequenceLaunch: return Icon::Play;
    case J::SequenceClear: case J::PatternClear: case J::TrackClear: return Icon::Trash;
    case J::PatternDouble: case J::ModAdd: return Icon::Plus;
    case J::Chords: return Icon::Chord;
    case J::ChordFile: return Icon::File;
    case J::PatternHalf: return Icon::Minus;
    case J::TrackMove: return Icon::Swap;
    case J::TrackCopy: return Icon::Copy;
    case J::TrackColour: case J::SequenceColour: return Icon::Pads;
    case J::Looper: return Icon::Loop;
    case J::Freeze: return Icon::Snow;
    case J::Stems: return Icon::Layers;
    default: return Icon::None;
    }
}
// The four groups' colours: the first, third, fifth and second of the sixteen, as the pads are lit (shell/pad_lights.cpp).
juce::Colour group_colour(unsigned group) {
    static const unsigned tracks[4] {0, 2, 4, 1};
    return slot_colour(tracks[group % 4]);
}

} // namespace

void draw_menu(Paint& p, const Snapshot& s) {
    const auto& ui = s.ui;
    if (!ui.menu) return;
    std::array<MenuItem, menu_here_max> items {};
    unsigned n = menu_here(s.project, ui, items);
    bool here = ui.menu == menu_here_view && n;
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    if (ui.menu == menu_learn_view) { // LEARN: the lessons as cards, a check on those done
        soft_keys(p, SoftKeys {{"BACK", "", "START"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
        CardSheet m;
        m.chip = "LEARN"; m.title = "LESSONS";
        unsigned done = 0;
        for (unsigned k = 0; k < lesson_count; ++k) {
            const Lesson& l = lesson_at(k);
            bool finished = ui.settings.lessons_done >> k & 1;
            done += finished;
            m.items[k] = card_item(finished ? Icon::Check : Icon::Play, l.name, finished ? juce::String("DONE") : juce::String(l.minutes) + " MIN");
        }
        m.detail = juce::String(done) + " OF " + juce::String(lesson_count) + " DONE"; m.detail_colour = colour::mid;
        m.count = lesson_count; m.current = ui.menu_row % lesson_count;
        card_sheet(p, m);
        return;
    }
    SoftKeys keys {{"BACK", n && !ui.lesson ? "HERE" : "", "GO"}, here ? 1 : 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    soft_keys(p, keys);
    if (here) {
        CardSheet m;
        m.chip = "MENU"; m.title = menu_page_name(ui.page);
        m.detail = "HERE"; m.detail_colour = colour::mid;
        for (unsigned k = 0; k < n; ++k) m.items[k] = card_item(job_icon(items[k].job), items[k].name, items[k].key, false, items[k].danger);
        m.count = n; m.current = ui.menu_row % n;
        card_sheet(p, m);
        return;
    }
    p.fill(0, zone::header_y, zone::width, zone::height - zone::header_y, colour::bg);
    Header h = header_of(s); // the transport and the tempo as on every page; the MENU's own chips in place of the page's
    h.left.clear(); h.right.clear(); h.lead = 0;
    h.left.push_back(chip("MENU", colour::text)); // GO is lit on B3
    // What the tracks take of each audio block, all together, and the selected track's share (snow: frozen), as the
    // Tracks page showed them (docs/MIXER_PLAN.md step 6, decision 5).
    const float all = s.machine.render_load;
    h.left.push_back(pill(Icon::Chip, "CPU " + juce::String(int(std::lround(all * 100))) + " %", load_colour(all)));
    if (s.track().used()) {
        const auto& f = s.freeze[s.ui.track % track_count];
        const float load = s.machine.track_load[s.ui.track % track_count];
        h.right.push_back(pill(f.state == FreezeState::Deep ? Icon::DeepSnow : f.frozen() ? Icon::Snow : Icon::Chip,
                               "T" + juce::String(s.ui.track % track_count + 1) + juce::String::fromUTF8(" \xc2\xb7 ")
                                   + juce::String(int(std::lround(load * 100))) + " %",
                               f.frozen() ? colour::ice : load_colour(load)));
    }
    h.right.push_back(label(s.project.name));
    header(p, h);
    constexpr float gap = 4, tw = (zone::width - 2 * zone::pad - 3 * gap) / 4, th = 36, top = zone::picture_y + 2;
    const auto& places = menu_places();
    unsigned lit = ui.menu_tile % 16;
    for (unsigned pad = 0; pad < 16; ++pad) { // pads 13-16 along the top, 1-4 along the bottom
        float x = zone::pad + float(pad % 4) * (tw + gap), y = top + float(3 - pad / 4) * (th + gap);
        auto c = group_colour(places[pad].group);
        bool on = pad == lit;
        if (!menu_tile_shown(ui, pad) || (ui.lesson && pad != 15)) { p.fill(x, y, tw, th, colour::quiet, 4); continue; } // Basic leaves it out, or a lesson runs: its pad stays empty
        p.fill(x, y, tw, th, on ? colour::raised : colour::panel, 4);
        if (on) p.ring(x, y, tw, th, c, 4, 1.2f);
        icon(p, place_icon(pad), x + 5, y + 5, 12, c, 1.4f);
        p.text(juce::String(pad + 1), x + tw - 24, y + 3, 19, 13, plain(colour::dim, type::small, Weight::Medium), juce::Justification::centredRight);
        juce::String name(ui.lesson && pad == 15 ? "LEAVE" : places[pad].name); // a lesson: its way out
        p.text(name, x + 5, y + 20, tw - 8, 13, p.fit(name, caps_text(on ? colour::text : colour::sec), tw - 8, type::floor));
    }
    // The foot line: the lit place and how it opens without the MENU.
    float fy = top + 4 * (th + gap) + 2, fw = zone::width - 2 * zone::pad;
    p.fill(zone::pad, fy, fw, 20, colour::panel, 4);
    juce::String name(ui.lesson && lit == 15 ? "LEAVE THE LESSON" : places[lit].name);
    float nw = p.text(name, zone::pad + 6, fy + 3, fw - 12, 14, caps_text(colour::text));
    juce::String key = places[lit].key[0] ? juce::String::fromUTF8("\xc2\xb7 ") + places[lit].key : juce::String();
    if (lit == 15 && !ui.lesson) key = juce::String::fromUTF8("\xc2\xb7 ") + juce::String(lesson_count) + " LESSONS";
    p.text(key, zone::pad + 6 + nw + 5, fy + 3, fw - 17 - nw, 14, p.fit(key, caps_text(colour::mid), fw - 17 - nw, type::floor));
}

} // namespace melodee
