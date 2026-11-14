// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SETTINGS (Shift + Pad 8): Melodee's own settings, kept on the card next
//   to the projects, as seven doors (four by two): PADS (response, pressure,
//   lights, what Double Seq puts in the new half, how many bars Capture keeps
//   and how it puts them on the grid), KNOBS (how the knobs,
//   Knob FX and the fader take over a value: Pickup, Scaled, Instant),
//   SCREEN (palette, brightness, where the place bar sits, and Screen Mirror:
//   the screen as a USB webcam for a computer, on or off, its size and rate;
//   under the rows, while it is on, how it goes: ready, live, paused while
//   the card is shared), AUDIO (the
//   sample rate and the buffer: longer rides out a slow moment without a
//   crackle, shorter is heard sooner), SAMPLING (how Audio Recall cuts the
//   ring and when a Free take ends on its own), MIDI (MIDI Config's page)
//   and ABOUT. The wheel picks a door, Push or B3 opens it, B1 goes back.
//   Behind a door its rows, one screen each: the wheel moves, Push opens a
//   value and the wheel changes it, Push keeps it; Minus / Plus change the
//   one under the cursor straight away; B1 back to the doors. The header of
//   the doors and of AUDIO says what the audio device runs at, and its
//   dropouts (amber) if any.
// ABOUT (the seventh door; round 2, about C "how the machine is doing"): the
//   page to open when something crackles. A row each: RENDER (each core's
//   load as a meter, and the audio blocks' share of their time), AUDIO (the
//   rate and the buffer, its dropouts in amber), CARD (room on it, and the
//   times it could not stream a sample in time), PLUG-INS (how many, and
//   what they cost). B3 again shows what this is and what it is made of.
#include "drivers/audio.h"
#include "drivers/screen_mirror.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/punch.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/update.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

struct Door { Icon icon; const char* name; std::array<int8_t, 6> rows; };
constexpr int8_t none = -1;
const std::array<Door, 8> doors {{
    {Icon::Pads, "PADS", {Settings::PadResponse, Settings::PadPressure, Settings::DoubleSeq, Settings::CaptureBars, Settings::CaptureQuantize,
                          Settings::RepeatVelocity}},
    {Icon::Bulb, "LIGHTS", {Settings::PadLights, Settings::PadFade, Settings::TapLight, none, none, none}},
    {Icon::Knob, "KNOBS", {Settings::KnobTakeover, Settings::KnobFxTakeover, Settings::FaderTakeover, none, none, none}},
    {Icon::Screen, "SCREEN", {Settings::Palette, Settings::Screen, Settings::PlaceBarRow, Settings::Mirror, Settings::MirrorSize,
                              Settings::MirrorRate}},
    {Icon::Speaker, "AUDIO", {Settings::SampleRate, Settings::AudioBuffer, none, none, none, none}},
    {Icon::Mic, "SAMPLING", {Settings::RecallGrabRow, Settings::RecallGap, Settings::RecallTrimEnd, Settings::TakeAutoEnd, Settings::AutoEndAfter,
                             Settings::SilenceBelow}},
    {Icon::Din, "MIDI", {none, none, none, none, none, none}},  // MIDI Config's own page
    {Icon::Info, "ABOUT", {none, none, none, none, none, none}}, // the About page
}};
constexpr unsigned screen_door = 3, audio_door = 4, midi_door = 6, about_door = 7;
void trouble(Paint& p, float right, float cy, unsigned count);

