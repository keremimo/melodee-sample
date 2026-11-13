// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STAGE (Sample): the selected track's quick pages, as the MPC's Sample Mode.
// B1-B3 each hold a cycle of pages (engine/engine_info.h, stage_groups()):
// the lit one again steps to the next page, another jumps to its first; the
// soft keys name each button's page, with a dot a page under a cycle. Every
// track gives the buttons the same jobs: B1 the source, B2 how it plays, B3
// where it goes. A kit: B1 Trim / Layers / Tune / Amp Env, B2 Play / Colour /
// Follow / Cloud, B3 Mix / Sends / Filter / Filt Env / Kit filter, on the last pad hit;
// a PLUGIN track (a synth: Melodee Poly, PD and Chip are plug-ins): B1 its
// presets (or its face when it has none) and its parameters, B2 arp and chord,
// B3 mix, sends, effects; a MIDI track: B1 device, program, CC, B2 arp and
// chord, B3 EDIT (the Edit page); an empty track what it becomes. The wheel fine-edits the card touched last
// (PRESETS: walks the presets; EDIT: the rows; an empty track: walks its list), Push moves to the next card
// (PRESETS: the whole list; FX: the next effect, or with none yet the effect picker; an empty track: makes it),
// Shift + Push is Back.
// The pads play the track; on a kit the pad played is the pad edited.
#include "melodee/engine/arp.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/chords.h"
#include "melodee/model/scales.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/key_pick.h"
#include "melodee/shell/meter_lights.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace melodee {
namespace {
using namespace ui;

// The picture zone at device size.
constexpr float px = zone::pad, pw = zone::width - 2 * zone::pad, top = zone::picture_y, bottom = zone::picture_y + zone::picture_h;
const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");

juce::String shown_value(const Track& t, int id) {
    if (id < 0) return {};
    const auto& info = engine_info(t.engine);
    auto v = show(info.params[id], t.params[size_t(id)]);
    return juce::String(v.value) + (v.unit.empty() ? "" : " " + juce::String(v.unit));
}

// Rows of a label, a bar and the value, centred in the picture: MIX, PERFORM, CC and the plain pages.
struct Row {
    juce::String label, sub, value;
    float fill = 0;
    bool bipolar = false;
    juce::Colour colour;
};
void rows(Paint& p, const juce::String& head, const std::vector<Row>& list, float label_w, float y0 = top, float y1 = bottom) {
    float rh = 20, gap = 15, h = list.size() * rh + (list.size() - 1) * gap + (head.isNotEmpty() ? 22 : 0);
    float y = y0 + (y1 - y0 - h) / 2;
    if (head.isNotEmpty()) { p.text(head, px, y, pw, 14, caps_text(colour::mid)); y += 22; }
    for (const auto& r : list) {
        float x = px;
        p.text(r.label, x, y, label_w, rh, p.fit(r.label, caps_text(r.colour), label_w, type::floor));
        x += label_w + 6;
        if (r.sub.isNotEmpty()) {
            p.text(r.sub, x, y, 92, rh, p.fit(r.sub, plain(colour::sec, type::body, Weight::Medium), 92, type::floor));
            x += 98;
        }
        float vw = 58, bw = px + pw - vw - 6 - x;
        p.fill(x, y + rh / 2 - 3, bw, 6, colour::line, 3);
        float f = std::clamp(r.fill, 0.0f, 1.0f);
        if (r.bipolar) {
            p.fill(x + bw / 2 - 0.5f, y + rh / 2 - 6, 1, 12, colour::dim);
            p.fill(x + f * bw - 2, y + rh / 2 - 5, 4, 10, r.colour, 2);
        } else if (f > 0) p.fill(x, y + rh / 2 - 3, bw * f, 6, r.colour, 3);
        p.text(r.value, px + pw - vw, y, vw, rh, p.fit(r.value, plain(colour::text, type::body, Weight::SemiBold), vw, type::floor),
               juce::Justification::centredRight);
        y += rh + gap;
    }
}

Row param_row(const Snapshot& s, int id, unsigned knob, bool shade = true) {
    const auto& t = s.track();
    const auto& info = engine_info(t.engine);
    const auto& pi = info.params[id];
    return {pi.label, {}, shown_value(t, id), gauge_of(pi, t.params[size_t(id)]), bipolar(pi),
            shade ? knob_shade(s.ui.track, knob) : colour::mid};
}

void axis(Paint& p, float y, const juce::String& left, const juce::String& middle, const juce::String& right) {
    auto style = caps_text(colour::mid);
    p.text(left, px, y, pw / 3, 14, p.fit(left, style, pw / 3, type::floor));
    p.text(middle, px + pw / 3, y, pw / 3, 14, p.fit(middle, caps_text(colour::sec), pw / 3, type::floor), juce::Justification::centred);
    p.text(right, px + 2 * pw / 3, y, pw / 3, 14, p.fit(right, style, pw / 3, type::floor), juce::Justification::centredRight);
}

// PRESET (round 2, stage-preset B "sound poster"): the sound's name at poster size, a star when it is
// starred; while pads are held the chord they make and its notes under it; the sounds before and after it, small,
// in the corners. The wheel or K1 walks them (the one turned to loads once the turning stops).
void preset_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& info = engine_info(t.engine);
    auto c = colour_of(s);
    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(t, s.ui, list);
    if (!count) {
        p.text(s.ui.sounds_filter == 1 ? "No starred sounds yet" : "Nothing loaded yet", px, top, pw, zone::picture_h,
               plain(colour::sec, type::body, Weight::Medium), juce::Justification::centred);
        return;
    }
    int at = 0;
    if (s.ui.preset_due) at = std::min<int>(s.ui.preset_cursor, int(count) - 1);
    else for (unsigned n = 0; n < count; ++n) if (list[n] == t.preset) at = int(n);
    auto name_of = [&](unsigned sound) { return sound < info.sound_count ? name_case(info.sounds[sound].name) : "Own sound " + juce::String(sound - info.sound_count + 1); };
    // The name at poster size, the star after it.
    unsigned sound = list[size_t(at)];
    bool starred = s.ui.settings.favourite(unsigned(t.engine), sound);
    auto name = name_of(sound);
    auto poster = p.fit(name, plain(s.ui.preset_due ? colour::amber : colour::text, 30, Weight::SemiBold), pw - 30, type::title);
    float nw = std::min(p.width(name, poster), pw - 30);
    p.text(name, px + 4, top + 14, nw, 36, poster);
    if (starred) icon(p, Icon::Star, px + 4 + nw + 8, top + 22, 16, c, 1.5f);
    // The chord the pads hold, and its notes.
    std::array<int, 16> held {};
    unsigned heard = 0;
    for (unsigned n = 0; n < 16; ++n) if (s.held_keys[n] >= 0 && (s.ui.held_pads & (1u << n))) held[heard++] = s.held_keys[n];
    if (heard) {
        std::sort(held.begin(), held.begin() + heard);
        juce::String chord = name_chord(held.data(), heard), notes;
        for (unsigned n = 0; n < heard; ++n) notes += (n ? "  " : "") + juce::String(note_name(held[n]));
        float x = px + 6;
        if (chord.isNotEmpty()) {
            auto cs = plain(c, type::list, Weight::SemiBold);
            p.text(chord, x, top + 54, 90, 18, cs);
            x += std::min(p.width(chord, cs), 90.0f) + 8;
        }
        p.text(notes, x, top + 55, px + pw - x, 16, p.fit(notes, plain(colour::sec, type::body), px + pw - x, type::floor));
    }
    // The sounds either side, small, in the corners.
    auto side = plain(colour::mid, type::body);
    if (at > 0) {
        icon(p, Icon::ChevronLeft, px, bottom - 16, 10, colour::dim, 1.6f);
        auto before = name_of(list[size_t(at - 1)]);
        p.text(before, px + 13, bottom - 19, pw / 2 - 20, 16, p.fit(before, side, pw / 2 - 20, type::floor));
    }
    if (at + 1 < int(count)) {
        icon(p, Icon::ChevronRight, px + pw - 10, bottom - 16, 10, colour::dim, 1.6f);
        auto after = name_of(list[size_t(at + 1)]);
        p.text(after, px + pw / 2, bottom - 19, pw / 2 - 13, 16, p.fit(after, side, pw / 2 - 20, type::floor), juce::Justification::centredRight);
    }
}

// TONE on a filter: the curve from 20 Hz to 20 kHz with the cutoff marked.
void filter_picture(Paint& p, const Snapshot& s, const StagePage& page, float y0 = top, float y1 = bottom) {
    const auto& t = s.track();
    int cut = t.params[size_t(page.params[0])], res = t.params[size_t(page.params[1])];
    float hz = 20.0f * std::exp2(cut / 100.0f * 9.97f), q = 0.7f + res * 0.06f;
    draw_filter_curve(p, px, y0 + 6, pw, (y1 - y0) - 26, 0, 2, hz, q, colour_of(s), knob_shade(s.ui.track, 0));
}

