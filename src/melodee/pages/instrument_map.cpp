// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// What stands in for the waveform on a pad holding an SFZ: which keys it has at
// which velocities, how many zones and files, what is still being read and
// what the file asks for that is not played.
#include "melodee/pages/common.h"

namespace melodee {
using namespace ui;

void instrument_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    auto c = colour_of(s);
    const auto& m = s.sample;
    const float line = 16, map_h = std::max(12.0f, h - 2 * line - 4);
    p.fill(x, y, w, map_h, colour::surf, 3);
    const float cw = w / 128, ch = map_h / 16;
    for (unsigned key = 0; key < 128; ++key) {
        if (key % 12 == 0) p.fill(x + key * cw, y, 1, map_h, colour::line, 0);
        unsigned bits = m.map[key];
        for (unsigned band = 0; band < 16;) {
            if (!((bits >> band) & 1)) { ++band; continue; }
            unsigned top = band;
            while (top + 1 < 16 && ((bits >> (top + 1)) & 1)) ++top;
            p.fill(x + key * cw, y + map_h - (top + 1) * ch, std::max(cw, 1.0f), (top - band + 1) * ch, c.withAlpha(0.85f), 0);
            band = top + 1;
        }
    }
    const float y1 = y + map_h + 3, y2 = y1 + line;
    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
    juce::String what = juce::String(m.zones) + (m.zones == 1 ? " zone" : " zones") + dot + juce::String(m.parts) + (m.parts == 1 ? " file" : " files");
    p.text(what, x, y1, w, line, plain(colour::text, type::small, Weight::Medium));
    bool reading = m.parts_ready + m.parts_failed < m.parts;
    juce::String note;
    if (reading) note = "Reading its samples " + juce::String(m.parts_ready) + " / " + juce::String(m.parts);
    else if (!m.error.empty()) note = juce::String(m.error) + dot + juce::String(m.parts_failed) + " silent";
    else if (!m.ignored.empty()) note = "Not played: " + juce::String(m.ignored);
    else if (m.left) note = juce::String(m.left) + " regions left out";
    if (note.isNotEmpty()) {
        auto style = p.fit(note, plain(colour::sec, type::small, Weight::Medium), w, type::floor);
        p.text(note, x, y2, w, line, style);
    }
}

} // namespace melodee
