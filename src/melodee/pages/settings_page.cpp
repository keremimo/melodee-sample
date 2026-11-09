// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SETTINGS (B2 on Project): Melodee's own settings, kept on the card next
//   to the projects. The wheel moves; Push opens a value, the wheel changes
//   it, Push keeps it. Minus / Plus change the value under the cursor
//   straight away. Palette (NIGHT, the one there is), pad response (soft,
//   medium, hard, fixed), pad lights (track colours, white, off), knob
//   pickup, screen brightness.
// ABOUT (B3): what this is and what it is made of.
#include "melodee/engine/effect_info.h"
#include "melodee/engine/punch.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"

namespace melodee {
namespace {
using namespace ui;

Header system_header(const Snapshot& s) {
    Header h = header_of(s);
    h.right.push_back(label(s.project.name));
    return h;
}

void settings_draw(Paint& p, const Snapshot& s) {
    frame(p, s, {{"PROJECT", "SETTINGS", "ABOUT"}, 1, colour::text}, system_header(s));
    const auto& set = s.ui.settings;
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 0.5f;
    for (unsigned row = 0; row < Settings::Rows; ++row, y += 21) {
        bool cursor = row == s.ui.settings_cursor, editing = cursor && s.ui.settings_editing;
        if (cursor) { p.fill(x, y, w, 20, colour::raised, 4); p.fill(x, y, 3, 20, colour::text); }
        p.text(Settings::row_name(row), x + 9, y, w / 2, 20, plain(cursor ? colour::text : colour::sec, 12, Weight::Medium));
        juce::String value = set.value_text(row);
        auto style = plain(cursor ? colour::text : colour::mid, 11, Weight::Medium);
        float vw = p.width(value, style), vx = x + w - 7 - vw;
        if (editing) { // the open value: framed, with which way the wheel goes
            p.fill(vx - 6, y + 3, vw + 12, 14, colour::surf, 3);
            p.ring(vx - 6, y + 3, vw + 12, 14, colour::text, 3, 1);
        }
        p.text(value, vx, y, vw, 20, style);
        if (row == Settings::Palette && cursor) // the palette's first track colours
            for (unsigned t = 0; t < 4; ++t) p.fill(vx - 10 - (4 - t) * 9.5f, y + 6.5f, 7, 7, track_colour(t), 2);
    }
}
void settings_wheel(Context& c, int delta) {
    if (c.ui.settings_editing) { c.change_setting(c.ui.settings_cursor, delta > 0 ? 1 : -1); return; }
    c.ui.settings_cursor = uint8_t(std::clamp(int(c.ui.settings_cursor) + delta, 0, int(Settings::Rows) - 1));
}
void settings_push(Context& c) { c.ui.settings_editing = !c.ui.settings_editing && c.ui.settings_cursor != Settings::Palette; }
void settings_nudge(Context& c, int way) { c.change_setting(c.ui.settings_cursor, way); }
void system_soft(Context& c, int key) {
    c.ui.settings_editing = false;
    c.open(key == 0 ? PageId::Project : key == 1 ? PageId::SettingsPage : PageId::About);
}

void about_draw(Paint& p, const Snapshot& s) {
    frame(p, s, {{"PROJECT", "SETTINGS", "ABOUT"}, 2, colour::text}, system_header(s));
    float x = zone::pad + 6, w = zone::width - 2 * zone::pad - 12, y = zone::picture_y + 4;
    p.text("Melodee", x, y, w, 30, plain(colour::text, 26, Weight::SemiBold));
    p.text("for the MPC Sample, written on sample-base", x, y + 30, w, 14, plain(colour::sec, 10));
    unsigned effects = unsigned(EffectKind::Count) - 1;
    juce::String juce_version = juce::String(JUCE_MAJOR_VERSION) + "." + juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER);
    const std::pair<const char*, juce::String> rows[] {
        {"ENGINES", "Prophet, FM6, CZ, SID, 909, Sampler"},
        {"EFFECTS", juce::String(effects) + " effects, " + juce::String(punch_count) + " punches"},
        {"TRACKS", juce::String(track_count) + " tracks, " + juce::String(pattern_count) + " patterns each"},
        {"BUILT ON", "sample-base, JUCE " + juce_version},
        {"TYPE", "Rubik, SIL Open Font Licence"},
        {"LICENCE", "AGPL-3.0"},
    };
    float ry = y + 54;
    for (const auto& row : rows) {
        p.text(row.first, x, ry, 64, 13, caps_text(colour::mid, 7));
        p.text(row.second, x + 66, ry, w - 66, 13, p.fit(row.second, plain(colour::text, 10, Weight::Medium), w - 66, 7));
        ry += 14.5f;
    }
}
} // namespace

const PageSpec settings_page {settings_draw, play_pad, settings_wheel, settings_push, system_soft, settings_nudge};
const PageSpec about_page {about_draw, play_pad, no_wheel, no_push, system_soft, [](Context&, int) {}};

} // namespace melodee