// Screen Mirror, under SCREEN's rows while it is on: how it goes (the runtime's state). Amber when it waits on the card or failed, and
// IN USE (a lock) while a change made in the rows waits for the computer to close the camera (never unplugged under
// an app showing it); the size offered only when a JPEG size fell back to 640 x 480 (no JPEG encoder), else the row
// above says it.
void mirror_line(Paint& p, const Snapshot& s, float x, float y, float w) {
    float cy = y + list_row_h / 2, right = x + w - 9;
    auto state = MirrorState(s.mirror_state);
    const char* word = "OFF";
    juce::Colour c = colour::mid;
    switch (state) {
    case MirrorState::Off: break;
    case MirrorState::Starting: word = "STARTING"; break;
    case MirrorState::Waiting: word = "READY"; c = colour::text; break;
    case MirrorState::Streaming: word = "LIVE"; c = colour::play; break;
    case MirrorState::CardShared: word = "CARD MOUNTED"; c = colour::amber; break;
    case MirrorState::Failed: word = "FAILED"; c = colour::amber; break;
    }
    if (s.mirror_held) { word = "IN USE"; c = colour::amber; }
    bool trouble_now = state == MirrorState::CardShared || state == MirrorState::Failed;
    Icon glyph = s.mirror_held ? Icon::Lock : trouble_now ? Icon::Warn : Icon::Laptop;
    icon(p, glyph, x + 10, cy - 7, 14, trouble_now || s.mirror_held ? colour::amber : colour::sec, 1.4f);
    p.text(word, x + 30, y, 120, list_row_h, caps_text(c));
    if (state == MirrorState::Off || state == MirrorState::Failed || !s.mirror_fallback) return;
    juce::String size = juce::String(s.mirror_width) + " x " + juce::String(s.mirror_height);
    auto style = plain(colour::amber, type::body, Weight::SemiBold);
    float sw = p.width(size, style);
    p.text(size, right - sw, y, sw + 1, list_row_h, style);
    icon(p, Icon::Warn, right - sw - 18, cy - 6, 12, colour::amber, 1.5f);
}

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
    bool audio = s.ui.settings_door % doors.size() == audio_door;
    Header h = audio ? settings_header(s) : header_of(s); // what the device runs at: on AUDIO, the door it belongs to
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
    if (s.ui.settings_door % doors.size() == screen_door) { // Screen Mirror, while it is on or a change waits
        if (MirrorState(s.mirror_state) != MirrorState::Off || s.mirror_held)
            mirror_line(p, s, x, zone::picture_y + 1 + door_rows(d) * (list_row_h + list_row_gap) + 6, w);
        return;
    }
    if (s.ui.settings_door % doors.size() != audio_door) return;
    // AUDIO (roadmap P2): what the blocks take of their time, beside the dropouts (a check while there are none).
    float y = zone::picture_y + 1 + door_rows(d) * (list_row_h + list_row_gap) + 6, cy = y + list_row_h / 2, right = x + w - 9;
    float load = s.machine.render_load;
    icon(p, Icon::Chip, x + 10, cy - 7, 14, colour::sec, 1.4f);
    p.text("DSP", x + 30, y, 40, list_row_h, caps_text(colour::mid));
    p.fill(x + 70, cy - 2, 120, 4, colour::line, 2);
    p.fill(x + 70, cy - 2, 120 * std::clamp(load, 0.0f, 1.0f), 4, load_colour(load), 2);
    p.text(juce::String(int(std::lround(load * 100))) + " %", x + 196, y, 44, list_row_h, plain(load_colour(load), type::body, Weight::SemiBold));
    if (s.audio_xruns) trouble(p, right, cy, s.audio_xruns);
    else icon(p, Icon::Check, right - 14, cy - 7, 14, colour::play, 1.8f);
    // Roadmap P4: what plug-in delay compensation holds everything back by, while it holds anything.
    if (!s.machine.pdc_frames) return;
    float py = y + list_row_h, pcy = py + list_row_h / 2;
    float ms = float(s.machine.pdc_frames) * 1000.0f / float(std::max(s.audio_rate, 1u));
    icon(p, Icon::Clock, x + 10, pcy - 7, 14, colour::sec, 1.4f);
    p.text("PDC", x + 30, py, 40, list_row_h, caps_text(colour::mid));
    p.text(juce::String(ms, 1) + " ms", x + 70, py, 80, list_row_h, plain(colour::text, type::body, Weight::SemiBold));
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
    if (n == about_door) { ui.about_view = 0; c.open(PageId::About); return; }
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

juce::String space_text(uint64_t bytes) { // 21.4 GB, 512 MB
    double gb = double(bytes) / 1e9;
    return gb >= 1 ? juce::String(gb, 1) + " GB" : juce::String(int(std::lround(double(bytes) / 1e6))) + " MB";
}

// An amber sign of trouble with how many times: a warning glyph and the count, its right edge at `right`.
void trouble(Paint& p, float right, float cy, unsigned count) {
    juce::String n(count);
    auto style = plain(colour::amber, type::body, Weight::SemiBold);
    float w = 22 + p.width(n, style);
    p.fill(right - w, cy - 9, w, 18, colour::raised, 9);
    icon(p, Icon::Warn, right - w + 5, cy - 6, 12, colour::amber, 1.5f);
    p.text(n, right - w + 19, cy - 9, w - 21, 18, style);
}

