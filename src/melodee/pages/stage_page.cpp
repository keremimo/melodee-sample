// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STAGE (Sample): the selected track's quick pages, as the MPC's Sample Mode.
// B1-B3 each hold a cycle of pages (engine/engine_info.h, stage_groups()):
// the lit one again steps to the next page, another jumps to its first; the
// soft keys name each button's page, with a dot a page under a cycle. A synth
// has SOUND (preset, tone, mix), SHAPE (envelopes, LFO) and PLAY (arp, chord,
// its own, effects); a kit Akai's Trim / Mix / Amp Env, Tune / Play and Filter /
// Filt Env on the last pad hit; a 909 its preset, the voice the last pad hit
// and levels, mix, effects; a MIDI track device and program, CC, arp and chord; an
// empty track what it becomes. The wheel fine-edits the card touched last
// (PRESET: walks the sounds; an empty track: walks its list), Push moves to the next card (PRESET: the Sounds
// list; FX: the next effect, or with none yet the effect picker; an empty track: makes it), Shift + Push is Back.
// The pads play the track; on a kit the pad played is the pad edited.
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sampler.h"
#include "melodee/model/chords.h"
#include "melodee/model/scales.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/stage_map.h"
#include "melodee/ui/waveform.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

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
void rows(Paint& p, const juce::String& head, const std::vector<Row>& list, float label_w) {
    float rh = 20, gap = 15, h = list.size() * rh + (list.size() - 1) * gap + (head.isNotEmpty() ? 22 : 0);
    float y = top + (zone::picture_h - h) / 2;
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

// PRESET: the sound before, the one in hand (turned to, or loaded) with its star, the one after; where it sits.
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
    auto number = [&](int n) { char text[16]; std::snprintf(text, sizeof(text), "%02d", n + 1); return juce::String(text); };
    float y = top + 13;
    auto side_row = [&](int n) {
        if (n >= 0 && n < int(count)) {
            p.text(number(int(list[size_t(n)])), px + 9, y, 22, 26, plain(colour::mid, type::small, Weight::SemiBold));
            auto name = name_of(list[size_t(n)]);
            p.text(name, px + 36, y, pw - 44, 26, p.fit(name, plain(colour::mid, type::list, Weight::Medium), pw - 44, type::floor));
        }
        y += 28;
    };
    side_row(at - 1);
    p.fill(px, y, pw, 36, colour::surf, 4);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(px), int(y), 3, 36));
    p.fill(px, y, 8, 36, c, 4);
    p.g.restoreState();
    unsigned sound = list[size_t(at)];
    bool starred = s.ui.settings.favourite(unsigned(t.engine), sound);
    p.text(number(int(sound)), px + 9, y, 22, 36, plain(colour::sec, type::small, Weight::SemiBold));
    auto name = name_of(sound);
    float room = pw - 44 - (starred ? 22 : 0);
    p.text(name, px + 36, y, room, 36, p.fit(name, plain(colour::text, type::title, Weight::SemiBold), room, type::floor));
    if (starred) star(p, px + pw - 14, y + 18, 7, c);
    y += 38;
    side_row(at + 1);
    float by = bottom - 16;
    juce::String where = number(at) + " / " + number(int(count) - 1);
    auto style = caps_text(colour::mid);
    float ww = p.width(where, style);
    p.fill(px, by + 5, pw - ww - 12, 3, colour::line, 1.5f);
    p.fill(px, by + 5, (pw - ww - 12) * (at + 1) / float(count), 3, c, 1.5f);
    p.text(where, px + pw - ww - 1, by, ww + 1, 14, style);
}

// TONE on a filter: the curve from 20 Hz to 20 kHz with the cutoff marked.
void filter_picture(Paint& p, const Snapshot& s, const StagePage& page) {
    const auto& t = s.track();
    int cut = t.params[size_t(page.params[0])], res = t.params[size_t(page.params[1])];
    float hz = 20.0f * std::exp2(cut / 100.0f * 9.97f), q = 0.7f + res * 0.06f;
    draw_filter_curve(p, px, top + 6, pw, zone::picture_h - 26, 0, 2, hz, q, colour_of(s), knob_shade(s.ui.track, 0));
}

