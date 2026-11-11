// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// STAGE (Sample): the selected track's quick pages, as the MPC's Sample Mode.
// B1-B3 each hold a cycle of pages (engine/engine_info.h, stage_groups()):
// the lit one again steps to the next page, another jumps to its first; the
// soft keys name each button's page, with a dot a page under a cycle. A synth
// has SOUND (preset, tone, mix), SHAPE (envelopes, LFO) and PLAY (arp, its
// own, effects); a kit Akai's Trim / Mix / Amp Env, Tune / Play and Filter /
// Filt Env on the last pad hit; a 909 its preset, the voice the last pad hit
// and levels, mix, effects; a MIDI track device and program, CC, arp; an
// empty track what it becomes. The wheel fine-edits the card touched last
// (PRESET: walks the sounds; an empty track: walks its list), Push moves to the next card (PRESET: the Sounds
// list; FX: the next effect; an empty track: makes it), Shift + Push is Back.
// The pads play the track; on a kit the pad played is the pad edited.
#include "melodee/engine/arp.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sampler.h"
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
    auto name_of = [&](unsigned sound) { return sound < info.sound_count ? juce::String(info.sounds[sound].name) : "Own sound " + juce::String(sound - info.sound_count + 1); };
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
    float x0 = px + 2, scale = (pw - 4) / total, y0 = top + 8, y1 = bottom - 24;
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
    line(x3, y_of(level), x4, y1, colour::mid);
    axis(p, bottom - 16, a >= 0 ? "A " + shown_value(t, a) : juce::String(), d >= 0 ? "D " + shown_value(t, d) : juce::String(),
         r >= 0 ? "R " + shown_value(t, r) : juce::String());
}

