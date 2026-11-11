// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SETTINGS (B2 on Project): Melodee's own settings, kept on the card next
//   to the projects. The wheel moves; Push opens a value, the wheel changes
//   it, Push keeps it. Minus / Plus change the value under the cursor
//   straight away. Palette (NIGHT, the one there is), pad response (soft,
//   medium, hard, full level), pad pressure (on, off), pad lights (track
//   colours, white, off), how the knobs, Knob FX and the fader take over a
//   value (Pickup, Scaled, Instant; the MPC keeps these on MIDI Config,
//   phase 9 shows them there too), what Double Seq puts in the new half
//   (Copy notes, as the MPC does, or Empty), screen brightness, the sample rate
//   (44.1, 48, 96 kHz) and the audio buffer (256 to 1024 frames: longer
//   rides out a slow moment without a crackle, shorter is heard sooner),
//   then sampling: how Audio Recall cuts the ring and when a Free take of
//   Melodee's own sound (or any source) ends on its own.
//   Seven rows show at a time, scrolled with the cursor; the line under
//   them says what the audio device runs at now and its dropouts so far
//   (a change takes a moment and stops playback).
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
    Header header = header_of(s);
    header.right.push_back(label("ALL PROJECTS")); // not the open project's: they stay when another loads
    frame(p, s, {{"PROJECT", "SETTINGS", "ABOUT"}, 1, colour::text}, header);
    const auto& set = s.ui.settings;
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 0.5f;
    constexpr float rh = 18;      // seven rows and the audio line in the picture's 148 px
    constexpr unsigned shown = 7;
    unsigned first = unsigned(std::clamp(int(s.ui.settings_cursor) - int(shown) + 2, 0, int(Settings::Rows - shown)));
    for (unsigned row = first; row < first + shown; ++row, y += rh) {
        bool cursor = row == s.ui.settings_cursor, editing = cursor && s.ui.settings_editing;
        if (cursor) { p.fill(x, y, w, rh - 1, colour::raised, 4); p.fill(x, y, 3, rh - 1, colour::text); }
        p.text(Settings::row_name(row), x + 9, y, w / 2, rh - 1, plain(cursor ? colour::text : colour::sec, type::body, Weight::Medium));
        juce::String value = set.value_text(row);
        auto style = plain(cursor ? colour::text : colour::mid, type::small, Weight::Medium);
        float vw = p.width(value, style), vx = x + w - 7 - vw;
        if (editing) { // the open value: framed, with which way the wheel goes
            p.fill(vx - 6, y + 2, vw + 12, rh - 5, colour::surf, 3);
            p.ring(vx - 6, y + 2, vw + 12, rh - 5, colour::text, 3, 1);
        }
        p.text(value, vx, y, vw, rh - 1, style);
        if (row == Settings::Palette && cursor) // the palette's first track colours
            for (unsigned t = 0; t < 4; ++t) p.fill(vx - 10 - (4 - t) * 9.5f, y + 5, 7, 7, track_colour(t), 2);
    }
    // What runs now: a change shows here once the device has reopened.
    juce::String rate = s.audio_rate == 44100 ? "44.1" : juce::String(s.audio_rate / 1000);
    unsigned frames = 128 * s.audio_periods;
    juce::String now = "Running " + rate + " kHz, " + juce::String(frames) + " frames, " +
                       juce::String(1000.0 * frames / std::max(1u, s.audio_rate), 1) + " ms, " +
                       juce::String(s.audio_xruns) + (s.audio_xruns == 1 ? " dropout" : " dropouts");
    p.text(now, x + 9, y + 2, w - 18, 15, plain(s.audio_xruns ? colour::text : colour::mid, type::small, Weight::Medium));
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
    p.text("for the MPC Sample, written on sample-base", x, y + 30, w, 14, plain(colour::sec, type::small));
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
        p.text(row.first, x, ry, 64, 13, caps_text(colour::mid));
        p.text(row.second, x + 66, ry, w - 66, 13, p.fit(row.second, plain(colour::text, type::small, Weight::Medium), w - 66, type::floor));
        ry += 14.5f;
    }
}
} // namespace

const PageSpec settings_page {settings_draw, play_pad, settings_wheel, settings_push, system_soft, settings_nudge};
const PageSpec about_page {about_draw, play_pad, no_wheel, no_push, system_soft, [](Context&, int) {}};

} // namespace melodee