// An envelope: attack, decay, sustain on K1-K3, release on Shift + K1; times on a square-root scale.
void envelope_picture(Paint& p, const Snapshot& s, const StagePage& page) {
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
    float x0 = px + 2, scale = (pw - 4) / total, y0 = top + 8, y1 = bottom - 8;
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
void lfo_picture(Paint& p, const Snapshot& s, const StagePage& page) {
    const auto& t = s.track();
    int shape = t.params[size_t(page.params[1])], amount = t.params[size_t(page.params[2])];
    float mid = top + zone::picture_h / 2.0f, amp = (zone::picture_h - 20) / 2.0f * std::max(0.15f, amount / 100.0f);
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

// An effect family's glyph: the chain slots, the bus a send goes to.
Icon family_icon(Family f) {
    switch (f) {
    case Family::Space: return Icon::Space;
    case Family::Time: return Icon::Time;
    case Family::Motion: return Icon::Modulation;
    case Family::Dirt: return Icon::Dirt;
    case Family::Tone: return Icon::Tone;
    case Family::Dynamics: return Icon::Dynamics;
    default: return Icon::Plug;
    }
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
    float heard = meter > 0 && !m.mute ? std::clamp((20 * std::log10(meter) + 48) / 48, 0.0f, 1.0f) : 0;
    if (heard > 0) p.fill(mx, my + mh * (1 - heard), 7, mh * heard, c, 3);
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
    const auto& bus = s.project.buses[m.bus % bus_count];
    const auto& first = effect_info(EffectKind(bus.fx.slots[0].kind));
    float by = top + 100, send = m.send / 100.0f;
    if (m.send > 0) {
        to_bus.startNewSubPath(from, cy + 4); to_bus.cubicTo(from + 30, cy + 4, bx - 30, by + 18, bx, by + 18);
        p.g.setColour(knob_shade(s.ui.track, 2));
        p.g.strokePath(to_bus, juce::PathStrokeType(1 + 3 * send, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    box(by, bus.fx.slots[0].used() ? family_icon(first.family) : Icon::Ring, m.send > 0 ? knob_shade(s.ui.track, 2) : colour::dim,
        "BUS " + juce::String::charToString(juce::juce_wchar('A' + m.bus % bus_count)), bus.fx.slots[0].used() ? juce::String(first.name) : juce::String());
}

// ARP: the notes it would play from the keys held (or an example chord), a column a step, beats marked.
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
    // A step a column, the beats' columns raised, pitch upward; nothing held: an example chord, faint.
    int lo = notes.front(), hi = std::max(notes.back(), lo + 12);
    float gy = top + 6, gh = zone::picture_h - 12, cw = (pw - 15 * 3) / 16.0f;
    auto ink = example ? c.withAlpha(0.35f) : c;
    for (unsigned n = 0; n < 16; ++n) {
        float x = px + n * (cw + 3);
        if (n % 4 == 0) p.fill(x, gy, cw, gh, colour::panel, 3);
        int key = order[n % order.size()];
        float y = gy + gh - 10 - (key - lo) / float(hi - lo) * (gh - 14);
        p.fill(x + 1.5f, y, cw - 3, 8, n % 4 == 0 ? ink : ink.withMultipliedAlpha(0.7f), 2);
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

// FX: the track's whole chain as a row of slots (a family glyph and a name each, the one the cards edit lit, empty
// ones open), then the one in hand: its name, family, and what it does drawn. An empty chain: Push and a plus.
void fx_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    unsigned count = t.fx.count(), size = std::clamp<unsigned>(t.fx.size, 1, max_slots), slot = s.ui.fx_cursor[UiState::cursor_of(s.ui.track)];
    constexpr float gap = 4, sy = top + 6, sh = 46;
    float sw = (pw - (size - 1) * gap) / size;
    for (unsigned n = 0; n < size; ++n) {
        float x = px + n * (sw + gap);
        if (n >= count) { // open: a quiet outline, the next one to fill with a plus
            p.ring(x + 0.5f, sy + 0.5f, sw - 1, sh - 1, n == count && !count ? c : colour::line, 4, n == count && !count ? 1.3f : 1);
            if (n == count) icon(p, Icon::Plus, x + sw / 2 - 7, sy + sh / 2 - 7, 14, count ? colour::dim : c, 1.8f);
            continue;
        }
        const auto& fx = t.fx.slots[n];
        const auto& info = effect_info(EffectKind(fx.kind));
        bool here = n == slot;
        p.fill(x, sy, sw, sh, here ? c.withAlpha(0.16f) : colour::surf, 4);
        if (here) p.ring(x, sy, sw, sh, c, 4, 1.2f);
        auto ink = fx.bypass ? colour::dim : here ? c : colour::sec;
        icon(p, family_icon(info.family), x + sw / 2 - 9, sy + 5, 18, ink, 1.6f);
        juce::String name = info.name;
        auto style = plain(fx.bypass ? colour::dim : here ? colour::text : colour::sec, type::small, Weight::Medium);
        p.text(name, x + 2, sy + 27, sw - 4, 14, p.fit(name, style, sw - 4, type::floor), juce::Justification::centred);
        if (fx.bypass) p.fill(x + 6, sy + sh / 2, sw - 12, 1.2f, colour::dim); // struck through
    }
    if (!count) { // nothing yet: Push adds one
        float mid = zone::width / 2.0f, gy = top + 86;
        icon(p, Icon::Push, mid - 28, gy, 26, colour::sec, 1.5f);
        icon(p, Icon::Plus, mid + 6, gy + 4, 18, colour::sec, 1.8f);
        return;
    }
    if (slot >= count) return;
    const auto& fx = t.fx.slots[slot];
    const auto& info = effect_info(EffectKind(fx.kind));
    juce::String name = info.name;
    auto title = plain(fx.bypass ? colour::mid : colour::text, type::title, Weight::SemiBold);
    float nw = p.text(name, px, top + 60, pw - 90, 26, p.fit(name, title, pw - 90, type::floor));
    auto fam = caps_text(colour::mid);
    juce::String family = juce::String(family_name(info.family)).toUpperCase();
    float fw = p.width(family, fam) + 8;
    p.fill(px + nw + 8, top + 66, fw, 15, colour::surf, 3);
    p.text(family, px + nw + 8, top + 66, fw, 15, fam, juce::Justification::centred);
    if (fx.bypass) draw_glyph(p, Glyph::LedOff, px + nw + 8 + fw + 6, top + 73.5f, colour::mid);
    // What it does: its picture as bars, as the chain cards draw it.
    float bx = px, by = top + 94, bw = pw, bh = bottom - 6 - by, step = bw / info.picture.size();
    for (size_t n = 0; n < info.picture.size(); ++n) {
        float h = std::max(2.0f, bh * info.picture[n] / 100.0f);
        p.fill(bx + n * step + 2, by + bh - h, step - 4, h, fx.bypass ? colour::dim : c.withAlpha(0.55f + 0.45f * (n % 2)), 2);
    }
}

juce::String seconds(uint32_t frames) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f s", frames / 44100.0);
    return text;
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
    if (pad.used() && s.sample.instrument) { instrument_picture(p, s, wx, wy, ww, wh); return; }
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
        if (page.kind == StageKind::Cloud || page.kind == StageKind::Follow) {
            bool on = t.params[page.kind == StageKind::Cloud ? sampler::Cloud : sampler::Follow];
            draw_glyph(p, on ? Glyph::Led : Glyph::LedOff, gx, gy + 6, on ? c : colour::dim);
            gx += 14;
        }
        if (page.kind == StageKind::Trim && s.ui.trim_zoom)
            p.text(juce::String::fromUTF8("\xc3\x97") + juce::String(1 << s.ui.trim_zoom), gx, gy - 1, 30, 14, caps_text(c));
    };
    // Volume and pan, as two thin meters at the sides.
    p.fill(px, wy, 5, wh, colour::line, 2.5f);
    float vol = pad.level / 127.0f;
    p.fill(px, wy + wh * (1 - vol), 5, wh * vol, c, 2.5f);
    p.fill(px + pw - 5, wy, 5, wh, colour::line, 2.5f);
    p.fill(px + pw - 7, wy + wh / 2 - 0.5f, 9, 1, colour::dim);
    p.fill(px + pw - 7, wy + wh / 2 - 3 + pad.pan / 50.0f * (wh / 2 - 3), 9, 6, knob_shade(s.ui.track, 2), 1.5f);

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
    if (page.kind == StageKind::Trim && s.ui.trim_zoom) { // a window round the handle Shift + K zoomed at
        float loop_at = float(std::clamp(pad.loop_point(), start, end)) / frames, window = 1.0f / float(1 << s.ui.trim_zoom);
        float centre = s.ui.trim_focus == 1 ? region_to : s.ui.trim_focus == 2 ? loop_at : region_from;
        from = std::clamp(centre - window / 2, 0.0f, 1.0f - window); to = from + window;
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
        waveform(p, wx, wy, ww, wh, s.sample.overview, from, to, region_from, region_to, c, cloud.wakes.data(), cloud.wake_count);
    else waveform(p, wx, wy, ww, wh, s.sample.overview, from, to, region_from, region_to, c, heads.wave.data(), heads.count);
    // A loop going round: for a moment a ghost stays at the place the head left (drawn last), so the jump reads as one.
    const float wrap = heard && (heard->flags & tap::Loop) ? heard->wrap : 0.0f;
    if (page.kind == StageKind::Trim) {
        p.fill(sx, wy, 1, wh, knob_shade(s.ui.track, 0));
        p.fill(sx - 2.5f, wy - 2.5f, 5, 5, knob_shade(s.ui.track, 0), 1);
        p.fill(ex - 1, wy, 1, wh, knob_shade(s.ui.track, 1));
        p.fill(ex - 3.5f, wy - 2.5f, 5, 5, knob_shade(s.ui.track, 1), 1);
        if (!pad.loop_lock || pad.loop) { // the loop point: dashed, a flag at its foot
            float lx = x_of(float(std::clamp(pad.loop_point(), start, end)) / frames);
            auto lc = pad.loop ? knob_shade(s.ui.track, 2) : knob_shade(s.ui.track, 2).withAlpha(0.5f);
            for (float d = wy; d < wy + wh; d += 6) p.fill(lx, d, 1, 3, lc);
            p.fill(lx, wy + wh - 5, 6, 5, lc, 1);
            if (wrap > 0) p.fill(lx - 1, wy + wh - 8, 9, 8, knob_shade(s.ui.track, 2).withAlpha(wrap), 1.5f); // lit as it goes round
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
        draw_envelope(p, ox, wy, ow, wh, length, time_seconds(a) * 0.3, hold ? length : decay, hold ? 1.0f : sustain,
                      time_seconds(rl) * 0.5, from_end && !hold, s.ui.track);
    }
    if (page.kind == StageKind::Filter) {
        float hz = 20.0f * std::exp2(pad.cutoff / 100.0f * 9.97f), q = 0.7f + pad.resonance * 0.06f;
        unsigned shape = pad.filter == PadFilter::Hpf2 || pad.filter == PadFilter::Hpf4 ? 2 : pad.filter == PadFilter::Bpf2 || pad.filter == PadFilter::Bpf4 ? 1 : 0;
        if (pad.filter != PadFilter::Off) draw_filter_curve(p, ox, wy, ow, wh, shape, 2, hz, q, knob_shade(s.ui.track, 1), knob_shade(s.ui.track, 0));
    }
    if (page.kind == StageKind::KitFilter) { // the one filter after every pad, over the whole sample
        float hz = 20.0f * std::exp2(t.params[sampler::Filter] / 100.0f * 9.97f), q = 0.7f + t.params[sampler::Resonance] * 0.06f;
        draw_filter_curve(p, wx, wy, ww, wh, unsigned(t.params[sampler::FilterType]) % 3, 2, hz, q, knob_shade(s.ui.track, 1), knob_shade(s.ui.track, 0));
    }
    if (page.kind == StageKind::Cloud) { // where the grains come from (K1), how long they are (K2), a few of them
        float at = x_of(region_from + (region_to - region_from) * t.params[sampler::Position] / 100.0f);
        float size = std::max(2.0f, ww / (to - from) * float(0.01 * std::pow(100.0, t.params[sampler::Size] / 100.0) * 44100 / frames));
        auto shade = t.params[sampler::Cloud] ? 1.0f : 0.5f;
        // While a cloud is heard its band travels with the grains (Scan); K1's line stays where K1 put it.
        float band = cloud.centre >= 0 ? x_of(float(cloud.centre / frames)) : at;
        p.fill(band - size / 2, wy, size, wh, colour::text.withAlpha(0.12f * shade));
        p.fill(at, wy, 1, wh, knob_shade(s.ui.track, 0).withMultipliedAlpha(shade));
        if (cloud.view.count || cloud.view.voice) draw_grains(p, wx, wy, ww, wh, from, to, frames, cloud, knob_shade(s.ui.track, 1));
        else
            for (unsigned g = 0; g < 9; ++g) { // still: a few grains round the place, as a picture of what it does
                float gx = at + std::sin(g * 2.3f) * 0.5f * size, gy = wy + wh * (0.25f + 0.5f * std::fmod(g * 0.37f, 1.0f));
                p.fill(gx, gy, 6, 3, knob_shade(s.ui.track, 1).withMultipliedAlpha(shade), 1.5f);
            }
    }
    if (wrap > 0) p.fill(x_of(float(wrap_left(*heard)) / frames) - 0.75f, wy, 1.5f, wh, colour::text.withAlpha(0.55f * wrap));
    p.g.restoreState();
    draw_heads(p, wx, wy, ww, wh, from, to, heads.wave.data(), heads.count);
    modes();
    // The cards carry the values; the row says only what they cannot: on Trim how much is kept, on Cloud where.
    juce::String middle = page.kind == StageKind::Trim ? "KEPT " + seconds(end - start) : juce::String();
    if (page.kind == StageKind::Cloud && t.params[sampler::Cloud])
        middle = "AROUND " + seconds(uint32_t(start + (end - start) * t.params[sampler::Position] / 100.0));
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
    if (page.kind == StageKind::Cloud && cloud.centre >= 0 && cloud.view.sounding()) { // where the grains are now
        two_part("GRAINS AROUND " + seconds(uint32_t(cloud.centre)), "");
        return;
    }
    if (page.kind == StageKind::Play && heads.sounding) { // how many voices sound
        two_part(juce::String(heads.sounding), heads.sounding == 1 ? " VOICE" : " VOICES");
        return;
    }
    axis(p, bottom - 16, "", middle, seconds(zoomed ? end - start : uint32_t((to - from) * frames)));
}

// VOICE (909): the pads as they sit, the voice's lit, the one hit brightest; the voice and its pads.
void voice_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& g = stage_groups(t.engine);
    auto c = colour_of(s);
    unsigned voice = stage_voice(t);
    int hit = int(t.last_note) - 36;
    float cell = 24, gap = 4, gx = px, gy = top + (zone::picture_h - (4 * cell + 3 * gap)) / 2;
    for (unsigned n = 0; n < 16; ++n) { // the pads the voice plays lit, the one hit brightest, each with its number
        unsigned row = 3 - n / 4, column = n % 4;
        bool same = g.pad_voice[n] == int(voice), here = int(n) == hit;
        juce::Colour fill = here ? c : same ? c.withAlpha(0.4f) : g.pad_voice[n] >= 0 ? colour::raised : colour::quiet;
        float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        p.fill(x, y, cell, cell, fill, 3);
        if (same) p.text(juce::String(n + 1), x, y, cell, cell, plain(here ? colour::bg : colour::text, type::small, Weight::SemiBold), juce::Justification::centred);
    }
    // The voice's name, and the drum the last pad played when the voice has more than one.
    float x = gx + 4 * cell + 3 * gap + 16, w = px + pw - x;
    juce::String name = engine_info(t.engine).pages[voice].title;
    p.text(name, x, top + 44, w, 30, p.fit(name, plain(colour::text, 24, Weight::SemiBold), w, type::floor));
    if (hit >= 0 && hit < int(kit909::Drums)) {
        juce::String drum = juce::String(kit909::drum_name(unsigned(hit))).toUpperCase();
        if (drum != name.toUpperCase()) p.text(drum, x, top + 78, w, 14, p.fit(drum, caps_text(c), w, type::floor));
    }
}

// LEVELS (909): six faders, the three on K1-K3 in their shades, the Shift three after them.
void levels_picture(Paint& p, const Snapshot& s, const StagePage& page) {
    const auto& t = s.track();
    const auto& info = engine_info(t.engine);
    float cw = pw / 6;
    for (unsigned n = 0; n < 6; ++n) {
        int id = n < 3 ? page.params[n] : page.shift[n - 3];
        if (id < 0) continue;
        float cx = px + n * cw + cw / 2, value = gauge_of(info.params[id], t.params[size_t(id)]);
        auto shade = n < 3 ? knob_shade(s.ui.track, n) : colour::mid;
        p.text(shown_value(t, id), cx - cw / 2, top + 2, cw, 18, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        float bh = zone::picture_h - 46, by = top + 24;
        p.fill(cx - 3.5f, by, 7, bh, colour::line, 3.5f);
        p.fill(cx - 3.5f, by + bh * (1 - value), 7, bh * value, n < 3 ? shade : knob_shade(s.ui.track, n - 3).withAlpha(0.55f), 3.5f);
        auto style = caps_text(shade);
        juce::String label = cell_name(p, info.params[id].label, style, cw); // "Cymbals" where the capitals do not fit
        if (label != juce::String(info.params[id].label)) style = plain(shade, type::small, Weight::SemiBold);
        p.text(label, cx - cw / 2, bottom - 18, cw, 14, style, juce::Justification::centred);
    }
}

// DEVICE (MIDI): the track, its port, its channel, the line lit as notes leave.
void route_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    bool sending = s.ui.track_out_at[s.ui.track] && s.now - s.ui.track_out_at[s.ui.track] < 150;
    static const char* ports[] {"External", "USB-C", "All USB"};
    struct Box { juce::String cap, text; bool lit; };
    Box boxes[] {{"TRACK", juce::String(s.ui.track + 1), true},
                 {"PORT", ports[unsigned(t.params[midi_track::Port]) % 3], false},
                 {"CHANNEL", juce::String(t.params[midi_track::Channel]), false}};
    float arrow = 22, bw = (pw - 2 * arrow) / 3, y = top + 30, h = 50;
    for (unsigned n = 0; n < 3; ++n) {
        float x = px + n * (bw + arrow);
        p.fill(x, y, bw, h, colour::surf, 4);
        if (sending && n) p.ring(x, y, bw, h, c, 4);
        p.text(boxes[n].cap, x + 8, y + 6, bw - 16, 14, caps_text(colour::mid));
        p.text(boxes[n].text, x + 8, y + 22, bw - 16, 22, p.fit(boxes[n].text, plain(n == 0 ? c : colour::text, type::list, Weight::SemiBold), bw - 16, type::floor));
        if (n == 2) break;
        float ax = x + bw + 4, ay = y + h / 2;
        juce::Path a;
        a.startNewSubPath(ax, ay); a.lineTo(ax + arrow - 8, ay);
        a.startNewSubPath(ax + arrow - 12, ay - 4); a.lineTo(ax + arrow - 8, ay); a.lineTo(ax + arrow - 12, ay + 4);
        p.g.setColour(sending ? c : colour::mid);
        p.g.strokePath(a, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // Where it lands, under the port: the computer, devices on USB (counted), or the jack; amber when nothing is there.
    unsigned outputs = s.ui.midi_outputs, devices = (outputs >> 8) & 255;
    Icon where = Icon::Jack;
    juce::String count;
    bool missing = false;
    switch (midi_track::dest_of(t.params[midi_track::Port])) {
    case MidiDest::UsbC: where = Icon::Laptop; missing = !(outputs & 2); break;
    case MidiDest::AllUsb: where = Icon::Usb; count = juce::String(devices + ((outputs & 2) ? 1 : 0)); missing = !devices && !(outputs & 2); break;
    default: break;
    }
    float wx = px + bw + arrow + bw / 2, wy = y + h + 14;
    icon(p, missing ? Icon::Warn : where, wx - 11 - (count.isNotEmpty() ? 10 : 0), wy, 22, missing ? colour::amber : colour::sec, 1.5f);
    if (count.isNotEmpty() && !missing) p.text(count, wx + 4, wy + 3, 30, 16, plain(colour::sec, type::body, Weight::SemiBold));
}

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

// An empty track's choosers.
void choice_rows(Paint& p, const Snapshot& s, const std::vector<std::pair<juce::String, juce::String>>& items, unsigned current) {
    float y = top + (zone::picture_h - items.size() * (list_row_h + list_row_gap)) / 2;
    for (unsigned n = 0; n < items.size(); ++n) {
        ListRow r;
        r.name = items[n].first; r.current = n == current; r.accent = colour_of(s);
        r.tag = r.current ? items[n].second : juce::String();
        p.g.saveState();
        list_row(p, px, y + n * (list_row_h + list_row_gap), pw, r);
        p.g.restoreState();
    }
}

void engines_picture(Paint& p, const Snapshot& s) {
    std::array<EngineKind, size_t(EngineKind::Count)> engines {};
    unsigned count = new_engines(engines);
    if (!count) return;
    unsigned at = s.ui.new_engine % count;
    const auto& info = engine_info(engines[at]);
    juce::String sound = info.sound_count ? name_case(info.sounds[std::min<unsigned>(s.ui.new_sound, info.sound_count - 1)].name) : juce::String();
    std::vector<std::pair<juce::String, juce::String>> items;
    for (unsigned n = 0; n < count; ++n) items.push_back({engine_info(engines[n]).name, sound});
    choice_rows(p, s, items, at);
}

void sources_picture(Paint& p, const Snapshot& s) {
    choice_rows(p, s, {{"Samples on the card", "SAMPLE SELECT"}, {"Record the input", "SAMPLE RECORD"},
                       {"Audio Recall", "LAST 25 S"}, {"Resample", "THEN A PAD"}}, s.ui.new_source % new_sources);
}

void ports_picture(Paint& p, const Snapshot& s) {
    juce::String ch = "CHANNEL " + juce::String(std::clamp<int>(s.ui.new_channel, 1, 16));
    choice_rows(p, s, {{"MIDI Out jack", ch}, {"USB-C", ch}, {"All USB", ch}}, s.ui.new_port % 3);
}

void bars_picture(Paint& p, const Snapshot& s, const StagePage& page, bool shift_row) {
    std::vector<Row> list;
    const auto& ids = shift_row ? page.shift : page.params;
    for (unsigned n = 0; n < 3; ++n) if (ids[n] >= 0) list.push_back(param_row(s, ids[n], n, !shift_row));
    if (list.empty()) return;
    rows(p, {}, list, 96);
    if (shift_row) icon(p, Icon::Shift, px, top + 6, 12, colour::mid, 1.4f); // the rows Shift + K1-K3 turn
}

ui::SoftKeys stage_keys(const Snapshot& s) {
    const auto& t = s.track();
    const auto& g = stage_groups(t.engine);
    auto at = stage_at(s.ui, t);
    std::array<juce::String, 3> labels;
    SoftKeys keys {labels, int(at.group), colour_of(s)};
    for (unsigned n = 0; n < 3; ++n) {
        unsigned sub = n == at.group ? at.sub : 0;
        keys.labels[n] = stage_page_at(t, n, sub).key;
        keys.dots[n] = uint8_t(stage_count(g, n) > 1 ? stage_count(g, n) : 0);
        keys.dot_at[n] = uint8_t(sub);
    }
    return keys;
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
    if (stage_kit_page(page.kind) != PageId::Count) {
        h.left.push_back(kit_pill(s));
    } else if (page.picture == StagePicture::Voice) {
        int hit = int(t.last_note) - 36;
        if (hit >= 0 && hit < int(kit909::Drums)) h.left.push_back(pill("PAD " + juce::String(hit + 1) + dot + juce::String(kit909::drum_name(unsigned(hit))), c));
    } else if (page.kind == StageKind::Arp || page.kind == StageKind::Chord) { // on or off, as the LED on the MPC would say
        bool on = page.kind == StageKind::Arp ? t.arp : t.chord;
        h.left.push_back(pill(on ? Icon::Check : Icon::None, on ? "ON" : "OFF", on ? c : colour::mid));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    auto keys = stage_keys(s);
    if (s.ui.shift && page.kind == StageKind::Trim && t.engine == EngineKind::Sampler) // the old Trim's Shift jobs on B1 and B2
        keys = SoftKeys {{sampler_pad(s).loop_lock ? "LOCK OFF" : "LOOP LOCK", "NORMALIZE", ""}, -1, c};
    frame(p, s, keys, h);
    switch (page.picture) {
    case StagePicture::Preset: preset_picture(p, s); break;
    case StagePicture::Filter: filter_picture(p, s, page); break;
    case StagePicture::Envelope: envelope_picture(p, s, page); break;
    case StagePicture::Lfo: lfo_picture(p, s, page); break;
    case StagePicture::ShiftRows: bars_picture(p, s, page, true); break;
    case StagePicture::Mix: mix_picture(p, s); break;
    case StagePicture::Arp: arp_picture(p, s); break;
    case StagePicture::Chord: chord_picture(p, s); break;
    case StagePicture::Fx: fx_picture(p, s); break;
    case StagePicture::Wave: wave_picture(p, s, page); break;
    case StagePicture::Voice: voice_picture(p, s); break;
    case StagePicture::Levels: levels_picture(p, s, page); break;
    case StagePicture::Route: route_picture(p, s); break;
    case StagePicture::Program: program_picture(p, s); break;
    case StagePicture::Cc: cc_picture(p, s); break;
    case StagePicture::Engines: engines_picture(p, s); break;
    case StagePicture::Sources: sources_picture(p, s); break;
    case StagePicture::Ports: ports_picture(p, s); break;
    case StagePicture::Bars: bars_picture(p, s, page, false); break;
    }
}

void soft(Context& c, int key) {
    const auto& page = stage_page_now(c.ui, c.track());
    if (c.ui.shift && page.kind == StageKind::Trim && c.track().engine == EngineKind::Sampler) {
        if (key == 0) c.toggle_loop_lock(); // Shift + B1, as on the MPC's Trim
        if (key == 1) c.normalize();        // Shift + B2
        return;
    }
    c.stage_tick(UINT32_MAX); // a sound still waiting to load lands first
    stage_press(c.ui, c.track(), unsigned(key));
}

void wheel(Context& c, int delta) {
    const auto& page = stage_page_now(c.ui, c.track());
    auto cards = page_bindings(c.project, c.ui, c.pattern(), false);
    if (page.kind == StageKind::Preset) { // the sounds, one a detent
        if (cards[0].kind == Binding::Kind::StageSound) c.preset_turn(cards[0].value + delta);
        return;
    }
    int n = stage_focus(c.ui, c.track());
    const auto& b = cards[size_t(n)];
    if (!b.bound()) return;
    c.apply(b, b.value + delta);
    auto& pot = c.ui.pots[size_t(n)];
    pot.engaged = false; pot.bound = 0; // the value left the pot: it picks it up again
    c.ui.turning = int8_t(n); c.ui.turned_at = c.now;
}

void push(Context& c) {
    auto& t = c.track();
    const auto& page = stage_page_now(c.ui, t);
    if (!t.used()) { c.stage_new(); return; }
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
    auto cards = page_bindings(c.project, c.ui, c.pattern(), false);
    int from = stage_focus(c.ui, t);
    for (int k = 1; k <= 3; ++k) { // the next card that holds something
        int n = (from + k) % 3;
        if (cards[size_t(n)].bound()) { c.ui.last_pot = int8_t(n); c.ui.last_pot_shift = false; c.ui.turning = int8_t(n); c.ui.turned_at = c.now; return; }
    }
}

// Minus / Plus: a synth's octave; a kit's bank, as on its pages.
void nudge(Context& c, int way) {
    if (c.track().engine == EngineKind::Sampler) sampler_bank(c, way);
    else octave(c, way);
}
} // namespace

// The ten sample pages draw with Stage's own kit picture (pages/sampler_pages.cpp and kin): one look per page,
// whichever way it was reached. `row`: the second row of cards on pages that have one, as two dots on the bottom row.
void kit_page(Paint& p, const Snapshot& s, StageKind kind, int row) {
    Header h = header_of(s);
    h.left.push_back(kit_pill(s));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, sampler_keys(s.ui.page, colour_of(s)), h);
    StagePage page {"", kind, StagePicture::Wave};
    wave_picture(p, s, page);
    if (row >= 0) page_dots(p, zone::width - zone::pad - 64, bottom - 9, 2, row, colour_of(s), true); // beside the length
}

const PageSpec stage_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