// An envelope: attack, decay, sustain on K1-K3, release on Shift + K1; times on a square-root scale.
void envelope_picture(Paint& p, const Snapshot& s, const StagePage& page, float top_y = top, float bottom_y = bottom) {
    const auto& t = s.track();
    const auto& info = engine_info(t.engine);
    auto time = [&](int id) {
        if (id < 0) return 0.3f;
        const auto& pi = info.params[id];
        return pi.format == Format::Time ? time_seconds(t.params[size_t(id)]) : 0.02f + 2.0f * t.params[size_t(id)] / std::max<float>(pi.max, 1);
    };
    int a = page.params[0], d = page.params[1], su = page.params[2], r = page.shift[0];
    float level = su >= 0 ? t.params[size_t(su)] / float(std::max<int>(info.params[su].max, 1)) : 1;
    float wa = std::sqrt(time(a)), wd = std::sqrt(time(d)), wr = std::sqrt(time(r)), hold = 0.35f, total = wa + wd + wr + hold;
    float x0 = px + 2, scale = (pw - 4) / total, y0 = top_y + 8, y1 = bottom_y - 8;
    float x1 = x0 + wa * scale, x2 = x1 + wd * scale, x3 = x2 + hold * scale, x4 = x3 + wr * scale;
    auto y_of = [&](float l) { return y1 - l * (y1 - y0); };
    juce::Path area;
    area.startNewSubPath(x0, y1); area.lineTo(x1, y_of(1)); area.lineTo(x2, y_of(level)); area.lineTo(x3, y_of(level)); area.lineTo(x4, y1);
    area.closeSubPath();
    p.fill(px, y1, pw, 1, colour::line);
    p.g.setColour(colour_of(s).withAlpha(0.14f)); p.g.fillPath(area);
    auto line = [&](float ax, float ay, float bx, float by, juce::Colour c) { p.g.setColour(c); p.g.drawLine(ax, ay, bx, by, 2); };
    line(x0, y1, x1, y_of(1), knob_shade(s.ui.track, 0));
    line(x1, y_of(1), x2, y_of(level), knob_shade(s.ui.track, 1));
    line(x2, y_of(level), x3, y_of(level), knob_shade(s.ui.track, 2));
    line(x3, y_of(level), x4, y1, colour::mid); // the cards hold the numbers: the shape alone here
}

// LFO: its shape at its depth, the rate under it.
void lfo_picture(Paint& p, const Snapshot& s, const StagePage& page, float y0 = top, float y1 = bottom) {
    const auto& t = s.track();
    int shape = t.params[size_t(page.params[1])], amount = t.params[size_t(page.params[2])];
    float mid = (y0 + y1) / 2.0f, amp = ((y1 - y0) - 20) / 2.0f * std::max(0.15f, amount / 100.0f);
    juce::Path line;
    for (int n = 0; n <= 300; ++n) {
        float phase = std::fmod(n / 100.0f, 1.0f), y;
        switch (shape) {
        case 1: y = 2 * phase - 1; break;
        case 2: y = phase < 0.5f ? 1 : -1; break;
        case 3: y = std::sin(float(n / 25) * 12.9898f); break;
        default: y = 1 - 4 * std::fabs(phase - 0.5f);
        }
        float x = px + pw * n / 300.0f;
        if (n == 0) line.startNewSubPath(x, mid - y * amp); else line.lineTo(x, mid - y * amp);
    }
    p.fill(px, mid, pw, 1, colour::line);
    p.g.setColour(colour_of(s));
    p.g.strokePath(line, juce::PathStrokeType(2, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded)); // the cards hold the numbers
}

