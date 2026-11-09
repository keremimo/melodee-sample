// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The sampler's own Edit pages, after Trim, with the same grammar as a
// synth's: B1 and B3 name the neighbouring pages and go there.
// PAD (2 of 7): K1 pitch, K2 fine, K3 level of the last pad hit.
// PLAY (3 of 7): gate or one shot, its mute group (pads in a group cut each
//   other), reverse.
// ENVELOPE (4 of 7): the pad's attack, decay, release; the wheel turns to
//   row 2, sustain. It shapes this pad, not the kit.
// FILTER (5 of 7), for the whole kit: cutoff, resonance, type.
// FOLLOW (6 of 7), for the whole kit: on, pads keep the project's tempo by
//   speed, each from its own tempo worked out from its length as bars.
// CLOUD (7 of 7): pads as clouds of grains while held: K1 where, K2 grain
//   size, K3 grains a second; row 2 scan, spread and Cloud on or off.
#include "melodee/engine/sampler.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
const PageId order[] {PageId::Trim, PageId::PadPage, PageId::PlayPage, PageId::EnvelopePage, PageId::FilterPage, PageId::FollowPage, PageId::CloudPage};
const char* keys[] {"TRIM", "PAD", "PLAY", "ENVELOPE", "FILTER", "FOLLOW", "CLOUD"};
const char* titles[] {"Trim", "Pad", "Play", "Envelope", "Filter", "Follow", "Cloud"};
unsigned index_of(PageId page) { for (unsigned n = 0; n < 7; ++n) if (order[n] == page) return n; return 0; }

