// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SETTINGS (Shift + Pad 8): Melodee's own settings, kept on the card next
//   to the projects, as seven doors (four by two): PADS (response, pressure,
//   lights, what Double Seq puts in the new half), KNOBS (how the knobs,
//   Knob FX and the fader take over a value: Pickup, Scaled, Instant),
//   SCREEN (palette, brightness, where the place bar sits), AUDIO (the
//   sample rate and the buffer: longer rides out a slow moment without a
//   crackle, shorter is heard sooner), SAMPLING (how Audio Recall cuts the
//   ring and when a Free take ends on its own), MIDI (MIDI Config's page)
//   and ABOUT. The wheel picks a door, Push or B3 opens it, B1 goes back.
//   Behind a door its rows, one screen each: the wheel moves, Push opens a
//   value and the wheel changes it, Push keeps it; Minus / Plus change the
//   one under the cursor straight away; B1 back to the doors. The header
//   says what the audio device runs at, and its dropouts (amber) if any.
// ABOUT (the seventh door): what this is and what it is made of.
#include "melodee/engine/effect_info.h"
#include "melodee/engine/punch.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;

struct Door { Icon icon; const char* name; std::array<int8_t, 6> rows; };
constexpr int8_t none = -1;
const std::array<Door, 7> doors {{
    {Icon::Pads, "PADS", {Settings::PadResponse, Settings::PadPressure, Settings::PadLights, Settings::DoubleSeq, none, none}},
    {Icon::Knob, "KNOBS", {Settings::KnobTakeover, Settings::KnobFxTakeover, Settings::FaderTakeover, none, none, none}},
    {Icon::Screen, "SCREEN", {Settings::Palette, Settings::Screen, Settings::PlaceBarRow, none, none, none}},
    {Icon::Speaker, "AUDIO", {Settings::SampleRate, Settings::AudioBuffer, none, none, none, none}},
    {Icon::Mic, "SAMPLING", {Settings::RecallGrabRow, Settings::RecallGap, Settings::RecallTrimEnd, Settings::TakeAutoEnd, Settings::AutoEndAfter,
                             Settings::SilenceBelow}},
    {Icon::Din, "MIDI", {none, none, none, none, none, none}},  // MIDI Config's own page
    {Icon::Info, "ABOUT", {none, none, none, none, none, none}}, // the About page
}};
constexpr unsigned midi_door = 5, about_door = 6;
unsigned door_rows(const Door& d) { unsigned n = 0; while (n < d.rows.size() && d.rows[n] >= 0) ++n; return n; }
const Door& door_of(const UiState& ui) { return doors[ui.settings_door % doors.size()]; }
// Where the row in hand sits behind its door (0 when it is not one of them).
unsigned place_of(const UiState& ui) {
    const auto& d = door_of(ui);
    for (unsigned n = 0; n < door_rows(d); ++n) if (unsigned(d.rows[n]) == ui.settings_cursor) return n;
    return 0;
}

Header settings_header(const Snapshot& s) {
    Header h = header_of(s);
    juce::String rate = s.audio_rate == 44100 ? "44.1" : juce::String(s.audio_rate / 1000);
    unsigned frames = 128 * s.audio_periods;
    h.right.push_back(pill(rate + " kHz" + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(1000.0 * frames / std::max(1u, s.audio_rate), 1) + " ms"));
    if (s.audio_xruns) h.right.push_back(pill(Icon::Warn, juce::String(s.audio_xruns), colour::amber)); // dropouts so far
    return h;
}

void doors_draw(Paint& p, const Snapshot& s) {
    frame(p, s, {{"BACK", "", "OPEN"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}}, settings_header(s));
    constexpr float gap = 4, w = (zone::width - 2 * zone::pad - 3 * gap) / 4, h = 66, top = zone::picture_y + 4;
    for (unsigned n = 0; n < doors.size(); ++n) {
        float x = zone::pad + (n % 4) * (w + gap), y = top + (n / 4) * (h + gap);
        bool on = n == s.ui.settings_door % doors.size();
        p.fill(x, y, w, h, on ? colour::raised : colour::surf, 5);
        if (on) p.fill(x, y, w, 2.5f, colour::text, 1);
        auto c = on ? colour::text : colour::mid;
        icon(p, doors[n].icon, x + w / 2 - 11, y + 12, 22, c, 1.5f);
        p.text(doors[n].name, x, y + 42, w, 14, caps_text(c), juce::Justification::centred);
    }
}