// MIX: what the cards cannot say. The track's meter with its level marked, where it sits between left and right,
// and where it goes: the master, and its send (as thick as it is) into its bus, named by the bus's first effect.
void mix_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& m = t.mix;
    auto c = colour_of(s);
    float mx = px + 4, my = top + 8, mh = zone::picture_h - 16;
    p.fill(mx, my, 7, mh, colour::line, 3);
    float meter = s.meters[s.ui.track % track_count];
    auto place = [](float db) { return std::clamp((db + 48) / 48, 0.0f, 1.0f); };
    float heard = meter > 0 && !m.mute ? place(20 * std::log10(meter)) : 0;
    if (heard > 0) p.fill(mx, my + mh * (1 - heard), 7, mh * heard, c, 3);
    for (float db : meter_mix_db) p.fill(mx + 10, my + mh * (1 - place(db)) - 0.5f, 4, 1, colour::dim); // the LEDs' five steps as a scale
    p.fill(mx - 3, my + mh * (1 - m.level / 127.0f) - 0.75f, 13, 1.5f, colour::text); // the level
    // The pan: a half dial, its needle where the track sits.
    float cx = px + 108, cy = top + 92, r = 32;
    juce::Path dial;
    dial.addCentredArc(cx, cy, r, r, 0, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    p.g.setColour(colour::line);
    p.g.strokePath(dial, juce::PathStrokeType(5, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    float a = std::clamp(m.pan / 50.0f, -1.0f, 1.0f) * juce::MathConstants<float>::halfPi;
    p.g.setColour(knob_shade(s.ui.track, 1));
    p.g.drawLine(cx, cy, cx + (r - 3) * std::sin(a), cy - (r - 3) * std::cos(a), 2.5f);
    p.circle(cx, cy, 4, knob_shade(s.ui.track, 1));
    p.text("L", cx - r - 8, cy + 6, 16, 14, caps_text(colour::dim), juce::Justification::centred);
    p.text("R", cx + r - 8, cy + 6, 16, 14, caps_text(colour::dim), juce::Justification::centred);
    p.fill(mx + 12, cy - 1, cx - r - 10 - mx - 12, 2.5f, c, 1);
    // Where it goes: the master, and the bus its send feeds.
    float bx = px + pw - 98, from = cx + r + 6;
    auto box = [&](float y, Icon glyph, juce::Colour gc, const juce::String& name, const juce::String& under) {
        p.fill(bx, y, 98, 36, colour::surf, 4);
        icon(p, glyph, bx + 8, y + 9, 18, gc, 1.5f);
        p.text(name, bx + 32, y + (under.isEmpty() ? 11 : 4), 62, 14, caps_text(colour::text));
        if (under.isNotEmpty()) p.text(under, bx + 32, y + 18, 62, 14, p.fit(under, plain(colour::mid, type::small), 62, type::floor));
    };
    juce::Path to_master, to_bus;
    to_master.startNewSubPath(from, cy - 4); to_master.cubicTo(from + 30, cy - 4, bx - 30, top + 30, bx, top + 30);
    p.g.setColour(c);
    p.g.strokePath(to_master, juce::PathStrokeType(3, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    box(top + 12, Icon::Speaker, colour::text, "MASTER", {});
    // The quick send (K3) into bus A, as thick as it is; the other three are SENDS' (B1 again).
    const auto& bus = s.project.buses[0];
    const auto& first = effect_info(EffectKind(bus.fx.slots[0].kind));
    float by = top + 100, send = m.sends[0] / 100.0f;
    if (m.sends[0] > 0) {
        to_bus.startNewSubPath(from, cy + 4); to_bus.cubicTo(from + 30, cy + 4, bx - 30, by + 18, bx, by + 18);
        p.g.setColour(knob_shade(s.ui.track, 2));
        p.g.strokePath(to_bus, juce::PathStrokeType(1 + 4 * send, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    box(by, bus.fx.slots[0].used() ? family_icon(first.family) : Icon::Ring, m.sends[0] > 0 ? knob_shade(s.ui.track, 2) : colour::dim,
        "BUS A", bus.fx.slots[0].used() ? juce::String(first.name) : juce::String());
}

// SENDS: the track's level on the left, a line from it to each of the four buses as thick as its send, each bus
// with its first effect's glyph and name and a bar of what it receives. The bus in view (K1) wears its letter on a
// chip in K1's shade, its line and bar in K2's (the send); the other three grey. On a kit the last pad's own sends,
// under its name; Shift held, the whole kit's (KIT SEND) under the track's.
void sends_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& m = t.mix;
    auto c = colour_of(s);
    const bool pad_view = t.engine == EngineKind::Sampler && !s.ui.shift;
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    auto send_of = [&](unsigned b) { return pad_view ? (pad.used() ? pad.sends[b] : 0) : m.sends[b]; };
    juce::String title = pad_view ? pad_name(s, t.last_pad % sampler_pads, false) : name_case(juce::String(t.name));
    if (pad_view && title.isEmpty()) { char label[4]; pad_label(t.last_pad % sampler_pads, label); title = label; } // an empty pad: its place
    p.text(title, px, top + 4, 140, 20, p.fit(title, plain(colour::text, type::list, Weight::SemiBold), 140, type::floor));
    float mx = px + 4, my = top + 36, mh = zone::picture_h - 44;
    p.fill(mx, my, 6, mh, colour::line, 3);
    float meter = s.meters[s.ui.track % track_count];
    float heard = meter > 0 && !m.mute ? std::clamp((20 * std::log10(meter) + 48) / 48, 0.0f, 1.0f) : 0;
    if (heard > 0) p.fill(mx, my + mh * (1 - heard), 6, mh * heard, c, 3);
    float ox = mx + 14, oy = my + mh / 2;
    p.fill(mx + 8, oy - 1, ox - mx - 8, 2.5f, c, 1);
    constexpr float bx = zone::pad + 144, bw = zone::width - zone::pad - bx, bh = 30, gap = 4.5f;
    unsigned in_view = s.ui.sends_bus % bus_count;
    for (unsigned n = 0; n < bus_count; ++n) {
        unsigned b = n == bus_count - 1 ? in_view : n < in_view ? n : n + 1; // the bus in view last, its line on top
        bool picked = b == in_view;
        float by = top + 4 + b * (bh + gap), mid = by + bh / 2, amount = send_of(b) / 100.0f;
        auto shade = picked ? knob_shade(s.ui.track, 1) : colour::mid;
        const auto& bus = s.project.buses[b];
        bool has_fx = bus.fx.slots[0].used();
        juce::Path line;
        line.startNewSubPath(ox, oy); line.cubicTo(ox + 40, oy, bx - 44, mid, bx - 2, mid);
        if (amount > 0) {
            p.g.setColour(shade);
            p.g.strokePath(line, juce::PathStrokeType(0.8f + 5 * amount, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        } else {
            juce::Path dashed;
            const float dash[] {2, 3};
            juce::PathStrokeType(1).createDashedStroke(dashed, line, dash, 2);
            p.g.setColour(colour::dim); p.g.fillPath(dashed);
        }
        p.fill(bx, by, bw, bh, picked ? colour::raised : colour::surf, 4);
        if (has_fx) icon(p, family_icon(effect_info(EffectKind(bus.fx.slots[0].kind)).family), bx + 8, by + 7, 16, amount > 0 ? shade : colour::mid, 1.5f);
        else p.circle(bx + 16, mid, 2, colour::dim);
        auto letter = juce::String::charToString(juce::juce_wchar('A' + b));
        if (picked) {
            p.fill(bx + 29, mid - 8, 16, 16, knob_shade(s.ui.track, 0), 3);
            p.text(letter, bx + 29, mid - 8, 16, 16, plain(colour::bg, type::body, Weight::Bold), juce::Justification::centred);
        } else {
            p.text(letter, bx + 31, by, 14, bh, plain(has_fx ? colour::text : colour::dim, type::body, Weight::Bold));
        }
        if (has_fx) {
            juce::String fx(effect_info(EffectKind(bus.fx.slots[0].kind)).name);
            p.text(fx, bx + 46, by, bw - 66, bh, p.fit(fx, plain(colour::mid, type::body), bw - 66, type::floor));
        }
        float bar = (bh - 8) * amount; // what it receives
        p.fill(bx + bw - 10, by + 4, 4, bh - 8, colour::line, 2);
        if (bar > 0) p.fill(bx + bw - 10, by + 4 + (bh - 8) - bar, 4, bar, shade, 2);
    }
}

// ARP: a keyboard along the foot, the keys held filled (none held: an example chord, faint); the notes to come
// hang over their keys as dots, the next one white and largest, falling toward the keys as they near, joined in
// the order they play, so the run reads like hands on an instrument.
void arp_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    std::vector<int> held;
    for (auto k : s.held_keys) if (k >= 0 && k <= 127) held.push_back(k);
    bool example = held.empty();
    if (example) held = {48, 52, 55};
    std::sort(held.begin(), held.end());
    held.erase(std::unique(held.begin(), held.end()), held.end());
    std::vector<int> notes;
    for (unsigned o = 0; o < std::max<unsigned>(t.arp_octaves, 1); ++o) for (int k : held) notes.push_back(k + int(o) * 12);
    std::vector<int> order = notes;
    switch (ArpMode(t.arp_mode % unsigned(ArpMode::Count))) {
    case ArpMode::Down: std::reverse(order.begin(), order.end()); break;
    case ArpMode::UpDown: for (int n = int(notes.size()) - 2; n > 0; --n) order.push_back(notes[size_t(n)]); break;
    case ArpMode::Random: for (size_t n = 0; n < order.size(); ++n) std::swap(order[n], order[(n * 7 + 3) % order.size()]); break;
    default: break;
    }
    // Two octaves of white keys from the held notes' lowest octave (three when the run climbs further).
    int from = held.front() / 12 * 12, span = notes.back() - from >= 24 ? 3 : 2;
    static const int whites[] {0, 2, 4, 5, 7, 9, 11};
    unsigned white_keys = unsigned(7 * span + 1);
    float ky = bottom - 36, kh = 34, ww = pw / white_keys;
    auto x_of = [&](int key) { // a key's middle along the keyboard
        int octave = (key - from) / 12, n = ((key - from) % 12 + 12) % 12;
        int w = 0; while (w < 6 && whites[w + 1] <= n) ++w;
        bool black = whites[w] != n;
        return std::min(px + pw - 4, px + (octave * 7 + w) * ww + (black ? ww : ww / 2));
    };
    auto is_held = [&](int key) { return std::find(held.begin(), held.end(), key) != held.end(); };
    auto fill = example ? c.withAlpha(0.45f) : c;
    for (unsigned k = 0; k < white_keys; ++k) {
        int key = from + 12 * int(k / 7) + whites[k % 7];
        p.fill(px + k * ww + 0.5f, ky, ww - 1, kh, is_held(key) ? fill : colour::sec.withAlpha(0.85f), 2);
    }
    for (unsigned k = 0; k + 1 < white_keys; ++k) {
        int white = whites[k % 7];
        if (white == 4 || white == 11) continue; // no black key after E or B
        int key = from + 12 * int(k / 7) + white + 1;
        p.fill(px + (k + 1) * ww - ww * 0.3f, ky, ww * 0.6f, kh * 0.58f, is_held(key) ? fill : colour::bg, 2);
    }
    // The next eight notes over their keys, the next lowest (nearest the keys), joined in their order.
    constexpr unsigned ahead = 8;
    float step = (ky - top - 14) / ahead;
    juce::Path run;
    std::array<juce::Point<float>, ahead> at {};
    for (unsigned k = 0; k < ahead; ++k) {
        int key = order[k % order.size()];
        at[k] = {x_of(key), ky - 10 - k * step};
        if (k == 0) run.startNewSubPath(at[k]); else run.lineTo(at[k]);
    }
    p.g.setColour(colour::dim);
    p.g.strokePath(run, juce::PathStrokeType(1));
    for (unsigned k = ahead; k-- > 0;) {
        auto dot = k == 0 ? colour::text : k < 4 ? knob_shade(s.ui.track, 0) : knob_shade(s.ui.track, 1);
        float alpha = (example ? 0.5f : 1.0f) * (1 - k * 0.07f);
        p.circle(at[k].x, at[k].y, k == 0 ? 4.6f : 3.6f, dot.withAlpha(alpha));
    }
}

// CHORD: the chord the last pad plays (pad 1 before any), its notes, and two octaves of keys with them lit;
// faint while chords are off (Shift + K1).
void chord_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    unsigned pad = s.ui.chord_pad >= 0 ? unsigned(s.ui.chord_pad) : 0;
    std::array<int, notes_per_step> keys {};
    unsigned n = chord_notes(s.project, t, pad, keys);
    auto ink = t.chord ? c : c.withAlpha(0.35f);
    juce::String name(chord_name(s.project, t, pad));
    float w = p.text(name, px, top + 2, 150, 32, plain(t.chord ? colour::text : colour::mid, 28, Weight::SemiBold));
    juce::String notes;
    for (unsigned k = 0; k < n; ++k) notes += (k ? "  " : "") + juce::String(note_name(keys[k]));
    p.text(notes, px + w + 10, top + 10, pw - w - 10, 20, plain(ink, type::body, Weight::Medium));
    int from = keys[0] / 12 * 12;
    float ky = top + 46, kh = bottom - 6 - ky, ww = pw / 14;
    auto lit = [&](int k) { for (unsigned q = 0; q < n; ++q) if (keys[q] == k) return true; return false; };
    static const int whites[] {0, 2, 4, 5, 7, 9, 11};
    for (unsigned k = 0; k < 14; ++k) {
        int key = from + 12 * int(k / 7) + whites[k % 7];
        float kx = px + k * ww;
        p.fill(kx + 0.5f, ky, ww - 1, kh, lit(key) ? ink : colour::sec.withAlpha(t.chord ? 1.0f : 0.5f), 2);
        if (whites[k % 7] == 0 || lit(key))
            p.text(note_name(key), kx, ky + kh - 14, ww, 12, plain(colour::bg, type::small, Weight::SemiBold), juce::Justification::centred);
    }
    for (unsigned k = 0; k < 14; ++k) {
        int white = whites[k % 7];
        if (white == 4 || white == 11) continue; // no black key after E or B
        int key = from + 12 * int(k / 7) + white + 1;
        p.fill(px + (k + 1) * ww - ww * 0.3f, ky, ww * 0.6f, kh * 0.55f, lit(key) ? ink : colour::bg, 2);
    }
}

// FX: the track's whole chain as a strip of six slots, then the one in hand: its name, family, and what it does
// drawn (pages/fx_drawing.cpp, as FX Select draws it). An empty chain: Push and a plus.
void fx_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    unsigned slot = s.ui.fx_cursor[UiState::cursor_of(s.ui.track)];
    fx_strip(p, t.fx, std::min<unsigned>(slot, t.fx.count()), colour_of(s), top + 6);
    fx_in_hand(p, s, t.fx, t.fx.count() ? std::min<unsigned>(slot, t.fx.count() - 1) : 0, colour_of(s), top + 54, bottom - 4);
}

juce::String seconds(uint32_t frames) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", frames / 44100.0);
    return text;
}

// LAYERS (roadmap P3): the last pad's four layers as rows, the one in hand lit (K1), a sounding one tinted; each its
// sample, its tune and level over the pad's, and on the right how a hit reaches it: Velocity its range across
// 1-127 (K2 and K3 its ends), Cycle a dot a layer with its own turn lit, Random every dot half lit.
void layers_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    const unsigned kit = t.last_pad % sampler_pads, hand = s.ui.pad_layer % pad_layers;
    std::array<unsigned, pad_layers> turn {};
    unsigned used = 0;
    for (unsigned n = 0; n < pad_layers; ++n) turn[n] = pad.layer(n).used() ? used++ : pad_layers;
    constexpr float gap = 4;
    const float rh = (zone::picture_h - 6 - gap * (pad_layers - 1)) / pad_layers, lane_x = px + 168, lane_w = pw - 172;
    for (unsigned n = 0; n < pad_layers; ++n) {
        const PadLayer l = pad.layer(n);
        const float y = top + 3 + n * (rh + gap);
        const bool in_hand = n == hand, sounding = l.used() && pad_voices(s.heads, int(kit), l.sample).sounding > 0;
        p.fill(px, y, pw, rh, sounding ? c.withAlpha(0.2f) : in_hand ? colour::raised : colour::surf, 5);
        if (in_hand) p.ring(px, y, pw, rh, c, 5, 1.2f);
        // the layer's number, filled in the track colour while in hand
        if (in_hand) p.fill(px + 5, y + rh / 2 - 9, 18, 18, c, 4);
        else p.ring(px + 5, y + rh / 2 - 9, 18, 18, l.used() ? colour::mid : colour::dim, 4, 1);
        p.text(juce::String(n + 1), px + 5, y + rh / 2 - 9, 18, 18, plain(in_hand ? colour::bg : l.used() ? colour::sec : colour::dim, type::small, Weight::SemiBold),
               juce::Justification::centred);
        const float nx = px + 30, nw = lane_x - nx - 6;
        if (!l.used()) { p.text(juce::String::fromUTF8("\xe2\x80\x94"), nx, y, nw, rh, plain(colour::dim, type::body)); continue; }
        juce::String name = name_case(juce::String(s.project.samples[l.sample % sample_slots].name));
        const bool offsets = n && (l.pitch || l.fine || l.gain);
        p.text(name, nx, offsets ? y + 2 : y, nw, offsets ? 16 : rh, p.fit(name, plain(sounding || in_hand ? colour::text : colour::sec, type::body, Weight::Medium), nw, type::floor));
        if (offsets) {
            juce::String tune = l.pitch || l.fine ? (l.pitch >= 0 ? "+" : "") + juce::String(l.pitch) + " st" : juce::String();
            juce::String level = l.gain ? (l.gain > 0 ? "+" : "") + juce::String(l.gain / 100.0, 1) + " dB" : juce::String();
            juce::String both = tune + (tune.isNotEmpty() && level.isNotEmpty() ? dot : juce::String()) + level;
            p.text(both, nx, y + rh - 15, nw, 13, plain(colour::mid, type::small));
        }
        const float ly = y + rh / 2;
        if (pad.layer_play == LayerPlay::Velocity) { // 1..127 across the lane, the range filled, its ends in K2's and K3's shades
            auto x_of = [&](int v) { return lane_x + lane_w * float(v - 1) / 126.0f; };
            p.fill(lane_x, ly - 1, lane_w, 2, colour::line, 1);
            float x0 = x_of(l.low), x1 = x_of(l.high);
            p.fill(x0, ly - 5, std::max(3.0f, x1 - x0), 10, (in_hand ? c : colour::mid).withAlpha(in_hand ? 0.55f : 0.35f), 3);
            p.fill(x0 - 1, ly - 7, 2, 14, in_hand ? knob_shade(s.ui.track, 1) : colour::mid);
            p.fill(x1 - 1, ly - 7, 2, 14, in_hand ? knob_shade(s.ui.track, 2) : colour::mid);
        } else { // a dot a sounding layer: Cycle lights this one's turn, Random every one at half
            const float dx = 12, dw = dx * float(used - 1), x0 = lane_x + lane_w / 2 - dw / 2;
            for (unsigned k = 0; k < used; ++k) {
                bool mine = k == turn[n];
                auto ink = pad.layer_play == LayerPlay::Cycle ? (mine ? (in_hand ? c : colour::sec) : colour::dim)
                                                              : (in_hand ? c : colour::sec).withAlpha(0.5f);
                p.circle(x0 + dx * k, ly, mine || pad.layer_play == LayerPlay::Random ? 3.5f : 2.5f, ink);
            }
        }
    }
}

// A kit whose last pad holds a multisampled instrument (round 3 "instrument C": its TRIM is ZONES).
bool instrument_pad(const Snapshot& s) {
    const auto& t = s.track();
    if (t.engine != EngineKind::Sampler) return false;
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    return pad.used() && s.project.samples[pad.sample % sample_slots].instrument();
}

// How the pad's layers play, as an LED-less pill in the header (Shift + K1 turns it).
HeaderItem layers_pill(const Snapshot& s) {
    const auto& pad = sampler_pad(s);
    unsigned count = 0;
    for (unsigned n = 0; n < pad_layers; ++n) count += pad.layer(n).used();
    return pill(juce::String(layer_play_name(pad.layer_play)), count > 1 ? colour_of(s) : colour::mid);
}

// Shift on LAYERS: B1 LOAD onto the layer in hand, B2 CLEAR it (dim on layer 1 or an empty layer).
ui::SoftKeys layers_keys(const Snapshot& s) {
    const auto& pad = sampler_pad(s);
    unsigned n = s.ui.pad_layer % pad_layers;
    SoftKeys keys {{"LOAD", "CLEAR", ""}, -1, colour_of(s)};
    if (!n || !pad.layers[n - 1].used()) keys.tints[1] = colour::dim;
    if (n && !pad.used()) keys.tints[0] = colour::dim;
    return keys;
}

// A kit page: the last pad's sample, its volume on the left and pan on the right, chips for how it plays,
// the region tinted; TRIM draws its handles (zoomed as Shift + K asks), the envelopes and the filter lie over it.
void wave_picture(Paint& p, const Snapshot& s, const StagePage& page) {
    const auto& t = s.track();
    const auto& pad = sampler_pad(s);
    auto c = colour_of(s);
    float wx = px + 13, ww = pw - 26, wy = top + 20, wh = zone::picture_h - 40;
    char label[4];
    pad_label(t.last_pad, label);
    if (pad.used() && s.sample.instrument) { instrument_picture(p, s, page.kind); return; }
    if (page.kind == StageKind::Layers && pad.used()) { layers_picture(p, s); return; }
    if (!pad.used() || !s.sample.frames) {
        p.text(label, px, wy + 14, pw, 26, plain(colour::text, type::title, Weight::SemiBold), juce::Justification::centred);
        juce::String why = !pad.used() ? juce::String("EMPTY") : s.sample.error.empty() ? juce::String("LOADING") : juce::String(s.sample.error);
        p.text(why, px, wy + 42, pw, 18, p.fit(why, pad.used() && !s.sample.error.empty() ? plain(colour::amber, type::body, Weight::Medium)
                                                                                         : caps_text(colour::mid, type::body), pw, type::floor), juce::Justification::centred);
        return;
    }
    // How it plays, as glyphs on the bottom row (lit when on): one shot or note on, loop (locked: the lock), reverse;
    // Cloud and Follow their LED; Trim's zoom as its factor.
    auto modes = [&] {
        float gx = px, gy = bottom - 14;
        auto glyph = [&](Icon i, bool on) { icon(p, i, gx, gy, 12, on ? c : colour::dim, 1.6f); gx += 18; };
        glyph(pad.play == PadPlay::Gate ? Icon::Fader : Icon::OneShot, true);
        glyph(Icon::Loop, pad.loop);
        if (pad.loop && pad.loop_lock) glyph(Icon::Lock, true);
        glyph(Icon::Reverse, pad.reverse);
        if (page.kind == StageKind::Trim && s.ui.trim_zoom) {
            double times = zoom_factor(s.sample.frames, s.ui.trim_zoom);
            p.text(juce::String::fromUTF8("\xc3\x97") + (times < 10 ? juce::String(times, 1) : juce::String(int(std::lround(times)))), gx, gy - 1, 44, 14, caps_text(c));
        }
    };
    // Volume and pan, as two thin meters at the sides.
    p.fill(px, wy, 5, wh, colour::line, 2.5f);
    float vol = pad.level / 127.0f;
    p.fill(px, wy + wh * (1 - vol), 5, wh * vol, c, 2.5f);
    p.fill(px + pw - 5, wy, 5, wh, colour::line, 2.5f);
    p.fill(px + pw - 7, wy + wh / 2 - 0.5f, 9, 1, colour::dim);
    p.fill(px + pw - 7, wy + wh / 2 - 3 + pad.pan / 50.0f * (wh / 2 - 3), 9, 6, knob_shade(s.ui.track, 2), 1.5f);

    // Filter and Kit filter: the wave gives a third of its height to the filter's own band below it (frequency, not time).
    const bool band = page.kind == StageKind::Filter || page.kind == StageKind::KitFilter;
    const float full_h = wh;
    if (band) wh = std::floor(full_h * 0.64f);
    uint32_t frames = s.sample.frames, end = pad.end ? std::min(pad.end, frames) : frames, start = std::min(pad.start, end);
    float region_from = float(start) / frames, region_to = float(end) / frames;
    float from = 0, to = 1;
    // The pad's own pages look at its region alone, the whole file a strip above with the region bracketed:
    // an envelope or a filter on a short slice stays readable.
    bool zoomed = (page.kind == StageKind::AmpEnv || page.kind == StageKind::FiltEnv || page.kind == StageKind::Filter
                   || page.kind == StageKind::Tune || page.kind == StageKind::Play || page.kind == StageKind::PadMix)
               && region_to - region_from < 0.95f && region_to > region_from;
    if (zoomed) {
        from = region_from; to = region_to;
        float my = top + 2, mh = 12, bx0 = wx + region_from * ww, bx1 = wx + region_to * ww;
        waveform(p, wx, my, ww, mh, s.sample.overview, 0, 1, region_from, region_to, colour::mid);
        p.fill(bx0, my - 1, std::max(2.0f, bx1 - bx0), mh + 2, c.withAlpha(0.22f), 2);
        p.ring(bx0, my - 1, std::max(2.0f, bx1 - bx0), mh + 2, c, 2, 1);
        juce::Path rays;
        rays.startNewSubPath(bx0, my + mh + 1); rays.lineTo(wx, wy - 2);
        rays.startNewSubPath(bx1, my + mh + 1); rays.lineTo(wx + ww, wy - 2);
        p.g.setColour(c.withAlpha(0.45f));
        p.g.strokePath(rays, juce::PathStrokeType(0.8f));
    }
    WaveDetail detail;
    if (page.kind == StageKind::Trim && s.ui.trim_zoom) { // the view round the handle in focus (shell/zoom_view.h), its own peaks that close
        FrameView view = trim_view(s.project, s.ui);
        if (view.width()) { from = float(view.from) / frames; to = float(view.to) / frames; }
        if (s.detail_to > s.detail_from) detail = {&s.detail, float(s.detail_from) / frames, float(s.detail_to) / frames};
    }
    auto x_of = [&](float at) { return wx + (at - from) / (to - from) * ww; };
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(wx), int(wy) - 3, int(ww + 1), int(wh + 6)));
    float sx = x_of(region_from), ex = x_of(region_to);
    p.fill(sx, wy, ex - sx, wh, c.withAlpha(0.12f));
    // The pad as it is heard: a head a voice, the newest with its wake (not on Cloud, whose grains are the picture).
    StageHeads heads = page.kind != StageKind::Cloud ? stage_heads(s, t.last_pad % sampler_pads, pad, frames) : StageHeads {};
    const PlayHead* heard = heads.newest;
    // Cloud (mock 6): its grains as heard; the bars a live grain covers lighten.
    CloudGrains cloud = page.kind == StageKind::Cloud ? cloud_grains(s, t.last_pad % sampler_pads, pad, frames, from, to, ww) : CloudGrains {};
    if (page.kind == StageKind::Cloud)
        waveform(p, wx, wy, ww, wh, s.sample.overview, wave_sides(s), from, to, region_from, region_to, c, cloud.wakes.data(),
                 cloud.wake_count, WaveDetail {});
    else waveform(p, wx, wy, ww, wh, s.sample.overview, wave_sides(s), from, to, region_from, region_to, c, heads.wave.data(), heads.count,
                  detail);
    // A loop going round: for a moment a ghost stays at the place the head left (drawn last), so the jump reads as one.
    const float wrap = heard && (heard->flags & tap::Loop) ? heard->wrap : 0.0f;
    float loop_x = -1;
    if (page.kind == StageKind::Follow && end > start) { // follow A: the grid on the wave
        // The project's beats over the pad's region in amber (the bar's first brighter), as many as # Beats says or
        // its length at the tempo; the sample's own hits as dots above, each joined to the beat it is moved onto.
        double beat_frames = dsp::model_rate * 600.0 / std::max(1u, s.tempo);
        unsigned beats = pad.beats ? pad.beats : unsigned(std::max(1L, std::lround((end - start) / beat_frames)));
        beats = std::min(beats, max_pad_beats);
        unsigned per_bar = std::max(1u, s.project.bar_ticks() / std::max(1u, s.project.beat_ticks()));
        auto x_of = [&](double frame) { return wx + ww * float((frame / frames - from) / (to - from)); };
        float xs = x_of(start), xe = x_of(end);
        for (unsigned b = 0; b <= beats; ++b) {
            float bx = xs + (xe - xs) * float(b) / float(beats);
            p.fill(bx - 0.5f, wy, 1, wh, colour::amber.withAlpha(b % per_bar == 0 ? 0.85f : 0.4f));
        }
        for (unsigned n = 0; n < std::min<unsigned>(s.sample.hit_count, max_hits); ++n) {
            const auto& hit = s.sample.hits[n];
            if (hit.frame < start || hit.frame >= end) continue;
            float hx = x_of(hit.frame);
            unsigned onto = unsigned(std::lround(double(hit.frame - start) / double(end - start) * beats));
            float bx = xs + (xe - xs) * float(onto) / float(beats);
            juce::Path join;
            join.startNewSubPath(hx, wy - 5); join.lineTo(bx, wy + 3);
            p.g.setColour(c);
            p.g.strokePath(join, juce::PathStrokeType(1.0f));
            p.circle(hx, wy - 5, 2.6f, c);
        }
    }
    if (page.kind == StageKind::Trim) { // start, end and loop, each in the colour of its card (the flags over the wave)
        p.fill(sx, wy, 1.5f, wh, knob_shade(s.ui.track, 0));
        p.fill(ex - 1.5f, wy, 1.5f, wh, knob_shade(s.ui.track, 1));
        if (!pad.loop_lock || pad.loop) { // the loop point: dashed
            loop_x = x_of(float(std::clamp(pad.loop_point(), start, end)) / frames);
            auto lc = pad.loop ? knob_shade(s.ui.track, 2) : knob_shade(s.ui.track, 2).withAlpha(0.5f);
            for (float d = wy; d < wy + wh; d += 6) p.fill(loop_x, d, 1.4f, 3, lc);
            if (wrap > 0) p.fill(loop_x - 3, wy, 7, wh, knob_shade(s.ui.track, 2).withAlpha(0.25f * wrap)); // lit as it goes round
        }
    }
    double length = (end - start) / 44100.0;
    float ox = sx, ow = std::max(ex - sx, 8.0f);
    if (page.kind == StageKind::AmpEnv || page.kind == StageKind::FiltEnv) {
        bool amp = page.kind == StageKind::AmpEnv;
        bool gate = pad.play == PadPlay::Gate, from_end = !gate && !pad.decay_from_start;
        int a = amp ? pad.attack : pad.filter_attack, d = amp ? pad.decay : pad.filter_decay, rl = amp ? pad.release : pad.filter_release;
        float sustain = (amp ? pad.sustain : pad.filter_sustain) / 100.0f;
        bool hold = amp && d >= 100;
        double decay = time_seconds(d) * (amp ? t.params[sampler::Decay] / 100.0 : 1.0);
        float soft = amp ? 1.0f - 0.6f * pad.velocity_sense / 100.0f : 1.0f; // K3: how much softer a soft hit plays
        draw_envelope(p, ox, wy, ow, wh, length, time_seconds(a) * 0.3, hold ? length : decay, hold ? 1.0f : sustain,
                      time_seconds(rl) * 0.5, from_end && !hold, s.ui.track, soft);
    }

    float cloud_x = -1;
    if (page.kind == StageKind::Cloud) { // POSITION the line (K1), SIZE the band round it (K2), Spread a paler band, Scan an arrow
        cloud_x = x_of(region_from + (region_to - region_from) * t.params[sampler::Position] / 100.0f);
        float size = std::max(3.0f, ww / (to - from) * float(0.01 * std::pow(100.0, t.params[sampler::Size] / 100.0) * 44100 / frames));
        float spread = size * (1 + 3 * t.params[sampler::Spread] / 100.0f);
        auto shade = t.params[sampler::Cloud] ? 1.0f : 0.5f;
        // While a cloud is heard its window travels with the grains (Scan); K1's line stays where K1 put it.
        float centre = cloud.centre >= 0 ? x_of(float(cloud.centre / frames)) : cloud_x;
        p.fill(centre - spread / 2, wy, spread, wh, knob_shade(s.ui.track, 1).withAlpha(0.08f * shade));
        p.fill(centre - size / 2, wy, size, wh, knob_shade(s.ui.track, 1).withAlpha(0.18f * shade));
        p.fill(cloud_x - 0.75f, wy, 1.5f, wh, knob_shade(s.ui.track, 0).withMultipliedAlpha(shade));
        if (cloud.view.count || cloud.view.voice) draw_grains(p, wx, wy, ww, wh, from, to, frames, cloud, knob_shade(s.ui.track, 1));
        else
            for (unsigned g = 0; g < 14; ++g) { // still: grains as dots inside the window, a picture of what it does
                float gx = centre + std::sin(g * 2.3f) * 0.45f * size, gy = wy + 6 + (wh - 12) * std::fmod(g * 0.37f + 0.11f, 1.0f);
                p.circle(gx, gy, 1.2f + 0.8f * std::fmod(g * 0.61f, 1.0f), knob_shade(s.ui.track, 1).withMultipliedAlpha(shade * (0.4f + 0.6f * std::fmod(g * 0.29f, 1.0f))));
            }
    }
    if (wrap > 0) p.fill(x_of(float(wrap_left(*heard)) / frames) - 0.75f, wy, 1.5f, wh, colour::text.withAlpha(0.55f * wrap));
    p.g.restoreState();
    draw_heads(p, wx, wy, ww, wh, from, to, heads.wave.data(), heads.count);
    if (page.kind == StageKind::Trim) { // the flags, in the cards' colours: no letters needed
        icon(p, Icon::Flag, sx - 2, wy - 14, 12, knob_shade(s.ui.track, 0), 1.5f);
        icon(p, Icon::Flag, ex - 2, wy - 14, 12, knob_shade(s.ui.track, 1), 1.5f);
        if (loop_x >= 0) icon(p, Icon::Flag, loop_x - 2, wy - 14, 12, knob_shade(s.ui.track, 2).withAlpha(pad.loop ? 1.0f : 0.5f), 1.5f);
    }
    if (page.kind == StageKind::Cloud && t.params[sampler::Scan] > 0) { // Scan: the way the window travels
        juce::Path arrow;
        float ay = wy - 7, ax = cloud_x + 6, bx = ax + 8 + 18 * t.params[sampler::Scan] / 100.0f;
        arrow.startNewSubPath(ax, ay); arrow.lineTo(bx, ay);
        arrow.startNewSubPath(bx - 4, ay - 3.5f); arrow.lineTo(bx, ay); arrow.lineTo(bx - 4, ay + 3.5f);
        p.g.setColour(knob_shade(s.ui.track, 0));
        p.g.strokePath(arrow, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    if (band) { // the filter's own band: what it does to each frequency, low to high
        float by = wy + wh + 4, bh = full_h - wh - 4;
        p.fill(wx, by, ww, bh, colour::panel, 2);
        float hz, q;
        unsigned shape;
        bool on = true;
        if (page.kind == StageKind::Filter) {
            hz = 20.0f * std::exp2(pad.cutoff / 100.0f * 9.97f); q = 0.7f + pad.resonance * 0.06f;
            shape = pad.filter == PadFilter::Hpf2 || pad.filter == PadFilter::Hpf4 ? 2 : pad.filter == PadFilter::Bpf2 || pad.filter == PadFilter::Bpf4 ? 1 : 0;
            on = pad.filter != PadFilter::Off;
        } else { // the one filter after every pad
            hz = 20.0f * std::exp2(t.params[sampler::Filter] / 100.0f * 9.97f); q = 0.7f + t.params[sampler::Resonance] * 0.06f;
            shape = unsigned(t.params[sampler::FilterType]) % 3;
        }
        if (on) draw_filter_curve(p, wx, by, ww, bh, shape, 2, hz, q, knob_shade(s.ui.track, 1), knob_shade(s.ui.track, 0), false);
        else p.fill(wx, by + bh / 2, ww, 1, colour::dim);
    }
    if (page.kind == StageKind::Tune) { // a pitch ruler under the wave: two octaves each way, the pad's pitch marked (K1)
        float ry = wy + wh - 8, cx = wx + ww / 2, per = ww / 50;
        p.fill(wx, ry - 6, ww, 14, colour::bg.withAlpha(0.85f));
        p.fill(wx, ry, ww, 1, colour::line);
        for (int st = -24; st <= 24; ++st) {
            float tx = cx + st * per, th = st % 12 == 0 ? 7.0f : 3.0f;
            p.fill(tx - 0.5f, ry - th / 2, 1, th, st == 0 ? colour::sec : colour::dim);
        }
        float at = cx + (pad.pitch + pad.fine / 100.0f) * per;
        juce::Path mark;
        mark.addTriangle(at - 4.5f, ry - 7, at + 4.5f, ry - 7, at, ry);
        p.g.setColour(knob_shade(s.ui.track, 0)); p.g.fillPath(mark);
    }
    modes();
    // The cards carry the values; the row says only what they cannot: on Trim the length kept (and the file's after
    // it, dim), on Cloud where the grains come from, at its line.
    juce::String middle;
    auto right_pair = [&](const juce::String& lead, const juce::String& rest) {
        auto a_style = plain(colour::text, type::small, Weight::SemiBold), b_style = plain(colour::dim, type::small, Weight::SemiBold);
        float bw = p.width(rest, b_style), aw = p.width(lead, a_style), rx = px + pw;
        p.text(rest, rx - bw, bottom - 16, bw + 1, 14, b_style);
        p.text(lead, rx - bw - 8 - aw, bottom - 16, aw + 1, 14, a_style);
    };
    // While it plays, the middle label in two parts: what is happening in the text colour, the rest in mid.
    auto two_part = [&](const juce::String& lead_text, const juce::String& rest_text) {
        axis(p, bottom - 16, "", "", seconds(uint32_t((to - from) * frames)));
        auto lead = caps_text(colour::text), rest = caps_text(colour::mid);
        float a = p.width(lead_text, lead), b = p.width(rest_text, rest), x0 = px + (pw - a - b) / 2;
        p.text(lead_text, x0, bottom - 16, a + 1, 14, lead);
        p.text(rest_text, x0 + a, bottom - 16, b + 1, 14, rest);
    };
    if (page.kind == StageKind::Trim && heard) { // the time heard, of the time kept; going round, which pass
        double into = heard->reverse() ? double(end) - heard->at : heard->at - double(start);
        juce::String now = seconds(uint32_t(std::clamp(into, 0.0, double(end - start))));
        if ((heard->flags & tap::Loop) && heard->pass) two_part("PASS " + juce::String(heard->pass + 1), dot + now);
        else two_part(now, " OF " + seconds(end - start));
        return;
    }
    if (page.kind == StageKind::Cloud) { // where the grains come from (or are now), at the line; the file's length dim
        double at = cloud.centre >= 0 && cloud.view.sounding() ? cloud.centre : double(start) + (end - start) * t.params[sampler::Position] / 100.0;
        juce::String place = seconds(uint32_t(at)), whole = seconds(zoomed ? end - start : uint32_t((to - from) * frames));
        auto a_style = plain(colour::text, type::small, Weight::SemiBold), b_style = plain(colour::dim, type::small, Weight::SemiBold);
        float aw = p.width(place, a_style), bw = p.width(whole, b_style);
        float lx = std::clamp(cloud_x - aw / 2, px + 70, px + pw - bw - 10 - aw);
        p.text(place, lx, bottom - 16, aw + 1, 14, a_style);
        p.text(whole, px + pw - bw, bottom - 16, bw + 1, 14, b_style);
        return;
    }
    if (page.kind == StageKind::Trim) { right_pair(seconds(end - start), seconds(frames)); return; }
    if (page.kind == StageKind::Play && heads.sounding) { // how many voices sound
        two_part(juce::String(heads.sounding), heads.sounding == 1 ? " VOICE" : " VOICES");
        return;
    }
    axis(p, bottom - 16, "", middle, seconds(zoomed ? end - start : uint32_t((to - from) * frames)));
}

// DEVICE (MIDI; round 2, midi-track A): the route, alive (common.cpp's midi_route(), Edit's DEVICE alike).
void route_picture(Paint& p, const Snapshot& s) { midi_route(p, s, px, top + 4, pw, bottom - top - 8); }

// PROGRAM (MIDI): the program sent, big, and its bank.
void program_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    using namespace midi_track;
    int program = t.params[Program], msb = t.params[BankMsb], lsb = t.params[BankLsb];
    // The program sent, big (or Off), the bank under it when one is set.
    p.text(program ? juce::String(program) : juce::String("Off"), px, top + 30, pw, 48, plain(program ? colour_of(s) : colour::mid, 44, Weight::SemiBold),
           juce::Justification::centred);
    auto bank = [](int v) { return v ? juce::String(v - 1) : juce::String("-"); };
    if (program && (msb || lsb)) p.text("BANK " + bank(msb) + " / " + bank(lsb), px, top + 86, pw, 14, caps_text(colour::sec), juce::Justification::centred);
}

// CC (MIDI): the three controllers with their General MIDI names.
void cc_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    using namespace midi_track;
    std::vector<Row> list;
    for (unsigned n = 0; n < 3; ++n) {
        int number = t.params[size_t(CcNumber1 + int(n))], value = t.params[size_t(Cc1 + int(n))];
        const char* name = cc_name(unsigned(number));
        list.push_back({"CC " + juce::String(number), name ? juce::String(name) : juce::String("Controller"), juce::String(value), value / 127.0f, false,
                        knob_shade(s.ui.track, n)});
    }
    rows(p, {}, list, 44); // DEVICE says where they go
}

// An empty track: what it can become, as a carousel; the wheel moves along it, Push makes the card in the middle.
void new_track_picture(Paint& p, const Snapshot& s) {
    p.fill(0, zone::picture_y, zone::width, zone::height - zone::picture_y, colour::bg); // the knob strip too: no pots here
    juce::String channel = juce::String::fromUTF8("MIDI \xc2\xb7 CH ") + juce::String(std::clamp<int>(s.ui.new_channel, 1, 16));
    std::vector<CarouselCard> cards(new_choice_count);
    cards[size_t(NewChoice::Plugin)] = {Icon::Plug, "SYNTH", "Plugin", "VST CLAP LV2"};
    cards[size_t(NewChoice::Kit)] = {Icon::Pads, "KIT", "Empty", "NO SAMPLES"};
    cards[size_t(NewChoice::Samples)] = {Icon::Folder, "KIT", "Card", "SAMPLES"};
    cards[size_t(NewChoice::Record)] = {Icon::Mic, "KIT", "Input", "RECORD"};
    cards[size_t(NewChoice::MidiOut)] = {Icon::Din, "MIDI", "Out jack", channel};
    cards[size_t(NewChoice::MidiUsb)] = {Icon::Din, "MIDI", "USB-C", channel};
    cards[size_t(NewChoice::MidiAllUsb)] = {Icon::Din, "MIDI", "All USB", channel};
    carousel(p, cards, new_pick_shown(s.ui, s.now), colour_of(s), zone::picture_y + 8, zone::height - zone::picture_y - 12);
}

void bars_picture(Paint& p, const Snapshot& s, const StagePage& page, bool shift_row, float y0 = top) {
    std::vector<Row> list;
    const auto& ids = shift_row ? page.shift : page.params;
    for (unsigned n = 0; n < 3; ++n) if (ids[n] >= 0) list.push_back(param_row(s, ids[n], n, !shift_row));
    if (list.empty()) return;
    rows(p, {}, list, 96, y0, bottom);
    if (shift_row) icon(p, Icon::Shift, px, y0 + 6, 12, colour::mid, 1.4f); // the rows Shift + K1-K3 turn
}

// COLOUR (a kit): the bank as its pads sit, each filled with the colour it lights in, the last pad ringed. K1 and the
// wheel colour the last pad; a hit picks another, Minus / Plus another bank.
void colours_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const unsigned bank = t.bank % sampler_banks;
    constexpr float gap = 4, y0 = top + 4, w = (pw - 3 * gap) / 4, h = (bottom - top - 8 - 3 * gap) / 4;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned kit = bank * 16 + n, row = 3 - n / 4, column = n % 4;
        const float x = px + column * (w + gap), y = y0 + row * (h + gap);
        const bool used = t.pads[kit].used();
        const auto fill = used ? pad_colour(s, kit) : colour::surf;
        p.fill(x, y, w, h, fill, 3);
        if (kit == t.last_pad) p.ring(x - 2, y - 2, w + 4, h + 4, colour::text, 5, 2);
        char label[4]; pad_label(kit, label);
        auto name = used ? pad_name(s, kit, true) : juce::String(label);
        auto style = plain(used ? ink_on(fill) : colour::dim, type::small, Weight::SemiBold);
        p.text(name, x + 5, y + h - 15, w - 10, 13, p.fit(name, style, w - 10, type::floor));
    }
}

