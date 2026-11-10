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
const PageId order[sampler_page_count] {PageId::Trim, PageId::PadMix, PageId::EnvelopePage, PageId::PadPage, PageId::PlayPage,
                                        PageId::PadFilterPage, PageId::FilterEnvPage, PageId::FilterPage, PageId::FollowPage, PageId::CloudPage};
const char* keys[sampler_page_count] {"TRIM", "MIX", "AMP ENV", "TUNE", "PLAY", "FILTER", "FILT ENV", "KIT FILTER", "FOLLOW", "CLOUD"};
const char* titles[sampler_page_count] {"Trim", "Mix", "Amp env", "Tune", "Play", "Filter", "Filter env", "Kit filter", "Follow", "Cloud"};
} // namespace

unsigned sampler_index(PageId page) {
    for (unsigned n = 0; n < sampler_page_count; ++n) if (order[n] == page) return n;
    return 0;
}

SoftKeys sampler_keys(PageId page, juce::Colour c) {
    unsigned n = sampler_index(page), count = sampler_page_count;
    return {{juce::String::fromUTF8("\xe2\x80\xb9 ") + keys[(n + count - 1) % count], "PAGE " + juce::String(n + 1) + " / " + juce::String(count),
             juce::String(keys[(n + 1) % count]) + juce::String::fromUTF8(" \xe2\x80\xba")}, 1, c};
}

void sampler_soft(Context& c, int key) {
    unsigned n = sampler_index(c.ui.page), count = sampler_page_count;
    if (key == 0) c.open(order[(n + count - 1) % count]);
    if (key == 2) c.open(order[(n + 1) % count]);
    c.ui.sampler_row = 0;
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
        auto style = caps_text(lead ? first : colour::mid, 7);
        float w = p.width(t, style) + 9;
        p.fill(x, y, w, 12, colour::surf, 3);
        p.text(t, x, y, w, 12, style, juce::Justification::centred);
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
    auto axis = caps_text(colour::dim, 7);
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

void sampler_rows_wheel(Context& c, int delta) { c.ui.sampler_row = uint8_t(std::clamp(c.ui.sampler_row + delta, 0, 1)); }
void sampler_bank(Context& c, int way) { c.track().bank = uint8_t((c.track().bank + sampler_banks + way) % sampler_banks); }

} // namespace melodee