// LFO: its shape at its depth, the rate under it.
void lfo_picture(Paint& p, const Snapshot& s, const StagePage& page) {
    const auto& t = s.track();
    int shape = t.params[size_t(page.params[1])], amount = t.params[size_t(page.params[2])];
    float mid = top + (zone::picture_h - 20) / 2.0f, amp = (zone::picture_h - 36) / 2.0f * std::max(0.15f, amount / 100.0f);
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
    p.g.strokePath(line, juce::PathStrokeType(2, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    axis(p, bottom - 16, shown_value(t, page.params[1]), shown_value(t, page.params[0]), "DEPTH " + shown_value(t, page.params[2]));
}

void mix_picture(Paint& p, const Snapshot& s) {
    const auto& m = s.track().mix;
    juce::String pan = m.pan == 0 ? juce::String("C") : (m.pan < 0 ? "L" : "R") + juce::String(std::abs(int(m.pan)));
    juce::String bus = juce::String::charToString(juce::juce_wchar('A' + m.bus % bus_count));
    rows(p, "TO MASTER" + dot + "SEND TO BUS " + bus,
         {{"LEVEL", {}, juce::String(m.level), m.level / 127.0f, false, knob_shade(s.ui.track, 0)},
          {"PAN", {}, pan, (m.pan + 50) / 100.0f, true, knob_shade(s.ui.track, 1)},
          {"SEND " + bus, {}, juce::String(m.send) + " %", m.send / 100.0f, false, knob_shade(s.ui.track, 2)}}, 64);
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
    juce::String head = example ? juce::String("HOLD PADS") + dot + "C3 E3 G3 SHOWN" : juce::String("HELD");
    if (!example) for (size_t n = 0; n < held.size() && n < 4; ++n) head += (n ? dot : juce::String(" ")) + juce::String(note_name(held[n]));
    juce::String rate = juce::String(arp_rate_name(t.arp_rate)) + dot + juce::String(arp_mode_name(ArpMode(t.arp_mode))).toUpperCase();
    float rw = p.width(rate, caps_text(colour::sec)) + 2;
    p.text(head, px, top + 2, pw - rw - 8, 14, p.fit(head, caps_text(colour::mid), pw - rw - 8, type::floor));
    p.text(rate, px + pw - rw, top + 2, rw, 14, caps_text(colour::sec), juce::Justification::centredRight);
    std::vector<int> notes;
    for (unsigned o = 0; o < std::max<unsigned>(t.arp_octaves, 1); ++o) for (int k : held) notes.push_back(k + int(o) * 12);
    std::vector<int> order = notes;
    switch (ArpMode(t.arp_mode % unsigned(ArpMode::Count))) {
    case ArpMode::Down: std::reverse(order.begin(), order.end()); break;
    case ArpMode::UpDown: for (int n = int(notes.size()) - 2; n > 0; --n) order.push_back(notes[size_t(n)]); break;
    case ArpMode::Random: for (size_t n = 0; n < order.size(); ++n) std::swap(order[n], order[(n * 7 + 3) % order.size()]); break;
    default: break;
    }
    int lo = notes.front(), hi = std::max(notes.back(), lo + 12);
    float gy = top + 22, gh = zone::picture_h - 26, cw = (pw - 15 * 3) / 16.0f;
    for (unsigned n = 0; n < 16; ++n) {
        float x = px + n * (cw + 3);
        if (n % 4 == 0) p.fill(x, gy, cw, gh, colour::panel, 3);
        int key = order[n % order.size()];
        float y = gy + gh - 10 - (key - lo) / float(hi - lo) * (gh - 14);
        p.fill(x + 1.5f, y, cw - 3, 8, n % 4 == 0 ? c : c.withAlpha(0.7f), 2);
    }
}

// FX: the track's chain, the slot the cards edit lit; its name and where it sits.
void fx_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    auto c = colour_of(s);
    unsigned count = t.fx.count(), slot = s.ui.fx_cursor[UiState::cursor_of(s.ui.track)];
    if (!count) {
        p.text("No effects on this track", px, top + 34, pw, 26, plain(colour::text, type::title, Weight::SemiBold), juce::Justification::centred);
        p.text("SHIFT + KNOB FX ADDS ONE", px, top + 66, pw, 14, caps_text(colour::mid), juce::Justification::centred);
        return;
    }
    unsigned shown = std::min(count + 1, 4u), first = slot >= shown ? slot - shown + 1 : 0;
    float bw = (pw - 3 * 5) / 4;
    for (unsigned n = 0; n < 4; ++n) {
        unsigned k = first + n;
        float x = px + n * (bw + 5), y = top + 8;
        bool here = k == slot && k < count;
        p.fill(x, y, bw, 48, here ? c.withAlpha(0.2f) : colour::surf, 4);
        p.text(juce::String(k + 1), x + 7, y + 4, bw - 14, 14, caps_text(colour::mid));
        juce::String name = k < count ? juce::String(effect_info(EffectKind(t.fx.slots[k].kind)).name) : juce::String("Empty");
        auto style = plain(here ? c : k < count ? colour::text : colour::mid, type::body, Weight::SemiBold);
        p.text(name, x + 7, y + 22, bw - 14, 20, p.fit(name, style, bw - 14, type::floor));
    }
    if (slot >= count) return;
    const auto& fx = t.fx.slots[slot];
    juce::String name = effect_info(EffectKind(fx.kind)).name;
    p.text(name, px, top + 70, pw, 26, p.fit(name, plain(colour::text, type::title, Weight::SemiBold), pw, type::floor));
    juce::String where = "TRACK " + juce::String(s.ui.track + 1) + " CHAIN" + dot + "SLOT " + juce::String(slot + 1) + " OF " + juce::String(count)
                       + (fx.bypass ? dot + "BYPASSED" : juce::String());
    p.text(where, px, top + 100, pw, 14, p.fit(where, caps_text(colour::mid), pw, type::floor));
    p.text("PUSH: NEXT EFFECT", px, top + 118, pw, 14, caps_text(colour::dim));
}

juce::String seconds(uint32_t frames) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.3f S", frames / 44100.0);
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
        juce::String why = !pad.used() ? juce::String("No sample on this pad") : s.sample.error.empty() ? juce::String("Loading") : juce::String(s.sample.error);
        p.text(why, px, wy + 42, pw, 18, p.fit(why, plain(colour::sec, type::body, Weight::Medium), pw, type::floor), juce::Justification::centred);
        return;
    }
    // How it plays, as the MPC marks the pad.
    float cx = px;
    auto chip_at = [&](const juce::String& text) {
        auto style = caps_text(c);
        float w = p.width(text, style) + 8;
        p.fill(cx, top + 1, w, 15, c.withAlpha(0.2f), 3);
        p.text(text, cx, top + 1, w, 15, style, juce::Justification::centred);
        cx += w + 4;
    };
    chip_at(pad.play == PadPlay::Gate ? "NOTE ON" : "ONE SHOT");
    if (pad.loop) chip_at(pad.loop_lock ? "LOOP LOCKED" : "LOOP");
    if (pad.reverse) chip_at("REVERSE");
    if (page.kind == StageKind::Trim && s.ui.trim_zoom) chip_at(juce::String::fromUTF8("\xc3\x97") + juce::String(1 << s.ui.trim_zoom));
    if (page.kind == StageKind::Cloud) chip_at(t.params[sampler::Cloud] ? "CLOUD ON" : "CLOUD OFF");
    if (page.kind == StageKind::Follow) chip_at(t.params[sampler::Follow] ? "FOLLOW ON" : "FOLLOW OFF");
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
    juce::String middle = "KEPT " + seconds(end - start);
    if (page.kind == StageKind::Tune) middle = juce::String(pad.pitch >= 0 ? "+" : "") + juce::String(pad.pitch) + " ST " + juce::String(pad.fine >= 0 ? "+" : "") + juce::String(pad.fine) + " CT";
    if (page.kind == StageKind::Play) middle = pad.group ? "MUTE GROUP " + juce::String(pad.group) : juce::String(pad.mono ? "MONO" : "POLY");
    if (page.kind == StageKind::Filter) middle = pad.filter == PadFilter::Off ? juce::String("FILTER OFF") : juce::String(pad_filter_name(pad.filter)).toUpperCase();
    if (page.kind == StageKind::KitFilter) {
        static const char* types[] {"KIT LOW PASS", "KIT BAND PASS", "KIT HIGH PASS"};
        middle = types[unsigned(t.params[sampler::FilterType]) % 3];
    }
    if (page.kind == StageKind::Follow) {
        static const char* hows[] {"BEATS", "TONES", "TAPE"};
        middle = t.params[sampler::Follow] ? juce::String(hows[unsigned(t.params[sampler::FollowHow]) % 3]) + dot + (t.params[sampler::PitchHow] ? "STRETCH" : "SPEED")
                                           : juce::String("K1 TURNS ON");
    }
    if (page.kind == StageKind::Cloud)
        middle = t.params[sampler::Cloud] ? "AROUND " + juce::String((start + (end - start) * t.params[sampler::Position] / 100.0) / 44100.0, 3) + " S"
                                          : juce::String("SHIFT + K3 ON");
    // While it plays, the middle label in two parts: what is happening in the text colour, the rest in mid.
    auto two_part = [&](const juce::String& lead_text, const juce::String& rest_text) {
        axis(p, bottom - 16, seconds(uint32_t(from * frames)), "", seconds(uint32_t(to * frames)));
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
    axis(p, bottom - 16, seconds(uint32_t(from * frames)), middle, seconds(uint32_t(to * frames)));
}