// Freeze plan F2c: Stage of a frozen track (its sound pages; MIX and SENDS stay live): the print of the pattern
// playing, bar by bar with the playhead, over FREEZE and PADS PLAY LIVE and what the print holds. A deep freeze
// (F2d): DEEP FREEZE and PADS OFF, or THAWING while its plug-ins load again.
bool frozen_view(const Snapshot& s, const StagePage& page) {
    const auto& f = s.freeze[s.ui.track % track_count];
    return f.frozen() && s.track().used() && page.picture != StagePicture::Mix && page.picture != StagePicture::Sends
        && page.picture != StagePicture::Colours; // a pad's colour is not in the print
}

void frozen_picture(Paint& p, const Snapshot& s) {
    const unsigned t = s.ui.track % track_count;
    const auto& f = s.freeze[t];
    const auto c = colour_of(s);
    unsigned live = (s.playing ? s.playing_patterns[t] : s.track().pattern) % pattern_count;
    const auto& sketch = s.sketches[t][live];
    const bool printed = f.plays(live) && s.print_peaks_known && (!s.playing || ((s.printing_tracks >> t) & 1)); // stopped: what will play
    const float x = 12, w = zone::width - 24, y = top + 16, h = 74;
    unsigned bar = std::max(1u, s.project.bar_ticks()), bars = std::max(1u, unsigned((sketch.length + bar - 1) / bar));
    unsigned every = bars <= 8 ? 1 : bars <= 16 ? 2 : bars <= 32 ? 4 : 8; // bar numbers a label's room apart
    p.fill(x, y, w, h, colour::quiet, 4);
    for (unsigned k = 0; k < bars; ++k) {
        float bx = x + w * float(k) / float(bars);
        if (k) p.fill(bx, y, 0.6f, h, colour::line);
        if (k % every == 0) p.text(juce::String(k + 1), bx + 3, top + 2, 24, 12, plain(colour::mid, type::small, Weight::SemiBold));
    }
    if (printed) {
        float cw = w / float(print_peak_columns), mid = y + h / 2;
        for (unsigned n = 0; n < print_peak_columns; ++n) {
            float level = std::sqrt(s.print_peaks[n] / 255.0f); // as loud ones are heard: soft tails still show
            float bh = std::max(1.0f, level * (h - 6));
            p.fill(x + n * cw + 0.4f, mid - bh / 2, std::max(1.0f, cw - 0.8f), bh, c.withAlpha(0.92f), 0.5f);
        }
    } else { // that pattern plays live (not frozen, or its print not ready yet)
        bool waiting = f.plays(live) && s.print.busy && s.print.track == t;
        p.text(waiting ? "PRINTING" : "LIVE", x, y, w, h, caps_text(waiting ? colour::ice : colour::mid), juce::Justification::centred);
    }
    if (s.playing && s.positions[t] >= 0 && sketch.length) { // the playhead
        float at = x + w * float(uint32_t(s.positions[t]) % sketch.length) / float(sketch.length);
        p.fill(at, y - 2, 1.2f, h + 4, colour::text);
    }
    // FREEZE and what still plays live, then what the print holds.
    auto ice = caps_text(colour::ice), mid = caps_text(colour::mid);
    const bool deep = f.state == FreezeState::Deep, thawing = (s.thawing >> t) & 1;
    juce::String freeze = deep ? "DEEP FREEZE" : "FREEZE", pads = thawing ? "THAWING" : deep ? "PADS OFF" : "PADS PLAY LIVE";
    if (thawing) mid = caps_text(colour::ice);
    float fw = p.width(freeze, ice), gw = p.width(pads, mid), lx = (zone::width - (14 + fw + 14 + gw)) / 2, ly = y + h + 8;
    icon(p, deep ? Icon::DeepSnow : Icon::Snow, lx, ly + 1, 11, colour::ice, 1.5f);
    p.text(freeze, lx + 14, ly, fw + 2, 13, ice);
    p.text(pads, lx + 14 + fw + 14, ly, gw + 2, 13, mid);
    juce::String held = juce::String(f.tap == PrintTap::PreFx ? "PRE FX" : "POST FX") + dot
                      + (f.tail == PrintTail::Ring ? "RING" : f.tail == PrintTail::Cut ? "CUT" : "LOOP") + dot + juce::String(bars) + (bars == 1 ? " BAR" : " BARS");
    p.text(held, px, ly + 17, pw, 13, mid, juce::Justification::centredTop);
}