void top(Paint& p, const Snapshot& s, const juce::String& pill_text, bool rows) {
    auto c = colour_of(s);
    unsigned n = index_of(s.ui.page);
    Header h = header_of(s);
    if (pill_text.isNotEmpty()) h.left.push_back(pill(pill_text, c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, sampler_keys(s.ui.page, c), h);
    p.text(titles[n], zone::pad, zone::picture_y, 200, 22, plain(colour::text, type::title, Weight::SemiBold));
    if (rows) {
        float dots = page_dots(p, zone::width - zone::pad, zone::picture_y + 11, 2, s.ui.sampler_row, c, true);
        p.text("ROW " + juce::String(s.ui.sampler_row + 1) + " / 2", zone::pad, zone::picture_y, zone::width - 2 * zone::pad - dots - 6, 22,
               caps_text(colour::mid), juce::Justification::centredRight);
    }
}
juce::String pad_pill(const Snapshot& s) {
    const auto& t = s.track();
    char label[4]; pad_label(t.last_pad, label);
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    return juce::String("PAD ") + juce::String(label) + (pad.used() ? juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(s.project.samples[pad.sample % sample_slots].name) : juce::String());
}
const PadSound& pad_of(const Snapshot& s) { return s.track().pads[s.track().last_pad % sampler_pads]; }
void pills(Paint& p, float y, std::initializer_list<juce::String> texts, juce::Colour first) {
    float x = zone::pad;
    bool lead = true;
    for (const auto& t : texts) {
        auto style = caps_text(lead ? first : colour::mid, 7);
        float w = p.width(t, style) + 9;
        p.fill(x, y, w, 12, colour::surf, 3);
        p.text(t, x, y, w, 12, style, juce::Justification::centred);
        x += w + 4; lead = false;
    }
}

void pad_draw(Paint& p, const Snapshot& s) {
    top(p, s, pad_pill(s), false);
    const auto& pad = pad_of(s);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30;
    // Pitch on a line from -12 to +12 (beyond, it sits at the end).
    p.text("PITCH", x, y, 60, 9, caps_text(colour::mid, 7));
    p.text((pad.pitch >= 0 ? "+" : "") + juce::String(pad.pitch) + " ST", x, y, w, 9, caps_text(colour::text, 7), juce::Justification::centredRight);
    p.fill(x, y + 18, w, 1, colour::line);
    for (int st = -12; st <= 12; ++st) p.fill(x + w * (st + 12) / 24.0f, y + (st % 12 ? 15 : 12), 0.5f, st % 12 ? 6 : 12, colour::dim);
    float px = x + w * (std::clamp<int>(pad.pitch, -12, 12) + 12) / 24.0f;
    p.fill(px - 0.5f, y + 10, 1.5f, 16, c);
    p.fill(px - 3, y + 7, 6, 6, c, 1.5f);
    // Fine, and the length at this pitch.
    float fy = y + 44;
    p.text("FINE", x, fy, 40, 9, caps_text(colour::mid, 7));
    p.fill(x, fy + 16, w * 0.32f, 1, colour::line);
    p.fill(x + w * 0.32f * (pad.fine + 50) / 100.0f - 0.5f, fy + 11, 1.5f, 10, knob_shade(s.ui.track, 1));
    if (s.sample.frames && pad.used()) {
        uint32_t end = pad.end ? pad.end : s.sample.frames, length = end > pad.start ? end - pad.start : 0;
        double at_pitch = length / std::exp2((pad.pitch + pad.fine / 100.0) / 12.0) / 44100.0;
        p.text("LEVEL " + juce::String(pad.level) + (pad.level == 100 ? juce::String::fromUTF8(" \xc2\xb7 UNITY") : juce::String()),
               x + w * 0.4f, fy, w * 0.3f, 9, caps_text(colour::mid, 7));
        p.text(juce::String(at_pitch, 3) + " S AT THIS PITCH", x, fy, w, 9, caps_text(colour::mid, 7), juce::Justification::centredRight);
        float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
        waveform(p, x + w * 0.4f, fy + 13, w * 0.6f, 14, s.sample.overview, from, to, from, to, c, 1);
    }
}

void play_draw(Paint& p, const Snapshot& s) {
    top(p, s, pad_pill(s), false);
    const auto& t = s.track();
    const auto& pad = pad_of(s);
    auto c = colour_of(s);
    float x = zone::pad, y = zone::picture_y + 30;
    p.text("WHEN HIT", x, y, 100, 9, caps_text(colour::mid, 7));
    const char* modes[] {"Gate", "One shot"};
    for (unsigned n = 0; n < 2; ++n) {
        bool on = unsigned(pad.play) == n;
        if (on) p.circle(x + 2, y + 21 + n * 18, 2, c);
        p.text(modes[n], x + 8, y + 15 + n * 18, 120, 12, plain(on ? colour::text : colour::dim, 11, Weight::Medium));
    }
    p.text("REVERSE", x, y + 58, 50, 12, caps_text(colour::mid, 7));
    p.text(pad.reverse ? "On" : "Off", x + 50, y + 58, 60, 12, plain(pad.reverse ? colour::text : colour::dim, 10, Weight::Medium));
    // The pads of its mute group, as they sit, with their names.
    float gx = x + 158, gw = 72, cell = (gw - 9) / 4;
    p.text(pad.group ? "GROUP " + juce::String(pad.group) + " CUTS EACH OTHER" : juce::String("IN NO GROUP"), gx, y, 160, 9, caps_text(colour::mid, 7));
    unsigned bank = t.last_pad / 16, listed = 0;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        bool same = pad.group && t.pads[kit].group == pad.group;
        p.fill(gx + column * (cell + 3), y + 14 + row * (cell + 3), cell, cell, same ? (kit == t.last_pad ? c : c.withAlpha(0.6f)) : colour::surf, 2);
        if (same && listed < 4) {
            char label[4]; pad_label(kit, label);
            float ly = y + 16 + listed * 14;
            p.fill(gx + gw + 8, ly + 3, 5, 5, c, 1);
            p.text(juce::String(label), gx + gw + 16, ly, 22, 11, caps_text(colour::mid, 6));
            p.text(s.project.samples[t.pads[kit].sample % sample_slots].name, gx + gw + 38, ly, 60, 11, plain(colour::text, 9, Weight::Medium));
            ++listed;
        }
    }
}

