// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// ROLL (B2 on a sequence page, or Seq held + B2): a piano roll of the
// selected track's pattern, read from its events (shell/roll.h); on a kit a
// lane per pad, numbered as the pads are (the kit's bank in view). Each
// note sits at its own tick for its own length, over bar lines, any number
// to a step. The cursor is a step of Q (the wheel; Shift + wheel note to
// note) and a pitch (Minus / Plus: the chord in the step, then a semitone);
// the note there is in focus, drawn white, and the cards edit it (K1 length,
// K2 velocity, K3 chance). The view scrolls with the cursor a step at a time;
// dots on the top and bottom edges mark notes above and below it. Pads write
// their note at the cursor, NEW NOTE long, or focus it; Pad Bank moves their
// octave (Shift: down). Erase tapped alone deletes the note in focus. Push
// opens its sheet: B1 closes it, B3 or Push do the row, a pad or a pot
// closes it. With Seq Record on while playing, pads play in live.
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/step_edit.h"
#include "melodee/ui/widgets.h"
#include <algorithm>

namespace melodee {
namespace {
using namespace ui;
constexpr float gutter = 23, ruler = 13, row_h = 10.5f, column = 18.5f;
constexpr unsigned rows = roll_rows, columns = roll_columns;
enum SheetRow : uint8_t { RowAccent, RowSlide, RowTie, RowDelete, sheet_rows };

bool black(int key) { return (0x54A >> (key % 12)) & 1; } // C#, D#, F#, G#, A#

juce::String where(const Snapshot& s, uint32_t tick) {
    return juce::String(roll_position(tick, s.project.bar_ticks(), s.project.beat_ticks(), s.pattern.q()));
}

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    UiState ui = s.ui;
    roll_follow(pattern, ui); // the view onto the cursor, even when it came from another page
    auto c = colour_of(s);
    unsigned q = pattern.q(), cursor = roll_cursor(pattern, ui), first = roll_first(pattern, ui);
    unsigned bar = s.project.bar_ticks(), beat = s.project.beat_ticks();
    uint32_t from = first * q, to = (first + columns) * q;
    int focus = roll_focus(pattern, ui), top_key = ui.roll_top;
    const Event* focused = focus >= 0 ? &pattern.events[size_t(focus)] : nullptr;

    const auto& track = s.track();
    bool kit = track.engine == EngineKind::Sampler || track.engine == EngineKind::Kit909; // lanes of pads, not keys
    Header h = header_of(s);
    if (!ui.recording) {
        if (focused) h.left.push_back(chip(where(s, focused->tick) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(key_label(track, focused->key)), c));
        else h.left.push_back(pill("BAR " + juce::String(cursor * q / bar + 1) + " / " + juce::String((pattern.length + bar - 1) / bar)));
    }
    h.right.push_back(tag_of(s, ui.track));
    bool sheet = ui.sheet_open && focused;
    frame(p, s, sheet ? SoftKeys({"CLOSE", "", "DO IT"}) : SoftKeys({"STEPS", "ROLL", "MOTION"}, 1, c), h);

