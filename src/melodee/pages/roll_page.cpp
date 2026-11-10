// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// ROLL (B2 on Steps): a piano roll of the bar the cursor is in. The wheel
// walks the cursor through time and the view follows; pads play and write
// notes at the cursor, up to four a step (a chord); Minus / Plus move the
// octave and the view with it. K1 length, K2 velocity, K3 chance. Push on a
// note opens the note sheet (accent, slide, tie, copy, delete); Shift + Push
// closes it. With Seq Record on, pads play in live instead.
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
constexpr float gutter = 23, ruler = 13, row_h = 10.5f, column = 18.5f;
constexpr unsigned rows = 12, sheet_rows = 5;
constexpr unsigned copy_ahead = 8; // Copy to: half a bar on, as on the board (step 23 to 31)

unsigned bars_of(const Pattern& p) { return std::max(1u, (p.steps() + bar_steps - 1) / bar_steps); } // pages of sixteen steps
unsigned cursor_of(const UiState& ui, const Pattern& p) { return unsigned(std::max<int>(ui.step_cursor, 0)) % p.steps(); }
// The top row's key: three semitones above the pads' top octave, so the
// pads' range sits in view (D2 pads show F#2 to F3).
int top_key(const Project& project, const Track& track) { return pad_note(project, track, 0).key + 15; }
bool black(int key) { return (0x54A >> (key % 12)) & 1; } // C#, D#, F#, G#, A#

// The selected note of the cursor step, if any.
const Note* selected(const Step& step, unsigned index) {
    unsigned n = 0;
    for (auto& note : step.notes) if (note.velocity && n++ == index) return &note;
    return step.first();
}

juce::String step_label(unsigned at, const Note* note) {
    return "STEP " + juce::String(at + 1) + (note ? juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(note_name(note->key)) : "");
}

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    const auto& track = s.track();
    auto c = colour_of(s);
    unsigned cursor = cursor_of(s.ui, pattern), bar = cursor / bar_steps, first = bar * bar_steps;
    const auto& cur = pattern.step(cursor);
    const Note* note = selected(cur, s.ui.roll_note);
    Header h = header_of(s);
    if (!s.ui.recording) h.left.push_back(pill("BAR " + juce::String(bar + 1) + " / " + juce::String(bars_of(pattern))));
    if (note) h.left.push_back(chip(step_label(cursor, note), c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, {{"STEPS", "ROLL", "MOTION"}, 1, c}, h);

    float top = zone::picture_y + ruler;
    int key_top = top_key(s.project, track);
    for (unsigned b = 0; b < 4; ++b)
        p.text(juce::String(bar + 1) + "." + juce::String(b + 1), gutter + b * 4 * column + 2, zone::picture_y, 30, 13,
               plain(colour::mid, type::small, Weight::SemiBold));
    for (unsigned r = 0; r < rows; ++r) {
        int key = key_top - int(r);
        float y = top + r * row_h;
        p.fill(0, y, zone::width, row_h - 0.5f, black(key) ? colour::quiet : colour::panel);
    }
    // Names on C, D, F, G and A (and the root): no two side by side, so each
    // has room at the floor size; drawn over the rows, a little taller than one.
    for (unsigned r = 0; r < rows; ++r) {
        int key = key_top - int(r);
        float y = top + r * row_h;
        int pitch = key % 12, root_pitch = int(s.project.root % 12);
        bool root = pitch == root_pitch;
        bool beside_root = (root_pitch == 4 && pitch == 5) || (root_pitch == 11 && pitch == 0) || (root_pitch == 5 && pitch == 4) || (root_pitch == 0 && pitch == 11);
        bool named = root || (!beside_root && (pitch == 0 || pitch == 2 || pitch == 5 || pitch == 7 || pitch == 9));
        if (!black(key) && named) {
            p.text(note_name(key), 0, y - 2, gutter - 4, row_h + 3, plain(root ? c : colour::dim, type::small, Weight::SemiBold),
                   juce::Justification::centredRight);
        }
    }
    p.fill(gutter - 1, top, 1, rows * row_h, colour::line);
    for (unsigned k = 1; k < bar_steps; ++k)
        p.fill(gutter + k * column, top, k % 4 ? 0.5f : 1, rows * row_h, k % 4 ? colour::bg : colour::line);
    p.fill(gutter + (cursor - first) * column, top, column, rows * row_h, c.withAlpha(0.12f));
    for (unsigned k = 0; k < bar_steps && first + k < pattern.steps(); ++k) {
        const auto& step = pattern.step(first + k);
        for (const auto& n : step.notes) {
            if (!n.velocity) continue;
            int r = key_top - n.key;
            if (r < 0 || r >= int(rows)) continue;
            float x = gutter + (k + float(n.offset) / pattern.q()) * column + 1, y = top + r * row_h + 1; // where in the step it was played
            float w = std::min<float>(step.length, float(bar_steps - k)) * column - 2;
            bool on = first + k == cursor && note && n.key == note->key; // the step is a copy: the same note by its key
            p.fill(x, y, w, row_h - 2.5f, on ? c : c.withAlpha(0.45f + n.velocity / 127.0f * 0.55f), 2.5f);
            if (on) p.ring(x - 1, y - 1, w + 2, row_h - 0.5f, colour::text, 3);
        }
    }
    if (s.playing) {
        unsigned at = (s.position % pattern.length) / pattern.q();
        if (at >= first && at < first + bar_steps) p.fill(gutter + (at - first) * column, top - 3, 1, rows * row_h + 3, colour::text);
    }
    // Where the view sits: time along the bottom, pitch along the right edge.
    float tw = zone::width - gutter - 3, bars = float(bars_of(pattern));
    p.fill(gutter, top + rows * row_h + 3.5f, tw, 2, colour::line, 1);
    p.fill(gutter + tw * bar / bars, top + rows * row_h + 3.5f, tw / bars, 2, colour::mid, 1);
    float ph = rows * row_h, place = std::clamp((key_top - 12) / 115.0f, 0.0f, 1.0f);
    p.fill(zone::width - 3, top, 2, ph, colour::line, 1);
    p.fill(zone::width - 3, top + ph * (1 - place) * 0.74f, 2, ph * 0.26f, colour::mid, 1);

    if (s.ui.sheet_open && note) {
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        MenuSheet m;
        m.title = "NOTE"; m.detail = step_label(cursor, note); m.accent = c;
        unsigned target = (cursor + copy_ahead) % pattern.steps();
        m.rows[0] = {"Accent", cur.flags & step_accent ? "On" : "Off", bool(cur.flags & step_accent), false, false};
        m.rows[1] = {"Slide", cur.flags & step_slide ? "On" : "Off", bool(cur.flags & step_slide), false, false};
        m.rows[2] = {"Tie to next", cur.flags & step_tie ? "On" : "Off", bool(cur.flags & step_tie), false, false};
        m.rows[3] = {"Copy to", "Step " + juce::String(target + 1), false, false, false};
        m.rows[4] = {"Delete note", "", false, true, false};
        m.count = sheet_rows; m.current = s.ui.sheet_row % sheet_rows;
        menu_sheet(p, m);
    }
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    c.play_pad(pad, velocity, down);
    if (!down || !c.track().used() || c.ui.sheet_open) return;
    if (c.ui.recording && c.host.playing()) return; // played in live by Seq Record
    auto& pattern = c.pattern();
    unsigned at = cursor_of(c.ui, pattern);
    int key = pad_note(c.project, c.track(), pad).key;
    int v = c.track().layout == PadLayout::Levels ? level_velocity(pad) : velocity;
    Step step = pattern.step(at);
    unsigned index = 0;
    for (auto& n : step.notes) {
        if (n.velocity && n.key == key) { c.ui.roll_note = uint8_t(index); return; } // already there
        if (n.velocity) ++index;
    }
    for (auto& n : step.notes)
        if (!n.velocity) {
            n = {uint8_t(std::clamp(key, 0, 127)), uint8_t(v)};
            c.set_step(c.ui.track, at, step);
            c.ui.roll_note = uint8_t(index);
            return;
        }
}