bool plugin_has_presets(const Snapshot& s);

ui::SoftKeys stage_keys(const Snapshot& s) {
    const auto& t = s.track();
    const auto& g = stage_groups(t.engine);
    auto at = stage_at(s.ui, t);
    std::array<juce::String, 3> labels;
    SoftKeys keys {labels, int(at.group), colour_of(s)};
    for (unsigned n = 0; n < 3; ++n) {
        if (!stage_has(g, n)) continue; // no pages: a blank key
        unsigned sub = n == at.group ? at.sub : 0;
        keys.labels[n] = stage_page_at(t, n, sub).key;
        if (stage_page_at(t, n, sub).kind == StageKind::PluginSound && !plugin_has_presets(s)) keys.labels[n] = "PLUG-IN"; // its face
        if (stage_page_at(t, n, sub).kind == StageKind::Trim && instrument_pad(s)) keys.labels[n] = "ZONES"; // round 3 "instrument C"
        keys.dots[n] = uint8_t(stage_count(g, n) > 1 ? stage_count(g, n) : 0);
        keys.dot_at[n] = uint8_t(sub);
    }
    return keys;
}

// Whether the track's plug-in is open and has presets (Stage B1's first page is PRESETS, else its face).
bool plugin_ready(const Snapshot& s) {
    const auto& panel = s.ui.plugin_panel;
    return panel.entry == s.track().plugin && panel.status == PluginStatus::Ready;
}
bool plugin_has_presets(const Snapshot& s) { return plugin_ready(s) && s.ui.plugin_panel.programs > 0; } // B1's name: PRESETS
bool plugin_presets(const Snapshot& s) { return plugin_has_presets(s) && !s.presets.list.empty(); }          // and the list to show
// PRESETS: the preset in view, while turning the one under the cursor, else the one it plays (-1: none yet).
int plugin_preset_at(const Snapshot& s) {
    const auto& list = s.presets.list;
    if (s.ui.preset_due && s.ui.presets_entry == s.track().plugin) return std::min<int>(s.ui.presets_cursor, int(list.size()) - 1);
    for (size_t n = 0; n < list.size(); ++n) if (list[n].index == s.presets.current) return int(n);
    return -1;
}

