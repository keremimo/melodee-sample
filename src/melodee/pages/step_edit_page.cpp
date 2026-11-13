// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STEP EDIT (Shift + Seq, as printed; round 2, step-edit2 C "step entry"):
// the thing every step sequencer has, entering a pattern with the transport
// stopped. A caret stands on a step of Q (shared with Steps and Roll). Play a
// pad or a chord and it is written there, heard as it goes in, and when the
// pads let go the caret moves on; B2 REST moves it on without writing, B3
// ERASE steps back and takes that step out, the wheel moves it without
// writing. K1-K3 are what a pad writes: its length in steps, its velocity
// (Played: as hit) and chance; Shift + K1 the pattern's length, Shift + K2 Q,
// Shift + K3 the time signature. Minus / Plus the octave (a kit's bank).
// Steps toggles, Roll places, Step Edit types; exact timing is Roll's (Shift +
// K1 NUDGE). B1 BACK, Push, Seq or Shift + Seq again go back to the editor
// (shell/seq_layer.h); Erase + pad erases all of that pad's events once B3
// says so. Picture: sixteen steps round the caret, each step's notes as bars
// (lower keys lower, taller for harder), the caret amber, and a panel with the
// pads held (else what the step holds) and the step the caret goes to.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/page_stack.h"
#include "melodee/shell/step_edit.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace melodee {
namespace {
using namespace ui;

// What a key is called on the track: on a kit its pad and the pad's name ("A01 Kick"), else its note.
juce::String key_name(const Snapshot& s, int key, bool with_name) {
    const auto& t = s.track();
    juce::String label = key_label(t, key);
    if (with_name && t.engine == EngineKind::Sampler && key >= int(sampler_first_note) && key <= int(sampler_last_key)) {
        unsigned kit = unsigned(key) - sampler_first_note;
        if (t.pads[kit % sampler_pads].used()) label += " " + pad_name(s, kit % sampler_pads, true);
    }
    return label;
}

void draw(Paint& p, const Snapshot& s) {
    const auto& pattern = s.pattern;
    auto c = colour_of(s);
    unsigned steps = pattern.steps(), q = pattern.q(), caret = unsigned(std::max<int>(s.ui.step_cursor, 0)) % steps;
    Header h = header_of(s);
    h.left.push_back(chip("STEP " + juce::String(caret + 1), colour::amber));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{"", "REST", "ERASE"}, -1, c}; // Shift + Seq (or Seq, or Push) closes (T-a)
    keys.glyphs = {Icon::None, Icon::ArrowRight, Icon::Trash};
    keys.tints[2] = colour::rec;
    frame(p, s, keys, h);

    // Sixteen steps round the caret, in fours; each step's notes as bars, lower keys lower, taller for harder.
    unsigned first = caret / 16 * 16;
    constexpr float top = zone::picture_y + 10, col_h = 62, col_w = 16.5f;
    for (unsigned i = 0; i < 16 && first + i < steps; ++i) {
        unsigned step = first + i;
        float x = zone::pad + i * 19.1f + std::floor(i / 4.0f) * 2.6f;
        bool here = step == caret;
        p.fill(x, top, col_w, col_h, here ? colour::amber.withAlpha(0.22f) : colour::quiet, 2);
        std::vector<std::pair<int, int>> notes; // key, velocity: one a key, the loudest
        for (size_t n = pattern.lower(step * q); n < pattern.events.size() && pattern.events[n].tick < (step + 1) * q; ++n) {
            const auto& e = pattern.events[n];
            auto found = std::find_if(notes.begin(), notes.end(), [&](const std::pair<int, int>& k) { return k.first == e.key; });
            if (found == notes.end()) notes.push_back({e.key, e.velocity});
            else found->second = std::max(found->second, int(e.velocity));
        }
        std::sort(notes.begin(), notes.end());
        for (unsigned r = 0; r < notes.size() && r < 4; ++r) {
            float v = notes[r].second / 127.0f, bh = 5 + 6 * v;
            p.fill(x + 1.5f, top + col_h - (r + 1) * 14 + 2 + (11 - bh), 13.5f, bh, c, 2);
        }
        auto ink = here ? colour::amber : step < caret ? colour::sec : colour::dim;
        p.text(juce::String(step + 1), x - 4, top + col_h + 6, col_w + 8, 13,
               plain(ink, type::small, here || i % 4 == 0 ? Weight::Bold : Weight::Medium), juce::Justification::centred);
        if (here) { // the caret's mark under its column
            juce::Path mark;
            mark.addTriangle(x + 3.2f, top + col_h + 6, x + 8.2f, top + col_h + 1, x + 13.2f, top + col_h + 6);
            p.g.setColour(colour::amber);
            p.g.fillPath(mark);
        }
    }

