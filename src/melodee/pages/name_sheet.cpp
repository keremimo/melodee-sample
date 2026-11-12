// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// THE NAME SHEET: typing a name, over any page. The pads type like phone
// keys (two letters a pad, a second tap within 0.6 s for the other; pad 14
// space, pad 15 digits and signs, pad 16 delete): each pad shows both, the
// one a tap gives now bright, swapping as you tap. The wheel moves the
// cursor, Shift + wheel or K2 turns the character under it, Erase deletes
// (with Shift, everything), K1 fills in a ready name; a thin gauge under the
// name is how much of its room it takes. B1 cancels, B2 is Aa, B3 SAVE or
// Push keeps it. Opens for Save and Rename on Project, Shift +
// Push on a Song marker, Save sound, Save Sample and Save Bank (Shift +
// Sample Select).
#include "melodee/engine/engine_info.h"
#include "melodee/pages/common.h"
#include "melodee/pages/page.h"

namespace melodee {
using namespace ui;

void draw_name_sheet(Paint& p, const Snapshot& s) {
    const auto& e = s.ui.name;
    bool sound = e.target == NameTarget::Sound || e.target == NameTarget::Sample || e.target == NameTarget::Bank;
    auto accent = sound ? colour_of(s) : colour::text;
    p.fill(0, 0, zone::width, zone::knobs_y, colour::bg);
    soft_keys(p, {{"CANCEL", e.caps_only ? "" : "Aa", "SAVE"}, e.lower ? 1 : 2, accent, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});

    juce::String what;
    switch (e.target) {
    case NameTarget::ProjectSave: what = "SAVE PROJECT AS"; break;
    case NameTarget::ProjectSaveAs: what = "SAVE AS NEW"; break;
    case NameTarget::ProjectRename: what = "RENAME PROJECT"; break;
    case NameTarget::Marker: what = "MARKER " + juce::String::fromUTF8("\xc2\xb7 BAR ") + juce::String(s.ui.song_cursor + 1); break;
    case NameTarget::Sound: what = "SAVE SOUND " + juce::String::fromUTF8("\xc2\xb7 ") + engine_info(s.track().engine).name; break;
    case NameTarget::Sample: what = "SAVE SAMPLE"; break;
    case NameTarget::Bank: what = "SAVE BANK " + juce::String::fromUTF8("\xc2\xb7 ") + juce::String::charToString(juce::juce_wchar('A' + s.track().bank % sampler_banks)); break;
    case NameTarget::Mixdown: what = "AUDIO MIXDOWN"; break;
    case NameTarget::Scene: what = "SCENE " + juce::String(s.ui.scene % pattern_count + 1); break; // roadmap P5
    case NameTarget::None: break;
    }
    Header h = header_of(s);
    h.left.clear(); h.right.clear();
    h.left.push_back(pill(what, accent));
    header(p, h);

    // The field with the cursor, and how much of its room the name takes as a thin gauge under it.
    float fx = zone::pad, fy = zone::picture_y + 4, fw = zone::width - 2 * zone::pad, fh = 28;
    p.fill(fx, fy, fw, fh, colour::panel, 4);
    juce::String text = juce::String::fromUTF8(e.text, int(e.length));
    auto style = p.fit(text.isEmpty() ? "M" : text, plain(colour::text, type::title, Weight::SemiBold), fw - 20, type::floor);
    float tx = fx + 8, ty = fy + 3;
    p.text(text, tx, ty, fw - 16, 22, style);
    float cx = tx + p.width(text.substring(0, e.cursor), style);
    p.fill(cx + 1, fy + 5, 2, fh - 10, accent);
    p.fill(fx, fy + fh + 3, fw, 2, colour::line, 1);
    p.fill(fx, fy + fh + 3, fw * e.length / std::max<float>(1, e.limit), 2, colour::sec, 1);

    // The pads as they sit: both letters, the one a tap gives now bright; the pad just hit lit.
    constexpr float gx = 4, gy = 3, top = zone::picture_y + 42;
    float cw = (zone::width - 2 * zone::pad - 3 * gx) / 4, ch = (zone::knobs_y - 1 - top - 3 * gy) / 4;
    bool fresh = e.last_pad >= 0 && s.now - e.last_at < NameEntry::retap_ms;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        float x = zone::pad + column * (cw + gx), y = top + row * (ch + gy), mid = y + ch / 2;
        bool lit = fresh && e.last_pad == int(n);
        p.fill(x, y, cw, ch, lit ? accent : colour::surf, 3);
        auto ink = lit ? colour::bg : colour::text, faint = lit ? colour::bg.withAlpha(0.5f) : colour::mid;
        if (n < 13) {
            unsigned choices = e.pad_choices(n), next = lit && choices > 1 ? (e.taps + 1u) % choices : 0;
            auto letter = [&](unsigned k) { return juce::String::charToString(juce::juce_wchar(e.pad_char(n, k))); };
            if (choices < 2) { p.text(letter(0), x, y, cw, ch, plain(ink, type::list, Weight::SemiBold), juce::Justification::centred); continue; }
            for (unsigned k = 0; k < 2; ++k)
                p.text(letter(k), x + cw / 2 - 18 + k * 18, y, 18, ch, plain(k == next ? ink : faint, type::list, k == next ? Weight::SemiBold : Weight::Medium),
                       juce::Justification::centred);
        } else if (n == 13) {
            icon(p, Icon::SpaceBar, x + cw / 2 - 9, mid - 9, 18, lit ? ink : colour::sec, 1.5f);
        } else if (n == 14) {
            p.text(e.digits ? "ABC" : "123", x, y, cw, ch, plain(lit ? ink : colour::sec, type::body, Weight::SemiBold), juce::Justification::centred);
        } else {
            icon(p, Icon::ArrowLeft, x + cw / 2 - 7, mid - 7, 14, lit ? ink : colour::sec, 1.6f);
        }
    }
}

} // namespace melodee