// PRESETS (hubs plan, PL-a): the preset's name at poster size, the plug-in under it, the presets before and after
// it small in the corners, as PRESET's sound poster. Without presets the plug-in's face: its name, its format and the
// share of each block it takes. While it opens, or when it failed, the plug-in view says so (pages/plugin_view.cpp).
void plugin_sound_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& panel = s.ui.plugin_panel;
    if (!plugin_ready(s)) { draw_plugin_view(p, s, t.plugin, top); return; }
    auto poster_at = [&](const juce::String& name, juce::Colour colour) {
        auto poster = p.fit(name, plain(colour, 30, Weight::SemiBold), pw - 12, type::title);
        p.text(name, px + 4, top + 14, pw - 12, 36, poster);
    };
    juce::String format = juce::String::fromUTF8(panel.format);
    if (!plugin_presets(s)) {
        poster_at(plugin_name(s, t.plugin), colour::text);
        p.text(format, px + 6, top + 54, pw - 12, 16, plain(colour::sec, type::body));
        float load = panel.load / 1000.0f, bar = 80;
        p.fill(px + 6, top + 80, bar, 3, colour::line, 1.5f);
        p.fill(px + 6, top + 80, bar * std::clamp(load, 0.0f, 1.0f), 3, load_colour(load), 1.5f);
        p.text(juce::String(panel.load / 10) + " %", px + 14 + bar, top + 74, 60, 14, plain(colour::mid, type::small));
        return;
    }
    const auto& list = s.presets.list;
    int at = plugin_preset_at(s);
    auto name_of = [&](int n) { return juce::String::fromUTF8(list[size_t(n)].name.c_str()); };
    if (at < 0) poster_at("No preset yet", colour::sec);
    else poster_at(name_of(at), s.ui.preset_due ? colour::amber : colour::text);
    juce::String sub = plugin_name(s, t.plugin) + (format.isNotEmpty() ? dot + format : juce::String());
    p.text(sub, px + 6, top + 54, pw - 12, 16, p.fit(sub, plain(colour::sec, type::body), pw - 12, type::floor));
    auto side = plain(colour::mid, type::body);
    if (at > 0) {
        icon(p, Icon::ChevronLeft, px, bottom - 16, 10, colour::dim, 1.6f);
        p.text(name_of(at - 1), px + 13, bottom - 19, pw / 2 - 20, 16, p.fit(name_of(at - 1), side, pw / 2 - 20, type::floor));
    }
    if (at + 1 < int(list.size())) {
        icon(p, Icon::ChevronRight, px + pw - 10, bottom - 16, 10, colour::dim, 1.6f);
        p.text(name_of(at + 1), px + pw / 2, bottom - 19, pw / 2 - 13, 16, p.fit(name_of(at + 1), side, pw / 2 - 20, type::floor),
               juce::Justification::centredRight);
    }
}