void envelope_draw(Paint& p, const Snapshot& s) {
    top(p, s, pad_pill(s), true);
    const auto& pad = pad_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 78;
    if (!s.sample.frames || !pad.used()) return;
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    waveform(p, x, y, w, h, s.sample.overview, from, to, from, to, colour_of(s).withAlpha(0.6f));
    // The level line, each stretch in the colour of the pot that moves it.
    double length = (end - pad.start) / 44100.0;
    auto seconds = [](int v, float k) { return time_seconds(v) * k; };
    double a = seconds(pad.attack, 0.3f), d = pad.decay >= 100 ? length : seconds(pad.decay, 1.0f), r = seconds(pad.release, 0.5f);
    float sustain = pad.decay >= 100 ? 1.0f : pad.sustain / 100.0f;
    auto x_of = [&](double t) { return x + float(std::min(t / std::max(length, 0.01), 1.0)) * w; };
    float top_y = y + 2, base = y + h - 2;
    auto y_of = [&](float l) { return base - l * (base - top_y); };
    struct Seg { double t0, t1; float l0, l1; unsigned knob; } segs[] {
        {0, a, 0, 1, 0}, {a, a + d, 1, sustain, 1}, {a + d, std::max(a + d, length - r), sustain, sustain, 3}, {std::max(a + d, length - r), length, sustain, 0, 2}};
    for (const auto& sg : segs) {
        if (sg.t1 <= sg.t0) continue;
        p.g.setColour(sg.knob == 3 ? colour::mid : knob_shade(s.ui.track, sg.knob));
        p.g.drawLine(x_of(sg.t0), y_of(sg.l0), x_of(sg.t1), y_of(sg.l1), 1.5f);
    }
    p.text("HELD AT " + juce::String(int(sustain * 100)) + " %", x, base + 4, w, 9, caps_text(colour::sec, 7), juce::Justification::centred);
    pills(p, base + 16, {"THIS PAD ONLY", "HARDER HITS ARE LOUDER"}, colour_of(s));
}

void filter_draw(Paint& p, const Snapshot& s) {
    top(p, s, "WHOLE KIT", false);
    const auto& v = s.track().params;
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 70;
    float cutoff = 20.0f * std::exp2(v[sampler::Filter] / 100.0f * 9.97f), q = 1 / std::max(0.05f, 2 - 1.9f * v[sampler::Resonance] / 100.0f);
    juce::Path line, area;
    float cx = x;
    for (int n = 0; n <= 120; ++n) {
        float hz = 20.0f * std::pow(1000.0f, n / 120.0f), r = hz / cutoff;
        float lp = 1 / std::sqrt((1 - r * r) * (1 - r * r) + (r / q) * (r / q));
        float gain = v[sampler::FilterType] == 1 ? lp * r / q : v[sampler::FilterType] == 2 ? lp * r * r : lp;
        float db = 20 * std::log10(std::max(gain, 1e-4f));
        float px = x + w * n / 120.0f, py = y + h - std::clamp((db + 30) / 45, 0.0f, 1.0f) * h;
        if (n == 0) { line.startNewSubPath(px, py); area.startNewSubPath(px, y + h); }
        line.lineTo(px, py); area.lineTo(px, py);
        if (hz <= cutoff) cx = px;
    }
    area.lineTo(x + w, y + h); area.closeSubPath();
    p.g.setColour(c.withAlpha(0.14f)); p.g.fillPath(area);
    p.g.setColour(c); p.g.strokePath(line, juce::PathStrokeType(1.5f));
    p.fill(cx, y, 1, h, knob_shade(s.ui.track, 0));
    auto axis = caps_text(colour::dim, 7);
    p.text("20 HZ", x, y + h + 3, 40, 9, axis); p.text("200", x + w / 3 - 10, y + h + 3, 40, 9, axis);
    p.text("2 K", x + 2 * w / 3 - 10, y + h + 3, 40, 9, axis); p.text("20 K", x + w - 40, y + h + 3, 40, 9, axis, juce::Justification::centredRight);
    static const char* types[] {"LOW PASS", "BAND PASS", "HIGH PASS"};
    pills(p, y + h + 16, {juce::String(types[v[sampler::FilterType] % 3]) + juce::String::fromUTF8(" \xc2\xb7 12 DB"), "AFTER EVERY PAD"}, c);
}

void follow_draw(Paint& p, const Snapshot& s) {
    top(p, s, "WHOLE KIT", false);
    const auto& pad = pad_of(s);
    auto c = colour_of(s);
    bool on = s.track().params[sampler::Follow] != 0;
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 28;
    if (!pad.used() || !s.sample.frames) { p.text("Hit a pad with a loop on it", x, y + 20, w, 20, plain(colour::dim, 11, Weight::Medium), juce::Justification::centred); return; }
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    double length = (end - pad.start) / 44100.0, own = pad.own_tempo / 10.0, project = s.project.tempo / 10.0;
    double played = on && own > 0 ? length * own / project : length;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    auto line = [&](float ly, const juce::String& left, double seconds, float width) {
        p.text(left, x, ly, w, 9, caps_text(colour::mid, 7));
        p.text(juce::String(seconds, 3) + " S", x, ly, w, 9, caps_text(colour::text, 7), juce::Justification::centredRight);
        waveform(p, x, ly + 11, w * width, 26, s.sample.overview, from, to, from, to, c, 1);
    };
    double longest = std::max(length, played);
    line(y, own > 0 ? "ITS OWN TEMPO " + juce::String(own, 0) + " BPM" : juce::String("ITS OWN TEMPO: TURN FOLLOW ON"), length, float(length / longest));
    line(y + 44, "IN THE PROJECT " + juce::String(project, 1) + " BPM", played, float(played / longest));
    pills(p, y + 88, {on ? "FOLLOWING BY SPEED" : "NOT FOLLOWING", "STRETCH: NOT YET"}, c);
}