void machine_draw(Paint& p, const Snapshot& s) {
    const auto& m = s.machine;
    constexpr float lx = zone::pad + 6, vx = 124, right = zone::width - 12, row_h = 33;
    auto value = [&](float y, const juce::String& text, juce::Colour c) {
        p.text(text, right - 90, y, 90, 18, plain(c, type::body, Weight::SemiBold), juce::Justification::centredRight);
    };
    auto bar = [&](float x, float y, float w, float fill, juce::Colour c) {
        p.fill(x, y, w, 4, colour::line, 2);
        p.fill(x, y, w * std::clamp(fill, 0.0f, 1.0f), 4, c, 2);
    };
    const std::pair<Icon, const char*> rows[] {{Icon::Chip, "RENDER"}, {Icon::Speaker, "AUDIO"}, {Icon::Card, "CARD"}, {Icon::Plug, "PLUG-INS"}};
    for (unsigned r = 0; r < 4; ++r) {
        float y = zone::picture_y + 8 + r * row_h;
        icon(p, rows[r].first, lx, y + 3, 16, colour::sec, 1.4f);
        p.text(rows[r].second, lx + 26, y + 4, 80, 14, caps_text(colour::mid));
    }
    // RENDER: each core's load as a meter, and the audio blocks' share of their time.
    float y = zone::picture_y + 8;
    for (unsigned c = 0; c < m.cores; ++c) {
        float load = m.core_load[c] / 100.0f, x = vx + c * 12;
        p.fill(x, y, 8, 22, colour::line, 2);
        p.fill(x, y + 22 * (1 - std::clamp(load, 0.0f, 1.0f)), 8, 22 * std::clamp(load, 0.0f, 1.0f), load_colour(load), 2);
    }
    value(y + 2, juce::String(int(std::lround(m.render_load * 100))) + " %", load_colour(m.render_load));
    // AUDIO: the rate and the buffer; its dropouts in amber, a check while there are none.
    y += row_h;
    juce::String rate = juce::String(s.audio_rate / 1000.0, s.audio_rate % 1000 ? 1 : 0) + " kHz" + juce::String::fromUTF8(" \xc2\xb7 ")
                      + juce::String(audio_frames * s.audio_periods);
    p.text(rate, vx, y + 2, 120, 18, plain(colour::sec, type::body));
    if (s.audio_xruns) trouble(p, right, y + 11, s.audio_xruns);
    else icon(p, Icon::Check, right - 14, y + 4, 14, colour::play, 1.8f);
    // CARD: room on it; the times it could not stream a sample in time, in amber.
    y += row_h;
    if (!m.card || !m.card_total) {
        p.text("No card", vx, y + 2, 120, 18, plain(colour::mid, type::body));
    } else {
        float used = 1 - float(double(m.card_free) / double(m.card_total));
        bar(vx, y + 9, m.stream_late ? 84 : 120, used, colour::sec);
        if (m.stream_late) trouble(p, vx + 124, y + 11, m.stream_late);
        value(y + 2, space_text(m.card_free), colour::text);
    }
    // PLUG-INS: how many, and their share of the audio time.
    y += row_h;
    if (!m.plugins) {
        p.text("None", vx, y + 2, 120, 18, plain(colour::mid, type::body));
    } else {
        p.text(juce::String(m.plugins), vx, y + 2, 16, 18, plain(colour::sec, type::body));
        bar(vx + 18, y + 9, 102, m.plugins_load, colour::sec);
        value(y + 2, juce::String(int(std::lround(m.plugins_load * 100))) + " %", load_colour(m.plugins_load));
    }
}

void credits_draw(Paint& p) {
    float x = zone::pad + 6, w = zone::width - 2 * zone::pad - 12, y = zone::picture_y + 4;
    p.text("Melodee", x, y, w, 30, plain(colour::text, 26, Weight::SemiBold));
    juce::String version = juce::String("Version ") + melodee_version();
    p.text(version, x, y + 30, w, 14, p.fit(version, plain(colour::sec, type::small), w, type::floor));
    unsigned effects = effect_count(); // as the effect picker counts them: not None, not the plug-in slot
    juce::String juce_version = juce::String(JUCE_MAJOR_VERSION) + "." + juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER);
    const std::pair<const char*, juce::String> rows[] {
        {"ENGINES", "Sampler, plug-ins, MIDI"},
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

void about_draw(Paint& p, const Snapshot& s) {
    Header h = header_of(s);
    h.right.push_back(label(s.project.name));
    SoftKeys keys {{"BACK", "", "ABOUT"}, 2, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    keys.dots[2] = 2; keys.dot_at[2] = uint8_t(s.ui.about_view % 2); // B3 again: the machine, then the credits
    frame(p, s, keys, h);
    if (s.ui.about_view % 2) credits_draw(p);
    else machine_draw(p, s);
}
} // namespace

const PageSpec settings_page {settings_draw, play_pad, settings_wheel, settings_push, settings_soft, settings_nudge};
const PageSpec about_page {about_draw, play_pad, no_wheel, no_push, [](Context& c, int key) {
    if (key == 0) c.back();
    else if (key == 2) c.ui.about_view = uint8_t((c.ui.about_view + 1) % 2);
}, [](Context&, int) {}};

} // namespace melodee