// Cloud's on or off, as an LED in the header (its K3 on Shift turns it).
HeaderItem cloud_pill(const Snapshot& s) {
    bool on = s.track().params[sampler::Cloud];
    return pill(on ? Icon::Rec : Icon::Ring, "CLOUD", on ? colour_of(s) : colour::mid);
}
// Follow (round 2, follow A): on or off as an LED in the header, as Cloud's.
HeaderItem follow_pill(const Snapshot& s) {
    bool on = s.track().params[sampler::Follow];
    return pill(on ? Icon::Rec : Icon::Ring, "FOLLOW", on ? colour_of(s) : colour::mid);
}

// The kit header pill, Stage's and the sample pages' alike: the pad and its sample.
HeaderItem kit_pill(const Snapshot& s) {
    const auto& t = s.track();
    char label[4];
    pad_label(t.last_pad, label);
    auto name = pad_name(s, t.last_pad % sampler_pads, false);
    return pill(juce::String(label) + (name.isNotEmpty() ? dot + name_case(name) : dot + "EMPTY"), colour_of(s));
}

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& page = stage_page_now(s.ui, t);
    auto c = colour_of(s);
    Header h = header_of(s);
    bool kit_sends = page.kind == StageKind::Sends && t.engine == EngineKind::Sampler; // the last pad's own sends
    if (stage_kit_page(page.kind) != PageId::Count || page.kind == StageKind::Colour || kit_sends) {
        h.left.push_back(kit_pill(s));
        if (page.kind == StageKind::Cloud) h.left.push_back(cloud_pill(s));
        if (page.kind == StageKind::Follow) h.left.push_back(follow_pill(s));
        if (page.kind == StageKind::Layers) h.left.push_back(layers_pill(s));
    } else if (page.kind == StageKind::PluginSound && plugin_presets(s)) { // which preset of how many
        int at = plugin_preset_at(s);
        h.left.push_back(pill((at >= 0 ? juce::String(at + 1) : juce::String::fromUTF8("\xe2\x80\x93")) + " / " + juce::String(int(s.presets.list.size()))));
    } else if (page.kind == StageKind::PluginEdit && plugin_ready(s)) { // the share of each audio block it takes, as Edit's
        const auto& panel = s.ui.plugin_panel;
        h.left.push_back(pill(Icon::Chip, juce::String(panel.load / 10) + " %", panel.load > 700 ? colour::amber : colour::mid));
    } else if (page.kind == StageKind::Arp || page.kind == StageKind::Chord) { // on or off, as the LED on the MPC would say
        bool on = page.kind == StageKind::Arp ? t.arp : t.chord;
        h.left.push_back(pill(on ? Icon::Check : Icon::None, on ? "ON" : "OFF", on ? c : colour::mid));
    }
    const bool frozen = frozen_view(s, page); // freeze plan F2c
    if (frozen) {
        h.left.clear();
        unsigned live = (s.playing ? s.playing_patterns[s.ui.track % track_count] : t.pattern) % pattern_count;
        const unsigned t = s.ui.track % track_count;
        const bool deep = s.freeze[t].state == FreezeState::Deep;
        h.left.push_back(chip(deep ? Icon::DeepSnow : Icon::Snow, "P" + juce::String(live + 1), colour::ice));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    auto keys = stage_keys(s);
    if (frozen && s.ui.shift) { // freeze plan F2d: B3 DEEP FREEZE, the plug-ins let go
        bool deep = s.freeze[s.ui.track % track_count].state == FreezeState::Deep;
        keys = SoftKeys {{"", "", deep ? "" : "DEEP FREEZE"}, -1, c};
        if (!deep) { keys.glyphs[2] = Icon::DeepSnow; keys.tints[2] = colour::ice; }
    }
    if (s.ui.shift && page.kind == StageKind::Trim && t.engine == EngineKind::Sampler && !instrument_pad(s)) // the old Trim's Shift jobs on B1 and B2
        { keys = SoftKeys {{"LOOP LOCK", "NORMALIZE", ""}, -1, c}; keys.toggles = 1; keys.lit = sampler_pad(s).loop_lock ? 1 : 0; keys.leds[0] = c; }
    if (s.ui.shift && page.kind == StageKind::Layers && t.engine == EngineKind::Sampler) keys = layers_keys(s);
    if (!t.used()) { // what it becomes: B1 CANCEL to the nearest track that is not empty
        bool any = false;
        for (const auto& other : s.project.tracks) any |= other.used();
        keys = SoftKeys {{any ? "CANCEL" : "", "", ""}, -1, c, {any ? KeyIcon::Back : KeyIcon::None, KeyIcon::None, KeyIcon::None}};
    }
    frame(p, s, keys, h);
    if (!t.used()) { new_track_picture(p, s); return; }
    if (frozen) { frozen_picture(p, s); return; }
    switch (page.picture) {
    case StagePicture::Preset: preset_picture(p, s); break;
    case StagePicture::Filter: filter_picture(p, s, page); break;
    case StagePicture::Envelope: envelope_picture(p, s, page); break;
    case StagePicture::Lfo: lfo_picture(p, s, page); break;
    case StagePicture::ShiftRows: case StagePicture::Bars: bars_picture(p, s, page, page.picture == StagePicture::ShiftRows); break;
    case StagePicture::Mix: mix_picture(p, s); break;
    case StagePicture::Sends: sends_picture(p, s); break;
    case StagePicture::Arp: arp_picture(p, s); break;
    case StagePicture::Chord: chord_picture(p, s); break;
    case StagePicture::Fx: fx_picture(p, s); break;
    case StagePicture::Wave: wave_picture(p, s, page); break;
    case StagePicture::Route: route_picture(p, s); break;
    case StagePicture::Program: program_picture(p, s); break;
    case StagePicture::Cc: cc_picture(p, s); break;
    case StagePicture::PluginSound: plugin_sound_picture(p, s); break;
    case StagePicture::PluginEdit: draw_plugin_view(p, s, t.plugin, top); break;
    case StagePicture::Engines: case StagePicture::Sources: case StagePicture::Ports: break; // new_track_picture
    case StagePicture::Colours: colours_picture(p, s); break;
    }
}