    // The panel: the pads held (bright), else what the caret's step holds (quiet); and where the caret goes next.
    float py = top + col_h + 26, ph = zone::knobs_y - 4 - py, px = zone::pad, pw = zone::width - 2 * zone::pad;
    p.fill(px, py, pw, ph, colour::panel, 4);
    std::vector<int> keys_shown;
    bool held = s.ui.entry_held != 0;
    if (held) {
        for (unsigned pad = 0; pad < 16; ++pad)
            if (s.ui.entry_held & (1u << pad)) keys_shown.push_back(s.held_keys[pad]);
    } else {
        for (size_t n = pattern.lower(caret * q); n < pattern.events.size() && pattern.events[n].tick < (caret + 1) * q; ++n)
            if (std::find(keys_shown.begin(), keys_shown.end(), int(pattern.events[n].key)) == keys_shown.end()) keys_shown.push_back(pattern.events[n].key);
    }
    float ix = px + 10, cy = py + ph / 2;
    for (unsigned k = 0; k < keys_shown.size() && k < 3; ++k) {
        if (keys_shown[k] < 0) continue;
        auto name = key_name(s, keys_shown[k], true);
        auto style = plain(held ? colour::text : colour::sec, type::body, Weight::SemiBold);
        float tw = std::min(p.width(name, style), 82.0f);
        p.fill(ix - 4, cy - 9, tw + 8, 18, held ? c.withAlpha(0.25f) : colour::raised, 9);
        p.text(name, ix, cy - 9, tw, 18, p.fit(name, style, 82, type::floor));
        ix += tw + 14;
    }
    if (keys_shown.size() > 3) p.text("+" + juce::String(int(keys_shown.size()) - 3), ix, cy - 9, 30, 18, plain(colour::mid, type::body));
    if (keys_shown.empty()) p.text("Rest", ix, cy - 9, 80, 18, plain(colour::dim, type::body));
    juce::String next = juce::String((caret + 1) % steps + 1);
    auto next_style = plain(colour::amber, type::body, Weight::Bold);
    float nw = p.width(next, next_style) + 14;
    p.fill(px + pw - 8 - nw, cy - 9, nw, 18, colour::raised, 9);
    p.text(next, px + pw - 8 - nw, cy - 9, nw, 18, next_style, juce::Justification::centred);
    icon(p, Icon::ArrowRight, px + pw - 8 - nw - 18, cy - 6, 12, colour::amber, 1.6f);
}

void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    int key = pad_key(c.project, c.track(), pad);
    if (down && c.ui.erase) { // Erase + pad: all of that pad's events, once B3 says so
        if (key >= 0) ask(c, UiState::Ask::EraseKeyEvents, key);
        return;
    }
    entry_pad(c, pad, velocity, down);
}

void wheel(Context& c, int delta) { entry_move(c, delta); }

// Back to the editor Step Edit sits under (Steps, Roll or Motion: the track's last).
void close(Context& c) {
    auto engine = c.track().engine;
    c.open(seq_editor_page(c.ui, engine == EngineKind::Sampler));
}

void push(Context& c) { close(c); }

void soft(Context& c, int key) {
    if (key == 1) entry_rest(c);
    else if (key == 2) entry_back(c);
}

// Minus / Plus: the octave the pads play (a kit's bank).
void nudge(Context& c, int way) {
    auto& t = c.track();
    if (t.engine == EngineKind::Sampler) t.bank = uint8_t((t.bank + sampler_banks + way) % sampler_banks);
    else octave(c, way);
}
} // namespace

const PageSpec step_edit_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