void wheel(Context& c, int delta) {
    if (c.ui.sheet_open) {
        c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(sheet_rows) - 1));
        return;
    }
    auto& pattern = c.pattern();
    c.ui.step_cursor = int16_t(std::clamp<int>(c.ui.step_cursor + delta, 0, int(pattern.steps()) - 1));
    c.ui.steps_bar = uint8_t(c.ui.step_cursor / bar_steps);
    c.ui.roll_note = 0;
}

void push(Context& c) {
    auto& pattern = c.pattern();
    unsigned at = cursor_of(c.ui, pattern);
    Step step = pattern.step(at);
    if (step.empty()) return;
    if (!c.ui.sheet_open) { c.ui.sheet_open = true; c.ui.sheet_row = 0; return; }
    switch (c.ui.sheet_row) {
    case 0: step.flags ^= step_accent; break;
    case 1: step.flags ^= step_slide; break;
    case 2: step.flags ^= step_tie; break;
    case 3: c.set_step(c.ui.track, (at + copy_ahead) % pattern.steps(), step); return;
    case 4: {
        // Delete the selected note; the others close up.
        Step left {};
        left.length = step.length; left.chance = step.chance; left.flags = step.flags;
        unsigned index = 0, w = 0;
        for (auto& n : step.notes) {
            if (!n.velocity) continue;
            if (index++ != c.ui.roll_note) left.notes[w++] = n;
        }
        c.set_step(c.ui.track, at, w ? left : Step {});
        c.ui.roll_note = 0;
        c.ui.sheet_open = false;
        return;
    }
    default: return;
    }
    c.set_step(c.ui.track, at, step);
}

void soft(Context& c, int key) {
    static const PageId pages[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(pages[key % 3]);
}
} // namespace

const PageSpec roll_page {draw, pad, wheel, push, soft, octave};

} // namespace melodee