// VOICE (909): the pads as they sit, the voice's lit, the one hit brightest; the voice and its pads.
void voice_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& g = stage_groups(t.engine);
    auto c = colour_of(s);
    unsigned voice = stage_voice(t);
    int hit = int(t.last_note) - 36;
    float cell = 24, gap = 4, gx = px, gy = top + (zone::picture_h - (4 * cell + 3 * gap)) / 2;
    juce::String pads;
    unsigned listed = 0, total = 0;
    for (unsigned n = 0; n < 16; ++n) if (g.pad_voice[n] == int(voice)) ++total;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned row = 3 - n / 4, column = n % 4;
        bool same = g.pad_voice[n] == int(voice), here = int(n) == hit;
        juce::Colour fill = here ? c : same ? c.withAlpha(0.4f) : g.pad_voice[n] >= 0 ? colour::raised : colour::quiet;
        p.fill(gx + column * (cell + gap), gy + row * (cell + gap), cell, cell, fill, 3);
        if (same) { pads += (listed ? (listed + 1 == total ? juce::String(" and ") : juce::String(", ")) : juce::String()) + juce::String(n + 1); ++listed; }
    }
    float x = gx + 4 * cell + 3 * gap + 16, w = px + pw - x;
    juce::String last = hit >= 0 && hit < int(kit909::Drums) ? "LAST HIT" + dot + "PAD " + juce::String(hit + 1) + dot + juce::String(kit909::drum_name(unsigned(hit))).toUpperCase()
                                                            : juce::String("PLAY A PAD TO PICK ITS VOICE");
    p.text(last, x, top + 30, w, 14, p.fit(last, caps_text(colour::mid), w, type::floor));
    juce::String name = engine_info(t.engine).pages[voice].title;
    p.text(name, x, top + 50, w, 26, p.fit(name, plain(colour::text, type::title, Weight::SemiBold), w, type::floor));
    juce::String which = (total > 1 ? "Pads " : "Pad ") + pads;
    p.text(which, x, top + 80, w, 18, plain(colour::sec, type::body, Weight::Medium));
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
    unsigned outputs = s.ui.midi_outputs, devices = (outputs >> 8) & 255;
    juce::String how;
    switch (midi_track::dest_of(t.params[midi_track::Port])) {
    case MidiDest::UsbC: how = (outputs & 2) ? "To the computer over USB-C" : "USB-C is not set up"; break;
    case MidiDest::AllUsb: how = devices ? "USB-C and " + juce::String(devices) + (devices == 1 ? " device" : " devices") : "USB-C, no device plugged in"; break;
    default: how = "Out of the MIDI Out jack"; break;
    }
    p.text(how, px, y + h + 16, pw, 18, p.fit(how, plain(colour::sec, type::body, Weight::Medium), pw, type::floor));
    p.text("NOTES, PROGRAM, CC AND THE CLOCK", px, y + h + 36, pw, 14, caps_text(colour::mid));
}