void soft(Context& c, int key) {
    const auto& page = stage_page_now(c.ui, c.track());
    if (c.ui.shift && c.frozen(c.ui.track) && page.picture != StagePicture::Mix && page.picture != StagePicture::Sends
        && page.picture != StagePicture::Colours) {
        if (key == 2) c.deep_freeze(c.ui.track); // freeze plan F2d
        return;
    }
    const auto& last = c.track().pads[c.track().last_pad % sampler_pads];
    const bool zones = last.used() && c.project.samples[last.sample % sample_slots].instrument(); // ZONES: no Trim jobs
    if (c.ui.shift && page.kind == StageKind::Trim && c.track().engine == EngineKind::Sampler && !zones) {
        if (key == 0) c.toggle_loop_lock(); // Shift + B1, as on the MPC's Trim
        if (key == 1) c.normalize();        // Shift + B2
        return;
    }
    if (c.ui.shift && page.kind == StageKind::Layers && c.track().engine == EngineKind::Sampler) {
        if (key == 0) c.layer_load();  // Shift + B1: Samples, Push onto the layer in hand
        if (key == 1) c.layer_clear(); // Shift + B2
        return;
    }
    c.stage_tick(UINT32_MAX); // a sound still waiting to load lands first
    if (!c.track().used()) { // B1 CANCEL: the nearest track that is not empty, on the page this one was picked on
        if (key == 0) cancel_new_track(c);
        return;
    }
    if (stage_opens_edit(c.track(), unsigned(key))) { c.open(PageId::Edit); return; } // a MIDI track's B3 EDIT
    stage_press(c.ui, c.track(), unsigned(key));
}

void wheel(Context& c, int delta) {
    const auto& page = stage_page_now(c.ui, c.track());
    auto cards = page_bindings(c.project, c.ui, c.pattern(), false);
    if (page.kind == StageKind::Preset) { // the sounds, one a detent
        if (cards[0].kind == Binding::Kind::StageSound) c.preset_turn(cards[0].value + delta);
        return;
    }
    if (page.kind == StageKind::PluginSound) { c.plugin_preset_turn(delta); return; } // its presets (none: nothing)
    if (page.kind == StageKind::PluginEdit) { plugin_rows(c, delta); return; }       // its rows of three
    int n = stage_focus(c.ui, c.track());
    if (!c.track().used()) { // the carousel: Push makes the card in the middle
        new_pick_turn(c.ui, delta, c.now);
        return;
    }
    const auto& b = cards[size_t(n)];
    if (!b.bound()) return;
    c.fine_edit(b, delta); // Trim's handles by the view on screen, a frame zoomed all the way in
    auto& pot = c.ui.pots[size_t(n)];
    pot.engaged = false; pot.bound = 0; // the value left the pot: it picks it up again
    c.ui.turning = int8_t(n); c.ui.turned_at = c.now;
}

void push(Context& c) {
    auto& t = c.track();
    const auto& page = stage_page_now(c.ui, t);
    if (!t.used()) { make_new_track(c, NewChoice(c.ui.new_pick % new_choice_count)); return; } // the card in the middle
    if (page.kind == StageKind::Preset) { c.stage_tick(UINT32_MAX); c.open(PageId::Sounds); return; }
    if (page.kind == StageKind::Fx && !t.fx.count()) { // nothing yet: the picker on the track's chain, to add the first
        c.ui.fx_loop = -1;
        c.open(PageId::Rack);
        c.ui.fx_cursor[UiState::cursor_of(c.ui.track)] = 0;
        c.ui.picker_open = true; c.ui.picker_items = false; c.ui.picker_replaces = false;
        return;
    }
    if (page.kind == StageKind::Fx && t.fx.count()) { // the next effect of the chain
        auto& slot = c.ui.fx_cursor[UiState::cursor_of(c.ui.track)];
        slot = uint8_t((slot + 1) % t.fx.count());
        return;
    }
    if (page.kind == StageKind::PluginSound) { // the whole list (PRESETS; B1 there comes back)
        c.stage_tick(UINT32_MAX);
        if (t.plugin < max_plugins && c.ui.plugin_panel.entry == t.plugin && c.ui.plugin_panel.programs) c.open_plugin_presets(t.plugin);
        return;
    }
    auto cards = page_bindings(c.project, c.ui, c.pattern(), false);
    int from = stage_focus(c.ui, t);
    for (int k = 1; k <= 3; ++k) { // the next card that holds something
        int n = (from + k) % 3;
        if (cards[size_t(n)].bound()) {
            c.ui.last_pot = int8_t(n); c.ui.last_pot_shift = false; c.ui.turning = int8_t(n); c.ui.turned_at = c.now;
            focus_handle(c.ui, cards[size_t(n)]); // Trim: the view zoomed in goes to that handle
            return;
        }
    }
}

// Minus / Plus: a synth's octave; a kit's bank, as on its pages.
void nudge(Context& c, int way) {
    if (stage_page_now(c.ui, c.track()).kind == StageKind::PluginEdit) { plugin_rows(c, way); return; } // as Edit's
    if (c.track().engine == EngineKind::Sampler) sampler_bank(c, way);
    else octave(c, way);
}
} // namespace

// The ten sample pages draw with Stage's own kit picture (pages/sampler_pages.cpp and kin): one look per page,
// whichever way it was reached. `row`: the second row of cards on pages that have one, as two dots on the bottom row.
void kit_page(Paint& p, const Snapshot& s, StageKind kind, int row) {
    Header h = header_of(s);
    h.left.push_back(kit_pill(s));
    if (kind == StageKind::Cloud) h.left.push_back(cloud_pill(s));
    if (kind == StageKind::Follow) h.left.push_back(follow_pill(s));
    if (kind == StageKind::Layers) h.left.push_back(layers_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, s.ui.shift && kind == StageKind::Layers ? layers_keys(s) : sampler_keys(s.ui.page, colour_of(s)), h);
    StagePage page {"", kind, StagePicture::Wave};
    wave_picture(p, s, page);
    if (row >= 0) page_dots(p, zone::width - zone::pad - 64, bottom - 9, 2, row, colour_of(s), true); // beside the length
}

// Stage's B1-B3 for the selected track, as Sample held's pads (Pads, Keys) name them: where each opens Stage.
ui::SoftKeys sound_keys(const Snapshot& s) {
    auto keys = stage_keys(s);
    if (s.ui.page != PageId::Stage) keys.active = -1; // not on Stage: none lit
    if (!s.track().used()) keys = ui::SoftKeys {{"", "", ""}, -1, colour_of(s)};
    return keys;
}

const PageSpec stage_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
