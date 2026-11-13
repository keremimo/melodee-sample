// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// LEVELS (16 Levels): on a kit the last pad hit spreads over all sixteen
// pads, on a synth its last note. The type, Scale first (the most played):
// Melodee's Scale, Tune by semitones, Velocity soft to loud, Filter (kit
// pads) the cutoff. K1 the type. Scale: K2 the root, K3 the scale (the
// project's key), pad 1 the root and each pad the next note of the scale up
// (a synth in the track's octave; a kit's sample counts as a C, moved the
// nearest way to the root). The others: K2 pad 1's end, K3 pad 16's, the pads
// between evenly from one to the other (sixteen levels between 40 and 127, a
// filter sweep over an octave). Shift + K1-K3 the pad's own pitch (a sample
// that is not a C tuned), volume and pan. B1 Velocity, B2 Filter (a kit), B3
// Scale and again Tune (and on an instrument's pad Zones), as Shift + 16
// Levels goes round them. Zones: the keys the instrument's zones sound on,
// lowest on pad 1, sixteen to a page (− / + the pages). The picture:
// Scale a step a pad at its height, the roots in the track's colour; Zones a
// block a pad holding a key; the others the ramp from end to end with a
// handle at each in its knob's colour; a tick a pad, the pad hit marked with
// its note or value. − / + move Scale (and a kit's Tune) an octave. 16 Levels
// again goes back.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/pages/sampler_common.h"
#include <algorithm>
#include <string>

