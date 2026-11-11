// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/pages/sampler_common.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
// The MPC's Sample Mode groups (parity phase 10): B1 Trim / Mix / Amp Env, B2 Tune / Play, B3 Filter / Filt Env;
// Melodee's own pages join the group they belong with: Follow and Cloud B2's, Kit filter B3's.
constexpr const PageId* order = sampler_order;
const char* keys[sampler_page_count] {"TRIM", "MIX", "AMP ENV", "TUNE", "PLAY", "FOLLOW", "CLOUD", "FILTER", "FILT ENV", "KIT FILT"};
const char* titles[sampler_page_count] {"Trim", "Mix", "Amp env", "Tune", "Play", "Follow", "Cloud", "Filter", "Filter env", "Kit filter"};
} // namespace

// A group's key: the page in view, else the group's first page; a dot a page under it, as on Stage.
SoftKeys sampler_keys(PageId page, juce::Colour c) {
    unsigned n = sampler_index(page), here = sampler_group(n);
    std::array<juce::String, 3> labels;
    SoftKeys out {labels, int(here), c};
    for (unsigned g = 0; g < sampler_groups; ++g) {
        unsigned first = sampler_group_first(g), size = sampler_group_size(g);
        out.labels[g] = keys[g == here ? n : first];
        out.dots[g] = uint8_t(size);
        out.dot_at[g] = uint8_t(g == here ? n - first : 0);
    }
    return out;
}

void sampler_soft(Context& c, int key) {
    c.open(sampler_next(c.ui.page, unsigned(key)));
    c.ui.sampler_row = 0;
}

// Push: the page map of all ten (as Edit's), the wheel moves, Push jumps; Shift + Push or a B key closes it.
void sampler_push(Context& c) {
    if (!c.ui.map_open) { c.ui.map_open = true; c.ui.map_cursor = uint8_t(sampler_index(c.ui.page)); return; }
    c.ui.map_open = false;
    c.open(order[c.ui.map_cursor % sampler_page_count]);
    c.ui.sampler_row = 0;
}

bool sampler_map_wheel(Context& c, int delta) {
    if (!c.ui.map_open) return false;
    c.ui.map_cursor = uint8_t(std::clamp<int>(c.ui.map_cursor + delta, 0, int(sampler_page_count) - 1));
    return true;
}

void sampler_wheel(Context& c, int delta) { sampler_map_wheel(c, delta); }

// The map over the picture: three columns, one a B key, its pages under it.
void draw_sampler_map(Paint& p, const Snapshot& s) {
    if (!s.ui.map_open || !is_sampler_page(s.ui.page)) return;
    auto c = colour_of(s);
    p.dim_zone(zone::picture_y, zone::picture_h, 0.75f);
    float px = zone::pad, pw = zone::width - 2 * zone::pad, py = zone::picture_y + 2, ph = zone::picture_h - 4;
    p.fill(px, py, pw, ph, colour::surf, 6);
    p.ring(px, py, pw, ph, colour::line, 6);
    float cw = (pw - 16) / 3, rh = 19;
    static const char* names[sampler_groups] {"B1", "B2", "B3"};
    unsigned here = sampler_index(s.ui.page);
    for (unsigned g = 0; g < sampler_groups; ++g) {
        float x = px + 5 + g * (cw + 3);
        p.text(names[g], x + 4, py + 3, cw - 8, 14, caps_text(colour::mid));
        for (unsigned k = 0; k < sampler_group_size(g); ++k) {
            unsigned n = sampler_group_first(g) + k;
            float y = py + 18 + k * (rh + 2);
            bool on = n == s.ui.map_cursor;
            if (on) p.fill(x, y, cw, rh, c, 4);
            else if (n == here) p.ring(x, y, cw, rh, colour::line, 4);
            p.text(juce::String(n + 1), x + 4, y, 15, rh, plain(on ? colour::bg : colour::dim, type::small, Weight::SemiBold));
            p.text(titles[n], x + 20, y, cw - 22, rh, p.fit(titles[n], plain(on ? colour::bg : colour::sec, type::small, Weight::Medium), cw - 22, type::floor));
        }
    }
}