// PROGRAM (MIDI): the program sent, big, and its bank.
void program_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    using namespace midi_track;
    int program = t.params[Program], msb = t.params[BankMsb], lsb = t.params[BankLsb];
    p.text("PROGRAM CHANGE", px, top + 16, pw, 14, caps_text(colour::mid), juce::Justification::centred);
    p.text(program ? juce::String(program) : juce::String("Off"), px, top + 36, pw, 48, plain(program ? colour_of(s) : colour::mid, 44, Weight::SemiBold),
           juce::Justification::centred);
    auto bank = [](int v) { return v ? juce::String(v - 1) : juce::String("-"); };
    juce::String sub = !program ? juce::String("Off: the device keeps its own sound")
                     : msb || lsb ? "Bank " + bank(msb) + " / " + bank(lsb) + ", sent when the track loads"
                     : juce::String("Sent when the track loads");
    p.text(sub, px, top + 92, pw, 18, p.fit(sub, plain(colour::sec, type::body, Weight::Medium), pw, type::floor), juce::Justification::centred);
}

// CC (MIDI): the three controllers with their General MIDI names.
void cc_picture(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    using namespace midi_track;
    static const char* ports[] {"EXTERNAL", "USB-C", "ALL USB"};
    std::vector<Row> list;
    for (unsigned n = 0; n < 3; ++n) {
        int number = t.params[size_t(CcNumber1 + int(n))], value = t.params[size_t(Cc1 + int(n))];
        const char* name = cc_name(unsigned(number));
        list.push_back({"CC " + juce::String(number), name ? juce::String(name) : juce::String("Controller"), juce::String(value), value / 127.0f, false,
                        knob_shade(s.ui.track, n)});
    }
    rows(p, "SENT ON CHANNEL " + juce::String(t.params[Channel]) + dot + ports[unsigned(t.params[Port]) % 3], list, 44);
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
    juce::String sound = info.sound_count ? juce::String(info.sounds[std::min<unsigned>(s.ui.new_sound, info.sound_count - 1)].name) : juce::String();
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
    rows(p, shift_row ? juce::String("SHIFT + K1-K3") : juce::String(), list, 96);
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

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    const auto& page = stage_page_now(s.ui, t);
    auto c = colour_of(s);
    Header h = header_of(s);
    if (stage_kit_page(page.kind) != PageId::Count) {
        char label[4];
        pad_label(t.last_pad, label);
        auto name = pad_name(s, t.last_pad % sampler_pads, false);
        h.left.push_back(pill(juce::String(label) + (name.isNotEmpty() ? dot + name.toUpperCase() : dot + "EMPTY"), c));
    } else if (page.picture == StagePicture::Voice) {
        int hit = int(t.last_note) - 36;
        if (hit >= 0 && hit < int(kit909::Drums)) h.left.push_back(pill("PAD " + juce::String(hit + 1) + dot + juce::String(kit909::drum_name(unsigned(hit))).toUpperCase(), c));
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

const PageSpec stage_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