void rows_draw(Paint& p, const Snapshot& s) {
    const auto& d = door_of(s.ui);
    Header h = settings_header(s);
    h.left.push_back(chip(d.icon, d.name, colour::text));
    frame(p, s, {{"BACK", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    const auto& set = s.ui.settings;
    float x = zone::pad, w = zone::width - 2 * zone::pad;
    for (unsigned n = 0; n < door_rows(d); ++n) {
        unsigned row = unsigned(d.rows[n]);
        float y = zone::picture_y + 1 + n * (list_row_h + list_row_gap);
        bool cursor = row == s.ui.settings_cursor, editing = cursor && s.ui.settings_editing;
        if (cursor) { p.fill(x, y, w, list_row_h, colour::raised, 3); p.fill(x, y, 2.5f, list_row_h, colour::text, 1); }
        p.text(Settings::row_name(row), x + 10, y, w / 2, list_row_h, plain(cursor ? colour::text : colour::sec, type::list, cursor ? Weight::Medium : Weight::Regular));
        juce::String value = set.value_text(row);
        auto style = plain(cursor ? colour::text : colour::mid, type::body, Weight::Medium);
        float vw = p.width(value, style), vx = x + w - 9 - vw;
        if (editing) { // the open value: framed
            p.fill(vx - 6, y + 3, vw + 12, list_row_h - 6, colour::surf, 3);
            p.ring(vx - 6, y + 3, vw + 12, list_row_h - 6, colour::text, 3, 1);
        }
        p.text(value, vx, y, vw + 1, list_row_h, style);
        if (row == Settings::Palette) // the palette's first track colours
            for (unsigned t = 0; t < 4; ++t) p.fill(vx - 10 - (4 - t) * 9.5f, y + 8, 7, 7, track_colour(t), 2);
    }
}

void settings_draw(Paint& p, const Snapshot& s) {
    if (s.ui.settings_in && door_rows(door_of(s.ui))) rows_draw(p, s);
    else doors_draw(p, s);
}

void open_door(Context& c) {
    auto& ui = c.ui;
    unsigned n = ui.settings_door % doors.size();
    ui.settings_editing = false;
    if (n == midi_door) { ui.midi_editing = false; c.open(PageId::MidiConfig); return; }
    if (n == about_door) { c.open(PageId::About); return; }
    ui.settings_in = true;
    ui.settings_cursor = uint8_t(doors[n].rows[0]);
}

void settings_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (!ui.settings_in) { ui.settings_door = uint8_t(std::clamp(int(ui.settings_door % doors.size()) + delta, 0, int(doors.size()) - 1)); return; }
    if (ui.settings_editing) { c.change_setting(ui.settings_cursor, delta > 0 ? 1 : -1); return; }
    const auto& d = door_of(ui);
    unsigned at = unsigned(std::clamp(int(place_of(ui)) + delta, 0, int(door_rows(d)) - 1));
    ui.settings_cursor = uint8_t(d.rows[at]);
}
void settings_push(Context& c) {
    if (!c.ui.settings_in) { open_door(c); return; }
    c.ui.settings_editing = !c.ui.settings_editing && c.ui.settings_cursor != Settings::Palette;
}
void settings_nudge(Context& c, int way) {
    if (!c.ui.settings_in) { settings_wheel(c, way); return; }
    c.change_setting(c.ui.settings_cursor, way);
}
void settings_soft(Context& c, int key) {
    auto& ui = c.ui;
    if (ui.settings_in) { if (key == 0) { ui.settings_in = false; ui.settings_editing = false; } return; }
    if (key == 0) c.back();
    if (key == 2) open_door(c);
}

void about_draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.right.push_back(label(s.project.name));
    frame(p, s, {{"BACK", "", ""}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}}, h);
    float x = zone::pad + 6, w = zone::width - 2 * zone::pad - 12, y = zone::picture_y + 4;
    p.text("Melodee", x, y, w, 30, plain(colour::text, 26, Weight::SemiBold));
    p.text("for the MPC Sample, written on sample-base", x, y + 30, w, 14, plain(colour::sec, type::small));
    unsigned effects = effect_count(); // as the effect picker counts them: not None, not the plug-in slot
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

const PageSpec settings_page {settings_draw, play_pad, settings_wheel, settings_push, settings_soft, settings_nudge};
const PageSpec about_page {about_draw, play_pad, no_wheel, no_push, [](Context& c, int key) { if (key == 0) c.back(); }, [](Context&, int) {}};

} // namespace melodee