const PadSound& sampler_pad(const Snapshot& s) { return s.track().pads[s.track().last_pad % sampler_pads]; }

juce::String sampler_pad_pill(const Snapshot& s) {
    const auto& t = s.track();
    char label[4]; pad_label(t.last_pad, label);
    const auto& pad = sampler_pad(s);
    return juce::String("PAD ") + juce::String(label)
         + (pad.used() ? juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(s.project.samples[pad.sample % sample_slots].name) : juce::String());
}

float pad_icons(Paint& p, float x, float y, const PadSound& pad, juce::Colour c) {
    p.g.setColour(c);
    if (pad.play == PadPlay::Gate) { // Note On: a note
        p.g.fillEllipse(x + 1, y + 6, 4.4f, 3.4f);
        p.g.fillRect(x + 4.6f, y + 0.5f, 1.1f, 7.2f);
        p.g.fillRect(x + 4.6f, y + 0.5f, 3.2f, 1.3f);
        x += 11;
    }
    if (pad.loop) { // Loop: a ring with an arrowhead
        juce::Path loop;
        loop.addCentredArc(x + 5.5f, y + 5, 4, 3.4f, 0, 0.6f, 5.6f, true);
        p.g.strokePath(loop, juce::PathStrokeType(1.3f));
        juce::Path head;
        head.addTriangle(x + 7.2f, y + 0.8f, x + 9.6f, y + 2.6f, x + 6.8f, y + 3.8f);
        p.g.fillPath(head);
        x += 13;
    }
    if (pad.reverse) { // reverse: an arrow pointing back
        juce::Path arrow;
        arrow.startNewSubPath(x + 10, y + 5); arrow.lineTo(x + 2, y + 5);
        arrow.startNewSubPath(x + 5, y + 2); arrow.lineTo(x + 2, y + 5); arrow.lineTo(x + 5, y + 8);
        p.g.strokePath(arrow, juce::PathStrokeType(1.3f));
        x += 13;
    }
    return x;
}

void sampler_top(Paint& p, const Snapshot& s, const juce::String& pill_text, bool rows) {
    auto c = colour_of(s);
    unsigned n = sampler_index(s.ui.page);
    Header h = header_of(s);
    if (pill_text.isNotEmpty()) h.left.push_back(pill(pill_text, c));
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, sampler_keys(s.ui.page, c), h);
    float title_w = p.text(titles[n], zone::pad, zone::picture_y, 200, 22, plain(colour::text, type::title, Weight::SemiBold));
    if (s.track().engine == EngineKind::Sampler && sampler_pad(s).used())
        pad_icons(p, zone::pad + title_w + 8, zone::picture_y + 6, sampler_pad(s), colour::sec);
    if (rows) {
        float dots = page_dots(p, zone::width - zone::pad, zone::picture_y + 11, 2, s.ui.sampler_row, c, true);
        p.text("ROW " + juce::String(s.ui.sampler_row + 1) + " / 2", zone::pad, zone::picture_y, zone::width - 2 * zone::pad - dots - 6, 22,
               caps_text(colour::mid), juce::Justification::centredRight);
    }
}

void sampler_pills(Paint& p, float y, std::initializer_list<juce::String> texts, juce::Colour first) {
    float x = zone::pad;
    bool lead = true;
    for (const auto& t : texts) {
        auto style = caps_text(lead ? first : colour::mid);
        float w = p.width(t, style) + 8;
        if (x + w > zone::width - zone::pad) report_text_issue({t, style.size, x + w - zone::pad, zone::width - 2.0f * zone::pad}); // the row runs off the screen
        p.fill(x, y, w, 15, colour::surf, 3);
        p.text(t, x, y, w, 15, style, juce::Justification::centred);
        x += w + 4; lead = false;
    }
}