    float top = zone::picture_y + ruler, height = rows * row_h, right = gutter + columns * column;
    auto x_of = [&](uint32_t tick) { return gutter + (float(tick) - float(from)) / float(q) * column; };
    // The ruler: bar.beat over each beat, where there is room for it.
    float last_label = -100;
    for (unsigned k = 0; k < columns; ++k) {
        uint32_t tick = (first + k) * q;
        if (tick % beat || gutter + k * column < last_label + 34) continue;
        last_label = gutter + k * column;
        p.text(juce::String(tick / bar + 1) + "." + juce::String(tick % bar / beat + 1), last_label + 2, zone::picture_y, 32, 13,
               plain(colour::mid, type::small, Weight::SemiBold));
    }
    for (unsigned r = 0; r < rows; ++r) {
        int key = top_key - int(r);
        bool shade = kit ? (key - int(sampler_first_note)) / 4 % 2 != 0 : key >= 0 && black(key); // a kit: pads in fours
        p.fill(0, top + r * row_h, zone::width, row_h - 0.5f, shade ? colour::quiet : colour::panel);
    }
    if (kit) { // the pads' numbers in their bank, every other lane so each has room, the cursor's always
        for (unsigned r = 0; r < rows; ++r) {
            int key = top_key - int(r), pad = key - int(sampler_first_note);
            if (pad < 0 || (key - int(ui.roll_key)) % 2) continue;
            p.text(juce::String(pad % 16 + 1), 0, top + r * row_h - 2, gutter - 4, row_h + 3,
                   plain(key == int(ui.roll_key) ? c : colour::dim, type::small, Weight::SemiBold), juce::Justification::centredRight);
        }
    }
    // Names on C, F, G and A and the root: never two side by side, so each has room at the floor size.
    int root = int(s.project.root % 12);
    for (unsigned r = 0; r < rows && !kit; ++r) {
        int key = top_key - int(r);
        if (key < 0) break;
        int pitch = key % 12;
        bool named = pitch == root || ((pitch == 0 || pitch == 5 || pitch == 7 || pitch == 9) && std::abs(pitch - root) > 1 && std::abs(pitch - root) < 11);
        if (black(key) || !named) continue;
        p.text(note_name(key), 0, top + r * row_h - 2, gutter - 4, row_h + 3, plain(pitch == root ? c : colour::dim, type::small, Weight::SemiBold),
               juce::Justification::centredRight);
    }
    p.fill(gutter - 1, top, 1, height, colour::line);
    for (unsigned k = 1; k < columns; ++k) {
        uint32_t tick = (first + k) * q;
        bool on_bar = tick % bar == 0, on_beat = tick % beat == 0;
        p.fill(gutter + k * column, top, on_beat ? 1 : 0.5f, height, on_bar ? colour::mid.withAlpha(0.45f) : on_beat ? colour::line : colour::bg);
    }
    // The cursor: its step's column and its pitch's row.
    if (cursor >= first && cursor < first + columns) p.fill(gutter + (cursor - first) * column, top, column, height, c.withAlpha(0.12f));
    int cursor_row = top_key - int(ui.roll_key);
    if (cursor_row >= 0 && cursor_row < int(rows)) p.fill(gutter, top + cursor_row * row_h, right - gutter, row_h - 0.5f, c.withAlpha(0.09f));
    // The notes, each at its own tick and for its own length; those above or below the rows are dots on the edge.
    std::array<uint8_t, columns> above {}, below {};
    for (size_t n = 0; n < pattern.events.size(); ++n) {
        const auto& e = pattern.events[n];
        if (e.tick >= pattern.length) break;
        uint32_t end = std::min<uint32_t>(e.tick + e.length, pattern.length);
        if (end <= from || e.tick >= to) continue;
        int r = top_key - int(e.key);
        if (r < 0 || r >= int(rows)) {
            if (e.tick >= from) (r < 0 ? above : below)[(e.tick - from) / q] = 1;
            continue;
        }
        float x = std::max(x_of(e.tick), gutter) + 1, xe = std::min(x_of(end), right + 1), y = top + r * row_h + 1;
        float w = std::max(xe - x - 1, 2.0f), hgt = row_h - 2.5f;
        bool on = int(n) == focus;
        p.fill(x, y, w, hgt, on ? colour::text : c.withAlpha(0.4f + e.velocity / 127.0f * 0.6f), 2.5f);
        if (on) p.ring(x - 1, y - 1, w + 2, hgt + 2, c, 3, 1.2f);
        if ((e.flags & step_accent) && w > 6) p.fill(x + 1.5f, y + 1.5f, 2, hgt - 3, on ? c : colour::bg, 1);
    }
    for (unsigned k = 0; k < columns; ++k) {
        float cx = gutter + k * column + column / 2;
        if (above[k]) p.circle(cx, top + 2, 1.8f, colour::mid);
        if (below[k]) p.circle(cx, top + height - 2.5f, 1.8f, colour::mid);
    }
    if (s.playing && pattern.length) {
        uint32_t at = uint32_t(s.position % pattern.length);
        if (at >= from && at < to) p.fill(x_of(at), top - 3, 1, height + 3, colour::text);
    }
    // Where the view sits: the whole pattern along the bottom with its notes, pitch along the right edge.
    float tw = zone::width - gutter - 3, by = top + height + 3.5f;
    p.fill(gutter, by, tw, 2, colour::line, 1);
    for (const auto& e : pattern.events) {
        if (e.tick >= pattern.length) break;
        p.fill(gutter + tw * float(e.tick) / float(pattern.length), by, 1, 2, colour::dim);
    }
    float steps = float(pattern.steps());
    p.fill(gutter + tw * first / steps, by, tw * std::min<float>(columns, steps) / steps, 2, colour::mid.withAlpha(0.7f), 1);
    float place = std::clamp((top_key - 12) / 115.0f, 0.0f, 1.0f);
    p.fill(zone::width - 3, top, 2, height, colour::line, 1);
    p.fill(zone::width - 3, top + height * (1 - place) * 0.74f, 2, height * 0.26f, colour::mid, 1);