namespace melodee {
namespace {
using namespace ui;

LevelsKind kind_of(const Track& t) { return LevelsKind(t.levels_kind % unsigned(LevelsKind::Count)); }
// The type as the pads play it: Zones only on an instrument's pad.
LevelsKind kind_of(const Snapshot& s) {
    auto kind = kind_of(s.track());
    return kind == LevelsKind::Zones && !levels_instrument(s.project, s.track()) ? LevelsKind::Scale : kind;
}

juce::String value_of(const Snapshot& s, unsigned pad) {
    auto level = levels_of(s.project, s.track(), pad);
    switch (kind_of(s)) {
    case LevelsKind::Zones: return level.none ? juce::String() : juce::String(note_name(level.shown));
    case LevelsKind::Scale: // its note: a synth's by name, a kit's (its sample a C) by letter
        if (level.key >= 0) return juce::String(note_name(level.key));
        return juce::String(note_letter(unsigned((level.semitones % 12 + 12) % 12)));
    case LevelsKind::Filter: if (s.track().engine == EngineKind::Sampler) return juce::String(level.cutoff); [[fallthrough]];
    case LevelsKind::Velocity: return juce::String(level.velocity);
    default: return level.semitones > 0 ? "+" + juce::String(level.semitones) : juce::String(level.semitones);
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    bool kit = t.engine == EngineKind::Sampler;
    auto c = colour_of(s);
    auto kind = kind_of(s);
    if (kind == LevelsKind::Filter && !kit) kind = LevelsKind::Velocity;
    const SampleRef* instrument = kit ? levels_instrument(s.project, t) : nullptr;
    Header h = header_of(s);
    h.left.push_back(chip(Icon::Velocity, "16 LEVELS", c));
    if (kit) { // whose levels: the pad and its sample
        char label[4]; pad_label(t.levels_pad, label);
        const auto& pad = t.pads[t.levels_pad % sampler_pads];
        juce::String name = pad.used() ? name_case(juce::String(s.project.samples[pad.sample % sample_slots].name)) : juce::String("Empty");
        h.left.push_back(pill(juce::String(label) + juce::String::fromUTF8(" \xc2\xb7 ") + name, c));
    } else if (kind != LevelsKind::Scale) h.left.push_back(pill(Icon::Note, juce::String(note_name(t.last_note)), c)); // Scale: the key's notes
    h.right.push_back(tag_of(s, s.ui.track));
    // The same three places on a kit and a synth: B1 Velocity, B2 Filter (kits), B3 Scale and Tune (and an instrument's
    // Zones) as a cycle with its dots.
    bool tune = kind == LevelsKind::Tune, zones = kind == LevelsKind::Zones;
    int active = kind == LevelsKind::Velocity ? 0 : kind == LevelsKind::Filter ? 1 : 2;
    SoftKeys keys {{"VELOCITY", kit ? "FILTER" : "", tune ? "TUNE" : zones ? "ZONES" : "SCALE"}, active, c};
    keys.dots[2] = instrument ? 3 : 2;
    keys.dot_at[2] = zones ? 2 : tune ? 1 : 0;
    frame(p, s, keys, h);

    constexpr float x0 = zone::pad + 14, x1 = zone::width - zone::pad - 14, base = zone::picture_y + 112, top = zone::picture_y + 22;
    auto ends_style = plain(colour::sec, type::small, Weight::SemiBold);
    if (kind == LevelsKind::Zones) { // a block a pad holding a key, the one hit white with its note over it
        auto x_at = [&](unsigned pad) { return x0 + (x1 - x0) * float(pad) / 15; };
        float w = (x1 - x0) / 15 * 0.72f, mid = (base + top) / 2 + 8;
        int last = -1;
        for (unsigned n = 0; n < 16; ++n) {
            auto level = levels_of(s.project, t, n);
            bool hit = int(n) == s.ui.levels_hit;
            if (level.none) p.fill(x_at(n) - w / 2, mid - 1, w, 2, colour::line, 1);
            else { p.fill(x_at(n) - w / 2, mid - 14, w, 28, hit ? colour::text : c, 2); last = int(n); }
            p.fill(x_at(n) - 1, base + 4, 2, n % 4 == 0 ? 8.0f : 5.0f, hit ? colour::text : colour::dim);
        }
        if (s.ui.levels_hit >= 0 && !value_of(s, unsigned(s.ui.levels_hit) % 16).isEmpty()) {
            unsigned n = unsigned(s.ui.levels_hit) % 16;
            p.text(value_of(s, n), x_at(n) - 30, mid - 36, 60, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        }
        p.text(value_of(s, 0), x0 - 8, base + 14, 80, 14, ends_style); // the page's first key and its last
        if (last > 0) p.text(value_of(s, unsigned(last)), x_at(unsigned(last)) - 72, base + 14, 80, 14, ends_style, juce::Justification::centredRight);
        if (unsigned pages = zone_pages(*instrument); pages > 1)
            p.text(juce::String(zone_page(t, pages) + 1) + " / " + juce::String(pages), (x0 + x1) / 2 - 40, base + 14, 80, 14, ends_style,
                   juce::Justification::centred);
        return;
    }
    if (kind == LevelsKind::Scale) { // the key's notes up the pads from its root: a step a pad at its height, the roots lit
        int highest = std::max(1, level_scale_step(s.project, 15));
        auto x_at = [&](unsigned pad) { return x0 + (x1 - x0) * float(pad) / 15; };
        auto y_at = [&](unsigned pad) { return base - float(level_scale_step(s.project, pad)) / float(highest) * (base - top); };
        float w = (x1 - x0) / 15 * 0.72f;
        for (unsigned n = 0; n < 16; ++n) {
            bool hit = int(n) == s.ui.levels_hit, root = level_scale_step(s.project, n) % 12 == 0;
            p.fill(x_at(n) - w / 2, y_at(n) - 1.5f, w, 3, hit ? colour::text : root ? c : colour::sec, 1.5f);
            p.fill(x_at(n) - 1, base + 4, 2, n % 4 == 0 ? 8.0f : 5.0f, hit ? colour::text : colour::dim);
        }
        if (s.ui.levels_hit >= 0) {
            unsigned n = unsigned(s.ui.levels_hit) % 16;
            p.text(value_of(s, n), x_at(n) - 30, std::max(float(zone::picture_y), y_at(n) - 22), 60, 16,
                   plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        }
        p.text(value_of(s, 0), x0 - 8, base + 14, 80, 14, ends_style); // pad 1, the root, and pad 16
        p.text(value_of(s, 15), x1 - 72, base + 14, 80, 14, ends_style, juce::Justification::centredRight);
        return;
    }
    // The ramp from pad 1's end to pad 16's over the type's whole range, a handle at each end.
    auto range = levels_range(kind);
    const auto& ends = t.levels_ends[unsigned(kind) % 4];
    auto y_of = [&](int v) { return base - float(v - range.low) / float(std::max(1, range.high - range.low)) * (base - top); };
    auto x_of = [&](unsigned pad) { return x0 + (x1 - x0) * float(pad) / 15; };
    float ylo = y_of(ends[0]), yhi = y_of(ends[1]);
    juce::Path area;
    area.startNewSubPath(x0, base); area.lineTo(x1, base); area.lineTo(x1, yhi); area.lineTo(x0, ylo); area.closeSubPath();
    p.g.setColour(c.withAlpha(0.22f)); p.g.fillPath(area);
    if (range.low < 0) p.fill(x0, y_of(0) - 0.5f, x1 - x0, 1, colour::line); // 0 on a signed type
    p.g.setColour(c); p.g.drawLine(x0, ylo, x1, yhi, 2);
    for (unsigned n = 0; n < 16; ++n) { // a tick a pad, the one hit white with its value over the ramp
        bool hit = int(n) == s.ui.levels_hit;
        p.fill(x_of(n) - 1, base + 4, 2, n % 4 == 0 ? 8.0f : 5.0f, hit ? colour::text : colour::dim);
    }
    if (s.ui.levels_hit >= 0) {
        unsigned n = unsigned(s.ui.levels_hit) % 16;
        int at = level_between(ends[0], ends[1], n);
        float hx = x_of(n), hy = y_of(at);
        p.fill(hx - 0.75f, hy, 1.5f, base - hy, colour::text);
        p.circle(hx, hy, 3.5f, colour::text);
        p.text(value_of(s, n), hx - 30, std::max(float(zone::picture_y), hy - 22), 60, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
    }
    p.circle(x0, ylo, 5, knob_shade(s.ui.track, 1)); // K2: pad 1's end
    p.circle(x1, yhi, 5, knob_shade(s.ui.track, 2)); // K3: pad 16's
    // Each end as the pad there plays it, under it.
    p.text(value_of(s, 0), x0 - 8, base + 14, 80, 14, ends_style);
    p.text(value_of(s, 15), x1 - 72, base + 14, 80, 14, ends_style, juce::Justification::centredRight);
}

// B1 Velocity; B2 Filter (a kit; nothing on a synth); B3 Scale, and again Tune, on both, and again Zones on an
// instrument's pad.
void soft(Context& c, int key) {
    auto& t = c.track();
    bool kit = t.engine == EngineKind::Sampler;
    auto kind = LevelsKind(t.levels_kind % unsigned(LevelsKind::Count));
    if (key == 1 && !kit) return;
    if (key == 2 && drum_pads(t)) return; // Drums pads: Velocity alone
    const bool instrument = kit && levels_instrument(c.project, t);
    LevelsKind next = key == 0 ? LevelsKind::Velocity
                    : key == 1 ? LevelsKind::Filter
                    : kind == LevelsKind::Scale ? LevelsKind::Tune
                    : kind == LevelsKind::Tune && instrument ? LevelsKind::Zones
                    : LevelsKind::Scale;
    t.levels_kind = uint8_t(next);
    c.ui.levels_hit = -1;
}

// − / +: Scale, and a kit's Tune (an instrument's pad above all), the pads an
// octave down or up, as on the other play pages.
void nudge(Context& c, int way) {
    auto& t = c.track();
    const auto kind = kind_of(t);
    if (kind == LevelsKind::Zones) { // the next sixteen of the instrument's keys, or the ones before
        const auto* instrument = levels_instrument(c.project, t);
        if (!instrument) return;
        const unsigned pages = zone_pages(*instrument);
        const int page = std::clamp(int(zone_page(t, pages)) + way, 0, int(pages) - 1);
        t.octave = int8_t(kit_level_octave + page);
        int last = -1;
        for (unsigned n = 0; n < 16; ++n) if (!levels_of(c.project, t, n).none) last = levels_of(c.project, t, n).shown;
        std::string keys = last >= 0 ? note_name(levels_of(c.project, t, 0).shown) + "\xe2\x80\x93" + note_name(last) : std::string("none");
        c.notice(std::string("Pads ") + keys + (pages > 1 ? " (" + std::to_string(page + 1) + "/" + std::to_string(pages) + ")" : std::string()));
        return;
    }
    if (t.engine != EngineKind::Sampler && kind == LevelsKind::Scale) { // a synth: the track's octave, and the notes the pads now play
        t.octave = int8_t(std::clamp(t.octave + way, 0, 7));
        c.notice(std::string("Pads ") + note_name(levels_of(c.project, t, 0).key) + "\xe2\x80\x93" + note_name(levels_of(c.project, t, 15).key));
        return;
    }
    if (t.engine != EngineKind::Sampler || (kind != LevelsKind::Tune && kind != LevelsKind::Scale)) return;
    t.octave = int8_t(std::clamp(t.octave + way, kit_lowest_octave, kit_highest_octave));
    const int octave = t.octave - kit_level_octave;
    c.notice(octave ? std::string("Octave ") + (octave > 0 ? "+" : "") + std::to_string(octave) : std::string("Octave 0, as it is"));
}
} // namespace

const PageSpec levels_page {draw, play_pad, no_wheel, no_push, soft, nudge};

} // namespace melodee
