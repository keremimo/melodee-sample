// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A plug-in over the cards (docs/MELODEE.md, Plug-ins), for Stage's EDIT on a
// PLUGIN track: its name, then while it opens the plug glyph, if it could not the
// broken plug and why in two words, if its 32-bit bridge died STOPPED; once
// open, its parameters as a list, each name on one full line with its
// number: the three on K1-K3 raised and ticked in their colours (their values
// on the cards), the row before (or after) with its values; the wheel or
// Minus / Plus move the rows. The share of each block it takes is the
// header's (pages/stage_page.cpp).
#include "melodee/pages/common.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/plugin/plugin_pages.h"
#include "melodee/plugin/plugin_skin.h"
#include "melodee/shell/screen_view.h"
#include "melodee/shell/stage_map.h"
#include "melodee/ui/knobs.h"
#include "melodee/ui/widgets.h"
#include <cmath>
#include <filesystem>

namespace melodee {
using namespace ui;

juce::String plugin_name(const Snapshot& s, unsigned entry) {
    if (entry >= max_plugins) return "Plug-in";
    const auto& panel = s.ui.plugin_panel;
    if (panel.entry == entry && panel.name[0]) return juce::String::fromUTF8(panel.name);
    const auto& ref = s.project.plugins[entry];
    if (!ref.used()) return "Plug-in";
    if (ref.name[0]) return juce::String::fromUTF8(ref.name); // as the picker named it
    return juce::String(std::filesystem::path(ref.path).stem().string());
}

void draw_plugin_view(Paint& p, const Snapshot& s, unsigned entry, float top) {
    const auto& panel = s.ui.plugin_panel;
    const float x = zone::pad, w = zone::width - 2 * zone::pad;
    bool ours = panel.entry == entry;
    if (!ours || panel.status == PluginStatus::None || panel.status == PluginStatus::Loading) { // the plug, waiting
        p.text(plugin_name(s, entry), x, top + 4, w, 22, p.fit(plugin_name(s, entry), plain(colour::text, type::title, Weight::SemiBold), w, type::floor));
        icon(p, Icon::Plug, x + w / 2 - 14, top + 40, 28, colour::dim, 1.4f);
        p.text("OPENING", x, top + 74, w, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    if (panel.status == PluginStatus::Failed || panel.status == PluginStatus::Crashed) {
        // The broken plug, the cause in two words, and (dim, for whoever fixes it) what the loader named.
        p.text(plugin_name(s, entry), x, top + 4, w, 22, p.fit(plugin_name(s, entry), plain(colour::text, type::title, Weight::SemiBold), w, type::floor));
        bool crashed = panel.status == PluginStatus::Crashed;
        auto error = juce::String::fromUTF8(panel.error);
        bool library = error.contains("shared object") || error.contains(".so");
        juce::String cause = crashed ? "STOPPED" : library ? "MISSING LIBRARY" : "WON'T OPEN";
        icon(p, Icon::PlugBroken, x + 2, top + 40, 40, colour::rec, 1.3f);
        icon(p, Icon::Warn, x + 56, top + 44, 13, colour::rec, 1.6f);
        p.text(cause, x + 74, top + 42, w - 74, 16, caps_text(colour::rec, type::body));
        juce::String code = crashed ? juce::String() : library ? error.upToFirstOccurrenceOf(":", false, false).trim() : error;
        if (code.isNotEmpty()) p.text(code, x + 56, top + 62, w - 56, 14, p.fit(code, plain(colour::dim, type::small), w - 56, type::floor));
        return;
    }
    if (!panel.rows) {
        icon(p, Icon::Sliders, x + w / 2 - 14, top + 40, 28, colour::dim, 1.4f);
        p.text("NONE", x, top + 74, w, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    // The window's rows (two of the page in hand): the three in hand raised on one panel, ticked in their knobs' colours.
    constexpr float row_h = 23;
    unsigned hand = panel.row * 3u;
    int first_hand = -1;
    for (unsigned k = 0; k < panel.window_rows * 3u; ++k) if (panel.window_first + k == hand) { first_hand = int(k); break; }
    if (first_hand >= 0) p.fill(x, top + 2 + first_hand * row_h, w, 3 * row_h, colour::panel, 3);
    for (unsigned k = 0; k < panel.window_rows * 3u; ++k) {
        const auto& param = panel.window[k];
        if (!param.used) continue;
        unsigned at = panel.window_first + k;
        bool held = at >= hand && at < hand + 3;
        float y = top + 2 + k * row_h;
        if (held) p.fill(x + 4, y + 5, 2.5f, row_h - 10, knob_shade(s.ui.track, at - hand), 1);
        p.text(juce::String(at + 1), x + 6, y, 22, row_h, plain(colour::dim, type::small, Weight::SemiBold), juce::Justification::centredRight);
        juce::String name = juce::String::fromUTF8(param.name), value = juce::String::fromUTF8(param.text);
        auto value_style = plain(colour::mid, type::body, Weight::SemiBold);
        float vw = held ? 0 : p.width(value, value_style) + 10;
        auto name_style = plain(held ? colour::text : colour::mid, type::body, held ? Weight::Medium : Weight::Regular);
        p.text(name, x + 34, y, w - 42 - vw, row_h, p.fit(name, name_style, w - 42 - vw, type::floor));
        if (!held) p.text(value, x + w - 8 - vw + 10, y, vw - 10, row_h, value_style, juce::Justification::centredRight);
    }
}

// --- The page under B1-B3 (docs/PLUGIN_PAGES_PLAN.md, picks 3 c and 4 a) ---

namespace {
constexpr float cols[3] {58, 160, 262}; // each column over its pot
constexpr float arc_r = 20, sweep = 0.75f * juce::MathConstants<float>::pi; // 270 degrees, the gap at the bottom

// The page line: the page's name (the plug-in's where its page has none), and where it is among the pages.
void page_line(Paint& p, const Snapshot& s, unsigned entry, float y) {
    const auto& panel = s.ui.plugin_panel;
    auto c = colour_of(s);
    juce::String name = panel.page_name[0] ? juce::String::fromUTF8(panel.page_name) : plugin_name(s, entry);
    p.text(name.toUpperCase(), zone::pad, y, 220, 14, p.fit(name.toUpperCase(), caps_text(c), 220, type::floor));
    if (panel.pages > 24) // too many for dots: the count
        p.text(juce::String(panel.page + 1) + " / " + juce::String(panel.pages), zone::width - zone::pad - 80, y, 80, 14,
               plain(colour::mid, type::small, Weight::SemiBold), juce::Justification::centredRight);
    else if (panel.pages > 1)
        page_dots(p, zone::width - zone::pad, y + 7, int(panel.pages), int(panel.page), c, true);
}

juce::Path arc_between(float cx, float cy, float from, float to) {
    juce::Path path;
    path.addCentredArc(cx, cy, arc_r, arc_r, 0, from, to, true);
    return path;
}

// One parameter over its pot: the arc (a choice of a few values: its values as dots), its value, its name. In hand,
// the knob's colour and what its card showed: faint with a white dot where the pot sits until the pot meets the value
// (takeover), the fader glyph, the Shift mark, the snowflake of a frozen track.
void arc_cell(Paint& p, float cx, float top, const PluginPanelParam& param, bool hand, const ui::KnobCard* card, juce::Colour shade) {
    if (!param.used) return;
    const float cy = top + 28;
    const bool pickup = hand && card && card->state == ui::KnobCard::State::Pickup;
    const bool turning = hand && card && card->state == ui::KnobCard::State::Turning;
    juce::Colour c = hand ? (pickup ? shade.withAlpha(0.45f) : shade) : colour::mid;
    auto stroke = [&](const juce::Path& path, juce::Colour colour, float w) {
        p.g.setColour(colour);
        p.g.strokePath(path, juce::PathStrokeType(w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    stroke(arc_between(cx, cy, -sweep, sweep), colour::line, 3);
    float v = std::clamp(param.value / float(plugin_param_max), 0.0f, 1.0f);
    auto point = [&](float at, float r) { float a = -sweep + 2 * sweep * at; return juce::Point<float>(cx + r * std::sin(a), cy - r * std::cos(a)); };
    if (param.steps >= 2 && param.steps <= 12) { // its values as dots, the one it is on in the colour
        unsigned now = unsigned(std::lround(v * float(param.steps - 1)));
        for (unsigned n = 0; n < param.steps; ++n) {
            auto at = point(float(n) / float(param.steps - 1), arc_r);
            p.circle(at.x, at.y, n == now ? 3.2f : 2.0f, n == now ? c : colour::mid);
        }
    } else if (v > 0.002f) {
        stroke(arc_between(cx, cy, -sweep, -sweep + 2 * sweep * v), c, turning ? 4.0f : 3.0f);
    }
    if (pickup && card->pot >= 0) {
        auto at = point(std::clamp(card->pot, 0.0f, 1.0f), arc_r);
        p.circle(at.x, at.y, 3.2f, colour::text);
        p.circle(at.x, at.y, 1.9f, colour::bg);
    }
    if (hand && card) {
        if (card->fader) icon(p, Icon::Fader, cx + 22, top + 2, 10, pickup ? colour::mid : shade, 1.4f);
        if (card->shift) shift_mark(p, cx - 32, top + 4, card->shift == 2 ? shade : colour::mid, card->shift == 2);
        if (card->frozen) icon(p, Icon::Snow, cx + 22, top + 14, 10, colour::ice, 1.4f);
    }
    juce::String value = juce::String::fromUTF8(param.text);
    auto value_style = plain(hand ? (pickup ? colour::dim : colour::text) : colour::mid, type::list, Weight::Medium);
    p.text(value, cx - 50, top + 50, 100, 18, p.fit(value, value_style, 100, type::floor), juce::Justification::centred);
    juce::String name = juce::String::fromUTF8(param.name);
    auto name_style = plain(hand ? c : colour::dim, type::small, Weight::SemiBold);
    if (pickup) name_style = plain(shade, type::small, Weight::SemiBold);
    auto lines = p.wrap(name, name_style, 100, 2);
    if (lines.size() <= 1) p.text(name, cx - 50, top + 69, 100, 13, name_style, juce::Justification::centred);
    else for (int n = 0; n < 2; ++n) p.text(lines[n], cx - 50, top + 64 + 12 * n, 100, 12, name_style, juce::Justification::centred);
}

// A row of three (its slots row * 3 .. + 2) at `top`; the row in hand raised, with its cards' states.
void arc_row(Paint& p, const Snapshot& s, unsigned row, float top, const std::array<ui::KnobCard, 3>& cards) {
    const auto& panel = s.ui.plugin_panel;
    bool hand = row == panel.row;
    if (hand) p.fill(zone::pad, top, zone::width - 2 * zone::pad, 90, colour::panel, 4);
    for (unsigned k = 0; k < 3; ++k) {
        unsigned at = row * 3 + k;
        if (at < panel.window_first || at >= panel.window_first + panel.window_rows * 3u) continue;
        const auto& param = panel.window[at - panel.window_first];
        arc_cell(p, cols[k], top, param, hand, hand ? &cards[k] : nullptr, knob_shade(s.ui.track, k));
    }
}

// The colour a picture's part takes: its knob's while its row is in hand, else grey.
juce::Colour part_colour(const Snapshot& s, int slot) {
    const auto& panel = s.ui.plugin_panel;
    if (slot < 0 || unsigned(slot) / 3 != panel.row) return colour::mid;
    return knob_shade(s.ui.track, unsigned(slot) % 3);
}

// Times from the value's text where it names them ("12 ms"), else from where it sits (0..1, squared, up to 4 s).
double seconds_of(const PluginPanelParam& param) {
    double out = 0;
    if (value_in(param.text, false, out)) return std::clamp(out, 0.0, 30.0);
    double v = param.value / double(plugin_param_max);
    return v * v * 4.0;
}

void envelope_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h, juce::Colour own = {}) {
    const auto& panel = s.ui.plugin_panel;
    double a = seconds_of(panel.picture_params[0]), d = seconds_of(panel.picture_params[1]), r = seconds_of(panel.picture_params[3]);
    float sustain = std::clamp(panel.picture_params[2].value / float(plugin_param_max), 0.0f, 1.0f);
    double hold = std::max(0.2, 0.3 * (a + d + r)), length = std::max(a + d + hold + r, 0.01);
    auto x_of = [&](double t) { return x + float(t / length) * w; };
    float top = y + 2, base = y + h - 2;
    auto y_of = [&](float l) { return base - l * (base - top); };
    struct Part { double t0, t1; float l0, l1; int slot; };
    std::array<Part, 4> parts {Part {0, a, 0, 1, panel.picture_slots[0]}, Part {a, a + d, 1, sustain, panel.picture_slots[1]},
                               Part {a + d, a + d + hold, sustain, sustain, panel.picture_slots[2]},
                               Part {a + d + hold, length, sustain, 0, panel.picture_slots[3]}};
    juce::Path area;
    area.startNewSubPath(x_of(0), base);
    for (const auto& part : parts) area.lineTo(x_of(part.t1), y_of(part.l1));
    area.closeSubPath();
    p.g.setColour((own.isTransparent() ? colour_of(s) : own).withAlpha(0.1f));
    p.g.fillPath(area);
    for (const auto& part : parts) {
        auto c = own.isTransparent() ? part_colour(s, part.slot) : own;
        p.g.setColour(c);
        p.g.drawLine(x_of(part.t0), y_of(part.l0), x_of(part.t1), y_of(part.l1), 2);
        if (c != colour::mid) { p.circle(x_of(part.t1), y_of(part.l1), 3, c); p.circle(x_of(part.t1), y_of(part.l1), 1.6f, colour::bg); }
    }
}

void filter_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h, juce::Colour own = {}) {
    const auto& panel = s.ui.plugin_panel;
    double hz = 0;
    if (!value_in(panel.picture_params[0].text, true, hz))
        hz = 20.0 * std::pow(1000.0, panel.picture_params[0].value / double(plugin_param_max));
    float q = 0.7f + 5.0f * std::clamp(panel.picture_params[1].value / float(plugin_param_max), 0.0f, 1.0f);
    draw_filter_curve(p, x, y, w, h, 0, 2, float(std::clamp(hz, 20.0, 20000.0)), q, own.isTransparent() ? colour_of(s) : own,
                      own.isTransparent() ? part_colour(s, panel.picture_slots[0]) : own, own.isTransparent());
}
// Up to three values large (a page file's "value" picture): each its name in its knob's colour while its row is in
// hand, its value, a gauge.
void value_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    const auto& panel = s.ui.plugin_panel;
    unsigned count = 0;
    while (count < 3 && panel.picture_params[count].used) ++count;
    if (!count) return;
    float cw = w / float(count);
    for (unsigned n = 0; n < count; ++n) {
        const auto& param = panel.picture_params[n];
        auto c = part_colour(s, panel.picture_slots[n]);
        float cx = x + n * cw;
        juce::String name = juce::String::fromUTF8(param.name), value = juce::String::fromUTF8(param.text);
        auto name_style = plain(c == colour::mid ? colour::sec : c, type::small, Weight::SemiBold);
        p.text(name, cx, y + 4, cw, 14, p.fit(name, name_style, cw - 8, type::floor), juce::Justification::centred);
        auto value_style = plain(colour::text, 30, Weight::Medium);
        p.text(value, cx, y + 20, cw, 36, p.fit(value, value_style, cw - 8, type::value), juce::Justification::centred);
        float gw = std::min(cw - 24, 80.0f), gx = cx + (cw - gw) / 2, gy = y + h - 14;
        p.fill(gx, gy, gw, 2, colour::line, 1);
        p.fill(gx, gy, gw * std::clamp(param.value / float(plugin_param_max), 0.0f, 1.0f), 2, c == colour::mid ? colour::sec : c, 1);
    }
}

// An EQ (a page file's "eq" picture): its bands' response summed, each band a dot at its frequency and gain.
// Frequency and gain from the values' text where it names Hz and dB, else from where they sit (20 Hz-20 kHz,
// -18-+18 dB); Q from a plain number, else from where it sits (0.3-10). The scale follows the largest gain.
void eq_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h, juce::Colour own = {}) {
    const auto& panel = s.ui.plugin_panel;
    struct Band { double hz, db, q; int slot; };
    std::vector<Band> bands;
    for (unsigned b = 0; b + 2 < PagePictureSlots::most; b += 3) {
        const auto& f = panel.picture_params[b], &g = panel.picture_params[b + 1], &q = panel.picture_params[b + 2];
        if (!f.used && !g.used) continue;
        Band band {1000, 0, 0.7, panel.picture_slots[b] >= 0 ? panel.picture_slots[b] : panel.picture_slots[b + 1]};
        if (f.used && !value_in(f.text, true, band.hz)) band.hz = 20.0 * std::pow(1000.0, f.value / double(plugin_param_max));
        if (g.used && !decibels_in(g.text, band.db)) band.db = (g.value / double(plugin_param_max) * 2 - 1) * 18;
        if (q.used) {
            double plain_q = std::atof(q.text);
            band.q = plain_q > 0.05 && plain_q < 40 ? plain_q : 0.3 * std::pow(33.0, q.value / double(plugin_param_max));
        }
        band.hz = std::clamp(band.hz, 20.0, 20000.0);
        bands.push_back(band);
    }
    double most = 0; // the scale: the largest band gain, up to the next 6, 12, 18 or 24 dB
    for (const auto& b : bands) most = std::max(most, std::abs(b.db));
    const double range = std::clamp(std::ceil(most / 6.0) * 6.0, 6.0, 24.0);
    auto x_of = [&](double hz) { return x + float(std::log(hz / 20.0) / std::log(1000.0)) * w; };
    auto y_of = [&](double db) { return y + h / 2 - float(std::clamp(db, -range, range) / range) * (h / 2 - 2); };
    p.fill(x, y + h / 2 - 0.5f, w, 1, colour::line);
    juce::Path line, area;
    for (int n = 0; n <= 120; ++n) {
        double hz = 20.0 * std::pow(1000.0, n / 120.0), db = 0;
        for (const auto& b : bands) { // a peaking band's gain at hz (its analogue prototype)
            double r = hz / b.hz, a = std::pow(10.0, b.db / 40.0), one = (1 - r * r) * (1 - r * r);
            double num = one + std::pow(r * a / b.q, 2), den = one + std::pow(r / (a * b.q), 2);
            db += 10 * std::log10(std::max(num / den, 1e-9));
        }
        float px = x_of(hz), py = y_of(db);
        if (n == 0) { line.startNewSubPath(px, py); area.startNewSubPath(px, y + h / 2); }
        line.lineTo(px, py); area.lineTo(px, py);
    }
    area.lineTo(x + w, y + h / 2); area.closeSubPath();
    auto curve = own.isTransparent() ? colour_of(s) : own;
    p.g.setColour(curve.withAlpha(0.14f)); p.g.fillPath(area);
    p.g.setColour(curve); p.g.strokePath(line, juce::PathStrokeType(1.5f));
    for (const auto& b : bands) {
        auto c = part_colour(s, b.slot);
        p.circle(x_of(b.hz), y_of(b.db), 3.5f, c);
        p.circle(x_of(b.hz), y_of(b.db), 1.8f, colour::bg);
    }
    auto axis = plain(colour::dim, type::small, Weight::SemiBold);
    p.text("20 Hz", x, y + h + 3, 44, 12, axis); p.text("200 Hz", x + w / 3 - 18, y + h + 3, 50, 12, axis);
    p.text("2 kHz", x + 2 * w / 3 - 16, y + h + 3, 50, 12, axis); p.text("20 kHz", x + w - 50, y + h + 3, 50, 12, axis, juce::Justification::centredRight);
}

// The plug-in's sound as measured here, left over right, a block a bar, newest on the right.
void scope_lanes(Paint& p, const Snapshot& s, float x, float y, float w, float h, juce::Colour own = {}) {
    const auto& panel = s.ui.plugin_panel;
    auto c = own.isTransparent() ? colour_of(s) : own;
    float lane = (h - 4) / 2, step = w / float(panel.scope.size());
    for (int side = 0; side < 2; ++side) {
        float mid = y + side * (lane + 4) + lane / 2;
        p.fill(x, mid - 0.5f, w, 1, colour::line);
        for (size_t n = 0; n < panel.scope.size(); ++n) {
            uint32_t v = panel.scope[n] >> (side * 16);
            float lo = int8_t(v & 0xff) / 127.0f, hi = int8_t((v >> 8) & 0xff) / 127.0f;
            if (hi - lo < 0.01f) continue;
            p.fill(x + n * step + 0.4f, mid - hi * lane / 2, std::max(1.2f, step - 1.2f), (hi - lo) * lane / 2,
                   c.withAlpha(0.35f + 0.65f * float(n) / float(panel.scope.size())), 0.8f);
        }
    }
}
// --- Skins (docs/PLUGIN_PAGES.md, Skins; plugin/plugin_skin.h) ---

const PluginSkin::Page* skin_page_of(const Snapshot& s) {
    const auto& panel = s.ui.plugin_panel;
    const auto& skin = s.plugin_skin;
    if (!skin) return nullptr;
    if (panel.skin_page == -2) return skin->has_strip ? &skin->strip : nullptr;
    if (panel.skin_page >= 0 && size_t(panel.skin_page) < skin->pages.size()) return &skin->pages[size_t(panel.skin_page)].second;
    return nullptr;
}

// A label's or value's text in the skin's font (Melodee's where it has none), 11 px and up.
void skin_text(Paint& p, const PluginSkin& skin, const juce::String& text, juce::Rectangle<float> box, int size, juce::Colour c, int align) {
    auto just = align < 0 ? juce::Justification::centredLeft : align > 0 ? juce::Justification::centredRight : juce::Justification::centred;
    float height = float(std::max(11, size));
    if (!skin.font) { p.text(text, box.getX(), box.getY(), box.getWidth(), box.getHeight(), plain(c, height, Weight::SemiBold), just); return; }
    p.g.setColour(c);
    p.g.setFont(juce::Font(juce::FontOptions(skin.font).withHeight(height)));
    p.g.drawText(text, box, just, false);
}

// A filmstrip's frame `frame` into `box`.
void skin_frame(Paint& p, const juce::Image& strip, unsigned frames, unsigned frame, juce::Rectangle<float> box) {
    int fh = strip.getHeight() / int(std::max(1u, frames));
    if (fh <= 0) return;
    p.g.drawImage(strip, int(box.getX()), int(box.getY()), int(box.getWidth()), int(box.getHeight()),
                  0, int(std::min(frame, frames - 1)) * fh, strip.getWidth(), fh);
}

// Melodee's marks on a control K1-K3 turn (one a parameter: its knob, switch or slider, else its value). The maker
// draws each control once, so Melodee adds only what the art cannot know: a white dot where the pot sits while it has
// not reached the value (on a knob's sweep, else along the control's bottom), the fader glyph while the fader drives
// it, the Shift mark while Shift reaches its second parameter.
void skin_ring(Paint& p, const PluginSkin::Control& c, juce::Rectangle<float> box, juce::Colour shade, const ui::KnobCard* card) {
    if (!card) return;
    const bool pickup = card->state == ui::KnobCard::State::Pickup;
    if (pickup && card->pot >= 0) {
        float dx = 0, dy = 0;
        if (c.kind == SkinKind::Knob) {
            float r = std::max(box.getWidth(), box.getHeight()) / 2 + 3;
            float a = (-0.75f + 1.5f * std::clamp(card->pot, 0.0f, 1.0f)) * juce::MathConstants<float>::pi;
            dx = box.getCentreX() + r * std::sin(a);
            dy = box.getCentreY() - r * std::cos(a);
        } else {
            auto under = box.expanded(4);
            dx = under.getX() + under.getWidth() * std::clamp(card->pot, 0.0f, 1.0f);
            dy = under.getBottom();
        }
        p.circle(dx, dy, 3.2f, colour::text);
        p.circle(dx, dy, 1.9f, colour::bg);
    }
    if (card->fader) {
        float fx = box.getRight() + 1, fy = box.getY() - 8;
        p.fill(fx - 6, fy - 1, 14, 14, colour::bg, 3);
        icon(p, Icon::Fader, fx - 4, fy + 1, 10, pickup ? colour::mid : shade, 1.4f);
    }
    if (card->shift == 2) shift_mark(p, box.getX() - 9, box.getY() - 6, shade, true);
}

// A skin page (or strip) at `ox`, `oy`: its background, each control, then Melodee's rings on those K1-K3 turn.
void draw_skin_page(Paint& p, const Snapshot& s, const PluginSkin& skin, const PluginSkin::Page& page, float ox, float oy,
                    const std::array<ui::KnobCard, 3>& cards) {
    const auto& panel = s.ui.plugin_panel;
    if (page.background.isValid()) p.g.drawImageAt(page.background, int(ox), int(oy));
    size_t at = 0; // the next control's place in panel.skin_params
    struct Ring { const PluginSkin::Control* control; juce::Rectangle<float> box; unsigned k; };
    std::vector<Ring> rings;
    std::vector<int> handled; // parameters with a knob, switch or slider here: their values get no ring
    for (const auto& c : page.controls)
        if (c.param >= 0 && (c.kind == SkinKind::Knob || c.kind == SkinKind::Switch || c.kind == SkinKind::Slider)) handled.push_back(c.param);
    std::vector<int> ringed;
    for (const auto& c : page.controls) {
        auto box = c.box.toFloat().translated(ox, oy);
        const PluginPanelParam* param = nullptr;
        if (c.param >= 0) {
            if (at >= panel.skin_params.size()) continue;
            int slot = panel.skin_slots[at];
            param = &panel.skin_params[at++];
            bool value_only = c.kind == SkinKind::Value && std::find(handled.begin(), handled.end(), c.param) != handled.end();
            bool once = std::find(ringed.begin(), ringed.end(), c.param) == ringed.end();
            if (slot >= 0 && unsigned(slot) / 3 == panel.row && !value_only && once) {
                rings.push_back({&c, box, unsigned(slot) % 3});
                ringed.push_back(c.param);
            }
        }
        float v = param ? std::clamp(param->value / float(plugin_param_max), 0.0f, 1.0f) : 0.0f;
        juce::Colour ink = c.coloured ? c.colour : colour::text;
        switch (c.kind) {
        case SkinKind::Image: p.g.drawImage(c.image, box); break;
        case SkinKind::Knob: case SkinKind::Switch:
            skin_frame(p, c.image, c.frames, unsigned(std::lround(v * float(c.frames - 1))), box);
            break;
        case SkinKind::Meter: {
            float level = (c.of == "in" ? panel.level_in : panel.level_out) / 1000.0f;
            skin_frame(p, c.image, c.frames, unsigned(std::lround(std::clamp(level, 0.0f, 1.0f) * float(c.frames - 1))), box);
            break;
        }
        case SkinKind::Slider: {
            if (c.image.isValid()) p.g.drawImage(c.image, box);
            if (!c.cap.isValid()) break;
            float cw = float(c.cap.getWidth()), ch = float(c.cap.getHeight());
            float cx = c.horizontal ? box.getX() + v * (box.getWidth() - cw) : box.getCentreX() - cw / 2;
            float cy = c.horizontal ? box.getCentreY() - ch / 2 : box.getY() + (1 - v) * (box.getHeight() - ch);
            p.g.drawImageAt(c.cap, int(std::lround(cx)), int(std::lround(cy)));
            break;
        }
        case SkinKind::Value: if (param) skin_text(p, skin, juce::String::fromUTF8(param->text), box, c.text_size, ink, c.align); break;
        case SkinKind::Label: skin_text(p, skin, c.text, box, c.text_size, ink, c.align); break;
        case SkinKind::Curve: {
            juce::Colour own = c.coloured ? c.colour : colour_of(s);
            bool labels_fit = box.getHeight() > 40;
            float h = labels_fit ? box.getHeight() - 15 : box.getHeight();
            if (c.of == "envelope" && panel.picture == uint8_t(PagePicture::Envelope)) envelope_picture(p, s, box.getX(), box.getY(), box.getWidth(), box.getHeight(), own);
            else if (c.of == "filter" && panel.picture == uint8_t(PagePicture::Filter)) filter_picture(p, s, box.getX(), box.getY(), box.getWidth(), h, own);
            else if (c.of == "eq" && panel.picture == uint8_t(PagePicture::Eq)) eq_picture(p, s, box.getX(), box.getY(), box.getWidth(), h, own);
            break;
        }
        case SkinKind::Scope: if (panel.measured) scope_lanes(p, s, box.getX(), box.getY(), box.getWidth(), box.getHeight(), c.coloured ? c.colour : juce::Colour()); break;
        }
    }
    for (const auto& r : rings) skin_ring(p, *r.control, r.box, knob_shade(s.ui.track, r.k), &cards[r.k]);
}
} // namespace

// A plug-in's own picture (plugin/plugin_screen.h) at x, y. Made into an image again only when a new one came; the
// screen thread draws every page, so the image kept here is that thread's alone.
void draw_screen_picture(Paint& p, const ScreenPicture& picture, float x, float y) {
    static juce::Image image;
    static uint32_t serial = 0;
    static uint8_t entry = no_plugin;
    const int w = picture.width, h = picture.height;
    if (picture.pixels.size() < size_t(w) * size_t(h) || w <= 0 || h <= 0) return;
    if (!image.isValid() || image.getWidth() != w || image.getHeight() != h || serial != picture.serial || entry != picture.entry) {
        if (!image.isValid() || image.getWidth() != w || image.getHeight() != h) image = juce::Image(juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);
        for (int row = 0; row < h; ++row) {
            auto* line = reinterpret_cast<juce::PixelARGB*>(bits.getLinePointer(row));
            const uint32_t* from = picture.pixels.data() + size_t(row) * size_t(w);
            for (int col = 0; col < w; ++col) {
                uint32_t c = from[col];
                line[col].setARGB(0xff, uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c));
            }
        }
        serial = picture.serial;
        entry = picture.entry;
    }
    p.g.drawImageAt(image, int(x), int(y));
}

bool plugin_screen_shown(const Snapshot& s, uint8_t area) {
    const auto at = plugin_screen_place(s.project, s.ui);
    const auto& picture = s.plugin_picture;
    return at.entry >= 0 && at.area == area && s.ui.plugin_panel.screen && picture && picture->entry == unsigned(at.entry)
        && picture->area == area;
}

bool plugin_page_shown(const Snapshot& s, unsigned entry) {
    const auto& panel = s.ui.plugin_panel;
    if (entry >= max_plugins || panel.entry != entry || panel.status != PluginStatus::Ready || (!panel.rows && !panel.screen)) return false;
    auto cards = cards_of(s);
    return std::none_of(cards.begin(), cards.end(), [](const ui::KnobCard& c) { return c.knob_fx; });
}

void draw_plugin_page(Paint& p, const Snapshot& s, unsigned entry, const SoftKeys& keys) {
    frame_keys(p, s, keys);
    const auto& panel = s.ui.plugin_panel;
    auto cards = cards_of(s);
    if (auto* seen = controls_sink()) { // the preview's controls audit: the arcs are this page's pots
        seen->knobs_drawn = true;
        for (unsigned k = 0; k < 3; ++k) seen->knobs[k] = panel.params[k].used ? juce::String::fromUTF8(panel.params[k].name) : juce::String();
    }
    if (plugin_screen_shown(s) && s.plugin_picture->entry == entry) {
        draw_screen_picture(p, *s.plugin_picture, 0, float(zone::header_y)); // its own page under B1-B3, all of it
        return;
    }
    if (s.ui.settings.plugin_skins && panel.skin_page >= 0) // the maker's skin, under B1-B3; Melodee's dots over it
        if (const auto* page = skin_page_of(s)) {
            draw_skin_page(p, s, *s.plugin_skin, *page, 0, float(zone::header_y), cards);
            if (panel.pages > 1 && panel.pages <= 24) page_dots(p, zone::width - zone::pad, 33, int(panel.pages), int(panel.page), colour_of(s), true);
            return;
        }
    page_line(p, s, entry, 26);
    if (panel.picture == uint8_t(PagePicture::Envelope)) {
        envelope_picture(p, s, 14, 46, 292, 80);
        arc_row(p, s, panel.row, 132, cards);
    } else if (panel.picture == uint8_t(PagePicture::Filter)) {
        filter_picture(p, s, 14, 46, 292, 62);
        arc_row(p, s, panel.row, 132, cards);
    } else if (panel.picture == uint8_t(PagePicture::Value)) {
        value_picture(p, s, 14, 46, 292, 80);
        arc_row(p, s, panel.row, 132, cards);
    } else if (panel.picture == uint8_t(PagePicture::Eq)) {
        eq_picture(p, s, 14, 46, 292, 62);
        arc_row(p, s, panel.row, 132, cards);
    } else if (panel.picture == uint8_t(PagePicture::Scope) && panel.measured) {
        scope_lanes(p, s, 14, 52, 292, 70);
        arc_row(p, s, panel.row, 132, cards);
    } else {
        for (unsigned r = 0; r < panel.window_rows; ++r) arc_row(p, s, panel.window_first / 3u + r, 44 + 96 * float(r), cards);
    }
}

bool plugin_strip_shown(const Snapshot& s, unsigned entry) {
    const auto& panel = s.ui.plugin_panel;
    return s.ui.settings.plugin_skins && panel.entry == entry && panel.status == PluginStatus::Ready && panel.skin_page == -2
        && skin_page_of(s) != nullptr;
}

void draw_plugin_strip(Paint& p, const Snapshot& s, float x, float y) {
    if (const auto* strip = skin_page_of(s)) draw_skin_page(p, s, *s.plugin_skin, *strip, x, y, cards_of(s));
}

// An effect in a chain: its page line, then its output as it plays (left over right, a block a bar) and its levels in
// and out.
void draw_plugin_sound(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    const auto& panel = s.ui.plugin_panel;
    auto c = colour_of(s);
    juce::String name = panel.page_name[0] ? juce::String::fromUTF8(panel.page_name).toUpperCase() : juce::String();
    if (name.isNotEmpty()) p.text(name, x, y, w - 90, 14, p.fit(name, caps_text(c), w - 90, type::floor));
    if (panel.pages > 1 && panel.pages <= 24) page_dots(p, x + w, y + 7, int(panel.pages), int(panel.page), c, true);
    float sy = y + 18, sw = w - 96;
    if (h < 30 || !panel.measured) return;
    scope_lanes(p, s, x, sy, sw, std::max(16.0f, h - 18));
    float mx = x + sw + 14, mw = w - sw - 14;
    for (int n = 0; n < 2; ++n) {
        float my = sy + n * 20;
        p.text(n ? "OUT" : "IN", mx, my, 30, 14, caps_text(colour::dim));
        float level = (n ? panel.level_out : panel.level_in) / 1000.0f;
        p.fill(mx + 30, my + 5, mw - 30, 4, colour::line, 2);
        if (level > 0.001f) p.fill(mx + 30, my + 5, (mw - 30) * std::min(1.0f, level), 4, n ? c : colour::mid, 2);
    }
}

} // namespace melodee