    if (sheet) {
        p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
        MenuSheet m;
        m.title = "NOTE"; m.detail = where(s, focused->tick) + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(note_name(focused->key)); m.accent = c;
        auto flag = [&](uint8_t f) { return bool(focused->flags & f); };
        m.rows[RowAccent] = {"Accent", flag(step_accent) ? "On" : "Off", flag(step_accent), false, false};
        m.rows[RowSlide] = {"Slide", flag(step_slide) ? "On" : "Off", flag(step_slide), false, false};
        m.rows[RowTie] = {"Tie to next", flag(step_tie) ? "On" : "Off", flag(step_tie), false, false};
        m.rows[RowDelete] = {"Delete note", "", false, true, false};
        m.count = sheet_rows; m.current = ui.sheet_row % sheet_rows;
        menu_sheet(p, m);
    }
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    if (down && c.ui.erase) { // Erase + pad: every note of its key, once B3 says so (as on Step Edit)
        c.ui.sheet_open = false;
        int key = pad_note(c.project, c.track(), pad).key;
        if (c.track().used() && key >= 0) ask(c, UiState::Ask::EraseKeyEvents, key);
        return;
    }
    c.play_pad(pad, velocity, down);
    if (!down || !c.track().used()) return;
    if (c.ui.sheet_open) c.ui.sheet_open = false; // a pad leaves the sheet and writes as ever
    if (c.ui.recording && c.host.playing()) return; // played in live by Seq Record
    int v = c.track().layout == PadLayout::Levels ? level_velocity(pad) : velocity;
    roll_write(c, pad_note(c.project, c.track(), pad).key, uint8_t(v));
}

void wheel(Context& c, int delta) {
    if (c.ui.sheet_open) {
        c.ui.sheet_row = uint8_t(std::clamp<int>(c.ui.sheet_row + delta, 0, int(sheet_rows) - 1));
        return;
    }
    if (c.ui.shift) { // note to note, over empty bars
        c.ui.shift_used = true;
        if (!roll_jump(c.pattern(), c.ui, delta)) c.notice(delta > 0 ? "No later note" : "No earlier note");
        return;
    }
    roll_step(c.pattern(), c.ui, delta);
}

void do_row(Context& c) {
    switch (c.ui.sheet_row % sheet_rows) {
    case RowAccent: roll_flag(c, step_accent); break;
    case RowSlide: roll_flag(c, step_slide); break;
    case RowTie: roll_flag(c, step_tie); break;
    case RowDelete: c.ui.sheet_open = false; roll_erase(c); break;
    default: break;
    }
}

void push(Context& c) {
    if (c.ui.sheet_open) { do_row(c); return; }
    if (roll_focus(c.pattern(), c.ui) < 0) { c.notice("No note here"); return; }
    c.ui.sheet_open = true;
    c.ui.sheet_row = 0;
}

void soft(Context& c, int key) {
    if (c.ui.sheet_open) { // CLOSE, -, DO IT
        if (key == 0) c.ui.sheet_open = false;
        if (key == 2) do_row(c);
        return;
    }
    static const PageId pages[] {PageId::Steps, PageId::Roll, PageId::Motion};
    c.open(pages[key % 3]);
}

void pitch(Context& c, int way) {
    if (c.ui.sheet_open) { wheel(c, way); return; }
    roll_pitch(c.pattern(), c.ui, way);
}
} // namespace

const PageSpec roll_page {draw, pad, wheel, push, soft, pitch};

} // namespace melodee
