// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// THE NAME SHEET: typing a name, over any page. The pads type like phone
// keys (two letters a pad, a second tap within 0.6 s for the other; pad 14
// space, pad 15 digits and signs, pad 16 delete), the wheel moves the
// cursor, Shift + wheel or K2 turns the character under it, Erase deletes
// (with Shift, everything), K1 fills in a ready name. B1 cancels, B2 is
// Aa, B3 or Push keeps it. Opens for Save and Rename on Project, Shift +
// Push on a Song marker, and Save sound (Shift + Sample Select).
#include "melodee/engine/engine_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/page.h"

namespace melodee {
using namespace ui;

void draw_name_sheet(Paint& p, const Snapshot& s) {
    const auto& e = s.ui.name;
    bool sound = e.target == NameTarget::Sound;
    auto accent = sound ? colour_of(s) : colour::text;
    p.fill(0, 0, zone::width, zone::knobs_y, colour::bg);
    soft_keys(p, {{"CANCEL", e.caps_only ? "" : "Aa", "DONE"}, e.lower ? 1 : 2, accent, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});

    juce::String what;
    switch (e.target) {
    case NameTarget::ProjectSave: what = "SAVE PROJECT AS"; break;
    case NameTarget::ProjectSaveAs: what = "SAVE AS NEW"; break;
    case NameTarget::ProjectRename: what = "RENAME PROJECT"; break;
    case NameTarget::Marker: what = "MARKER " + juce::String::fromUTF8("\xc2\xb7 BAR ") + juce::String(s.ui.song_cursor + 1); break;
    case NameTarget::Sound: what = "SAVE SOUND " + juce::String::fromUTF8("\xc2\xb7 ") + engine_info(s.track().engine).name; break;
    case NameTarget::None: break;
    }
    Header h = header_of(s);
    h.left.clear(); h.right.clear();
    h.left.push_back(pill(what, accent));
    h.right.push_back(pill(juce::String(e.length) + " / " + juce::String(e.limit)));
    header(p, h);

    // The field, with the cursor.
    float fx = zone::pad, fy = zone::picture_y + 1, fw = zone::width - 2 * zone::pad, fh = 30;
    p.fill(fx, fy, fw, fh, colour::surf, 5);
    p.ring(fx, fy, fw, fh, colour::line, 5);
    juce::String text = juce::String::fromUTF8(e.text, int(e.length));
    auto style = p.fit(text.isEmpty() ? "M" : text, plain(colour::text, 18, Weight::SemiBold), fw - 20, 10);
    float tx = fx + 9, ty = fy + 3;
    p.text(text, tx, ty, fw - 18, 24, style);
    float cx = tx + p.width(text.substring(0, e.cursor), style);
    p.fill(cx, fy + 6, 1.5f, fh - 12, accent);
    if (text.isEmpty()) p.text("Type on the pads", tx + 6, ty, fw - 24, 24, plain(colour::dim, 12));

    // The pads as they sit: what each types, the one just hit lit.
    constexpr float gap = 2, top = zone::picture_y + 35;
    float cw = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::knobs_y - 2 - top - 3 * gap) / 4;
    bool fresh = e.last_pad >= 0 && s.now - e.last_at < NameEntry::retap_ms;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (cw + gap), y = top + row * (ch + gap);
        bool lit = fresh && e.last_pad == int(n);
        p.fill(x, y, cw, ch, lit ? accent : colour::raised, 4);
        juce::String label;
        if (n < 13) {
            for (unsigned k = 0; k < e.pad_choices(n); ++k) label += juce::String::charToString(juce::juce_wchar(e.pad_char(n, k)));
        } else {
            label = n == 13 ? "SPACE" : n == 14 ? (e.digits ? "ABC" : "123") : "DEL";
        }
        auto ls = n < 13 ? plain(lit ? colour::bg : colour::text, 12, Weight::SemiBold) : caps_text(lit ? colour::bg : colour::mid, 7);
        p.text(label, x, y, cw, ch, ls, juce::Justification::centred);
        if (lit && n < 13 && e.pad_choices(n) > 1) // which of its letters is in
            p.fill(x + cw / 2 - 7 + 8 * e.taps, y + ch - 4, 6, 1.5f, colour::bg);
    }
}

} // namespace melodee