void draw_filter_curve(Paint& p, float x, float y, float w, float h, unsigned shape, unsigned poles, float cutoff, float q,
                       juce::Colour c, juce::Colour marker) {
    juce::Path line, area;
    float cx = x;
    for (int n = 0; n <= 120; ++n) {
        float hz = 20.0f * std::pow(1000.0f, n / 120.0f), r = hz / cutoff;
        float lp = 1 / std::sqrt((1 - r * r) * (1 - r * r) + (r / q) * (r / q));
        float gain = shape == 1 ? lp * r / q : shape == 2 ? lp * r * r : lp;
        if (poles == 4) gain = gain * (shape == 1 ? lp * r : shape == 2 ? lp * r * r : lp) / std::max(1.0f, q * 0.7f);
        float db = 20 * std::log10(std::max(gain, 1e-4f));
        float px = x + w * n / 120.0f, py = y + h - std::clamp((db + 30) / 45, 0.0f, 1.0f) * h;
        if (n == 0) { line.startNewSubPath(px, py); area.startNewSubPath(px, y + h); }
        line.lineTo(px, py); area.lineTo(px, py);
        if (hz <= cutoff) cx = px;
    }
    area.lineTo(x + w, y + h); area.closeSubPath();
    p.g.setColour(c.withAlpha(0.14f)); p.g.fillPath(area);
    p.g.setColour(c); p.g.strokePath(line, juce::PathStrokeType(1.5f));
    p.fill(cx, y, 1, h, marker);
    auto axis = caps_text(colour::dim);
    p.text("20 HZ", x, y + h + 3, 40, 9, axis); p.text("200", x + w / 3 - 10, y + h + 3, 40, 9, axis);
    p.text("2 K", x + 2 * w / 3 - 10, y + h + 3, 40, 9, axis); p.text("20 K", x + w - 40, y + h + 3, 40, 9, axis, juce::Justification::centredRight);
}

void draw_envelope(Paint& p, float x, float y, float w, float h, double length, double a, double d, float sustain, double r, bool fade_end,
                   unsigned track) {
    length = std::max(length, 0.01);
    auto x_of = [&](double t) { return x + float(std::clamp(t / length, 0.0, 1.0)) * w; };
    float top = y + 2, base = y + h - 2;
    auto y_of = [&](float l) { return base - l * (base - top); };
    struct Seg { double t0, t1; float l0, l1; unsigned knob; };
    std::array<Seg, 4> segs;
    if (fade_end) { // held at the top, then faded over the decay before the end
        double from = std::max(a, length - d);
        segs = {Seg {0, a, 0, 1, 0}, Seg {a, from, 1, 1, 3}, Seg {from, length, 1, 0, 1}, Seg {0, 0, 0, 0, 3}};
    } else {
        segs = {Seg {0, a, 0, 1, 0}, Seg {a, a + d, 1, sustain, 1}, Seg {a + d, std::max(a + d, length - r), sustain, sustain, 3},
                Seg {std::max(a + d, length - r), length, sustain, 0, 2}};
    }
    for (const auto& sg : segs) {
        if (sg.t1 <= sg.t0) continue;
        p.g.setColour(sg.knob == 3 ? colour::mid : knob_shade(track, sg.knob));
        p.g.drawLine(x_of(sg.t0), y_of(sg.l0), x_of(sg.t1), y_of(sg.l1), 1.5f);
    }
}

void sampler_rows_wheel(Context& c, int delta) {
    if (sampler_map_wheel(c, delta)) return;
    c.ui.sampler_row = uint8_t(std::clamp(c.ui.sampler_row + delta, 0, 1));
}
void sampler_bank(Context& c, int way) { c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks); }

const PlayHead* pad_head(const Snapshot& s, unsigned pad, const PadSound& sound, uint32_t frames, ui::WaveHead& out) {
    if (!frames) return nullptr;
    for (const auto& head : s.heads) {
        if (!head.shown() || head.pad != int(pad) || head.slot != sound.sample || (head.flags & tap::Cloud)) continue;
        uint32_t wake_from = head.reverse() ? head.to : head.pass && (head.flags & tap::Loop) ? head.loop_at : head.from;
        out.at = float(head.at / frames);
        out.wake_from = float(wake_from) / float(frames);
        out.strength = head.level;
        out.wake = head.wake;
        return &head; // newest first
    }
    return nullptr;
}

} // namespace melodee