void cloud_draw(Paint& p, const Snapshot& s) {
    const auto& v = s.track().params;
    top(p, s, v[sampler::Cloud] ? "CLOUD ON" : "CLOUD OFF", true);
    const auto& pad = pad_of(s);
    auto c = colour_of(s);
    float x = zone::pad, w = zone::width - 2 * zone::pad, y = zone::picture_y + 30, h = 74;
    if (!pad.used() || !s.sample.frames) return;
    uint32_t end = pad.end ? pad.end : s.sample.frames;
    float from = float(pad.start) / s.sample.frames, to = float(end) / s.sample.frames;
    waveform(p, x, y, w, h, s.sample.overview, from, to, from, to, c.withAlpha(0.5f));
    float at = x + w * v[sampler::Position] / 100.0f, size = std::max(2.0f, w * float(0.01f * std::pow(100.0f, v[sampler::Size] / 100.0f) * 44100 / std::max(1u, end - pad.start)));
    p.fill(at - size / 2, y, size, h, colour::text.withAlpha(0.12f));
    p.fill(at, y - 3, 1, h + 3, knob_shade(s.ui.track, 0));
    // A few grains round the place, as dots in K2's shade.
    for (unsigned g = 0; g < 9; ++g) {
        float gx = at + (std::sin(g * 2.3f) * 0.5f) * size, gy = y + h * (0.25f + 0.5f * std::fmod(g * 0.37f, 1.0f));
        p.fill(gx, gy, 6, 3, knob_shade(s.ui.track, 1), 1.5f);
    }
    double centre = (pad.start + (end - pad.start) * v[sampler::Position] / 100.0) / 44100.0;
    p.text("GRAINS AROUND " + juce::String(centre, 3) + " S", x, y + h + 3, w, 9, caps_text(colour::sec, 7), juce::Justification::centred);
    juce::String scan = v[sampler::Scan] == 50 ? juce::String("SCAN STILL") : "SCAN " + juce::String((v[sampler::Scan] - 50) / 50.0, 2);
    pills(p, y + h + 16, {scan, "SPREAD " + juce::String(v[sampler::Spread]) + " CT"}, colour::mid);
}

void rows_wheel(Context& c, int delta) { c.ui.sampler_row = uint8_t(std::clamp(c.ui.sampler_row + delta, 0, 1)); }
void bank(Context& c, int way) { c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks); }
} // namespace

SoftKeys sampler_keys(PageId page, juce::Colour c) {
    unsigned n = index_of(page);
    return {{juce::String::fromUTF8("\xe2\x80\xb9 ") + keys[(n + 6) % 7], "PAGE " + juce::String(n + 1) + " / 7",
             juce::String(keys[(n + 1) % 7]) + juce::String::fromUTF8(" \xe2\x80\xba")}, 1, c};
}
void sampler_soft(Context& c, int key) {
    unsigned n = index_of(c.ui.page);
    if (key == 0) c.open(order[(n + 6) % 7]);
    if (key == 2) c.open(order[(n + 1) % 7]);
    c.ui.sampler_row = 0;
}

const PageSpec pad_edit_page {pad_draw, play_pad, no_wheel, no_push, sampler_soft, bank};
const PageSpec play_edit_page {play_draw, play_pad, no_wheel, no_push, sampler_soft, bank};
const PageSpec envelope_page {envelope_draw, play_pad, rows_wheel, no_push, sampler_soft, bank};
const PageSpec filter_page {filter_draw, play_pad, no_wheel, no_push, sampler_soft, bank};
const PageSpec follow_page {follow_draw, play_pad, no_wheel, no_push, sampler_soft, bank};
const PageSpec cloud_page {cloud_draw, play_pad, rows_wheel, no_push, sampler_soft, bank};

} // namespace melodee
