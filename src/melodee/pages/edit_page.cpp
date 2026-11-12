// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// EDIT (Push on Keys): one engine page at a time, its curve filling the
// picture with every parameter named at the point it moves, in its knob's
// colour while it is on the row the pots hold, dim otherwise (the rest of the
// page's parameters listed small at the top right), so nothing hides behind
// a row count. B1 and B3 name the neighbouring pages and go there, B2 MAP
// (its dots the page in view) opens the page map; the wheel moves between
// rows of three; K1-K3 edit the row in view.
#include "melodee/engine/dsp.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace melodee {
namespace {
using namespace ui;

// The picture's box at device size: the whole picture, a line left under it for the axis.
constexpr float px = zone::pad, py = zone::picture_y + 4, pw = zone::width - 2 * zone::pad, ph = 124;

const EditPage& current_page(const Snapshot& s, unsigned& index) {
    const auto& info = engine_info(s.track().engine);
    index = s.ui.edit_page[s.ui.track] % edit_page_count(info);
    return edit_page_at(info, index);
}

// Which knob shade a parameter wears: its card's when on the row in view.
juce::Colour shade_of(const Snapshot& s, const EditPage& page, int id) {
    const auto& row = page.rows[s.ui.edit_row[s.ui.track] % std::max(page.row_count(), 1u)];
    for (unsigned n = 0; n < 3; ++n) if (row[n] == id) return knob_shade(s.ui.track, n);
    return colour::mid;
}

void axis(Paint& p, const juce::String& left, const juce::String& middle, const juce::String& right) {
    auto style = caps_text(colour::dim);
    float y = py + ph + 2;
    p.text(left, px, y, 100, 9, style);
    p.text(middle, px + pw / 2 - 50, y, 100, 9, style, juce::Justification::centred);
    p.text(right, px + pw - 100, y, 100, 9, style, juce::Justification::centredRight);
}

// A parameter's name where it acts on the picture, in its shade.
void name_at(Paint& p, const Snapshot& s, const EditPage& page, int id, float x, float y, juce::Justification j) {
    if (id < 0) return;
    juce::String label = engine_info(s.track().engine).params[id].label;
    auto style = caps_text(shade_of(s, page, id));
    float w = p.width(label, style) + 1;
    float lx = j == juce::Justification::centredRight ? x - w : j == juce::Justification::centred ? x - w / 2 : x;
    lx = std::clamp(lx, px, px + pw - w);
    p.fill(lx - 2, y, w + 4, 14, colour::bg.withAlpha(0.7f), 2);
    p.text(label, lx, y, w, 14, style);
}

// The page's parameters the picture does not draw: small at the top left, name and value, in their shades.
// Returns how wide the column is (0 when it lists nothing), so a picture can start right of it.
float legend(Paint& p, const Snapshot& s, const EditPage& page, std::initializer_list<int> drawn) {
    const auto& info = engine_info(s.track().engine);
    const auto& v = s.track().params;
    float y = py, widest = 0;
    for (const auto& row : page.rows)
        for (int id : row) {
            if (id < 0 || std::find(drawn.begin(), drawn.end(), id) != drawn.end()) continue;
            auto shown = show(info.params[id], v[size_t(id)]);
            juce::String value = juce::String(shown.value) + (shown.unit.empty() ? "" : " " + juce::String(shown.unit));
            auto vs = plain(shade_of(s, page, id) == colour::mid ? colour::dim : colour::sec, type::small, Weight::SemiBold);
            juce::String label = info.params[id].label;
            auto ls = caps_text(shade_of(s, page, id) == colour::mid ? colour::dim : shade_of(s, page, id));
            float lw = p.width(label, ls) + 1;
            p.fill(px, y, lw + 8 + p.width(value, vs) + 4, 14, colour::bg.withAlpha(0.7f), 2);
            p.text(label, px + 2, y, lw, 14, ls);
            p.text(value, px + 2 + lw + 6, y, p.width(value, vs) + 1, 14, vs);
            widest = std::max(widest, lw + 8 + p.width(value, vs) + 4);
            y += 14;
        }
    return widest;
}

juce::String hertz(float hz) {
    if (hz >= 1000) return juce::String(hz / 1000, hz >= 10000 ? 0 : 1) + " kHz";
    return juce::String(int(hz + 0.5f)) + " Hz";
}

void filter_picture(Paint& p, const Snapshot& s, const EditPage& page) {
    using namespace prophet;
    const auto& v = s.track().params;
    auto c = colour_of(s);
    float cutoff = 20.0f * std::exp2(v[Cutoff] / 100.0f * 9.97f), k = 3.95f * v[Resonance] / 100.0f;
    auto response = [&](float hz, float fc) {
        std::complex<float> sn(0, hz / fc), one(1, 0);
        auto stage = one / (one + sn);
        auto h = stage * stage * stage * stage;
        return std::abs(h / (one + k * h));
    };
    auto x_of = [&](float hz) { return px + pw * std::log2(hz / 20.0f) / std::log2(1000.0f); };
    auto y_of = [&](float gain) {
        float db = 20 * std::log10(std::max(gain, 1e-4f));
        return py + ph - 4 - std::clamp((db + 36) / 54, 0.0f, 1.0f) * (ph - 12);
    };
    juce::Path line, area;
    float peak_x = 0, peak_y = py + ph;
    for (int n = 0; n <= 154; ++n) {
        float hz = 20.0f * std::pow(1000.0f, n / 154.0f);
        float x = x_of(hz), y = y_of(response(hz, cutoff));
        if (n == 0) { line.startNewSubPath(x, y); area.startNewSubPath(x, py + ph); }
        line.lineTo(x, y); area.lineTo(x, y);
        if (y < peak_y) { peak_y = y; peak_x = x; }
    }
    area.lineTo(px + pw, py + ph); area.closeSubPath();
    p.fill(px, py + ph - 1, pw, 1, colour::line);
    p.g.setColour(c.withAlpha(0.14f)); p.g.fillPath(area);
    // Where the envelope takes the cutoff, dotted in ENV AMT's shade.
    float opened = cutoff * std::exp2(v[EnvAmount] / 100.0f * 7.0f);
    if (v[EnvAmount] > 0) {
        juce::Path env;
        for (int n = 0; n <= 60; ++n) {
            float hz = 20.0f * std::pow(1000.0f, n / 60.0f);
            float x = x_of(hz), y = y_of(response(hz, std::min(opened, 20000.0f)));
            if (n == 0) env.startNewSubPath(x, y); else env.lineTo(x, y);
        }
        juce::Path dotted;
        const float dashes[] {1, 3};
        juce::PathStrokeType(1).createDashedStroke(dotted, env, dashes, 2);
        p.g.setColour(shade_of(s, page, EnvAmount)); p.g.fillPath(dotted);
    }
    p.g.setColour(c); p.g.strokePath(line, juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    float cx = x_of(std::clamp(cutoff, 20.0f, 20000.0f));
    juce::Path mark, dashed;
    mark.startNewSubPath(cx, py + 8); mark.lineTo(cx, py + ph);
    const float dash[] {1.5f, 3};
    juce::PathStrokeType(1).createDashedStroke(dashed, mark, dash, 2);
    p.g.setColour(shade_of(s, page, Cutoff)); p.g.fillPath(dashed);
    if (v[Resonance] > 5) p.circle(peak_x, peak_y, 3.5f, shade_of(s, page, Resonance));
    name_at(p, s, page, Cutoff, cx - 4, py + ph - 18, juce::Justification::centredRight);
    name_at(p, s, page, Resonance, peak_x, std::max(py, peak_y - 18), juce::Justification::centred);
    if (v[EnvAmount] > 0) name_at(p, s, page, EnvAmount, std::min(x_of(std::min(opened, 20000.0f)), px + pw - 30), py + 20, juce::Justification::centred);
    legend(p, s, page, {Cutoff, Resonance, EnvAmount});
    axis(p, "20 Hz", hertz(cutoff), "20 kHz");
}

void envelope_picture(Paint& p, const Snapshot& s, const EditPage& page) {
    // The row ids of an envelope page: attack, decay, sustain on row 1, release first on row 2.
    int a = page.rows[0][0], d = page.rows[0][1], su = page.rows[0][2], r = page.rows[1][0];
    const auto& v = s.track().params;
    float ta = time_seconds(v[size_t(a)]), td = time_seconds(v[size_t(d)]), tr = time_seconds(v[size_t(r)]);
    float level = v[size_t(su)] / 100.0f, hold = 0.25f;
    // Times on a square-root scale so short and long stages both show.
    float wa = std::sqrt(ta), wd = std::sqrt(td), wr = std::sqrt(tr), total = wa + wd + wr + hold;
    float left = legend(p, s, page, {a, d, su, r}); // the page's other values in a column, the envelope right of it
    float start = px + 2 + (left > 0 ? left + 8 : 0);
    float scale = (px + pw - 2 - start) / total, top = py + 6, bottom = py + ph - 1;
    auto y_of = [&](float l) { return bottom - l * (bottom - top); };
    float x0 = start, x1 = x0 + wa * scale, x2 = x1 + wd * scale, x3 = x2 + hold * scale, x4 = x3 + wr * scale;
    juce::Path area;
    area.startNewSubPath(x0, bottom); area.lineTo(x1, y_of(1)); area.lineTo(x2, y_of(level));
    area.lineTo(x3, y_of(level)); area.lineTo(x4, bottom); area.closeSubPath();
    p.fill(px, bottom, pw, 1, colour::line);
    p.g.setColour(colour_of(s).withAlpha(0.14f)); p.g.fillPath(area);
    auto segment = [&](float ax, float ay, float bx, float by, juce::Colour c) {
        p.g.setColour(c);
        p.g.drawLine(ax, ay, bx, by, 2);
    };
    segment(x0, bottom, x1, y_of(1), shade_of(s, page, a));
    segment(x1, y_of(1), x2, y_of(level), shade_of(s, page, d));
    segment(x2, y_of(level), x3, y_of(level), shade_of(s, page, su));
    segment(x3, y_of(level), x4, bottom, shade_of(s, page, r));
    name_at(p, s, page, a, (x0 + x1) / 2 + 4, y_of(0.55f), juce::Justification::centredLeft);
    name_at(p, s, page, d, (x1 + x2) / 2 + 6, y_of((1 + level) / 2) - 16, juce::Justification::centredLeft);
    name_at(p, s, page, su, (x2 + x3) / 2, y_of(level) - 18, juce::Justification::centred);
    name_at(p, s, page, r, (x3 + x4) / 2 + 4, std::min(y_of(level / 2), bottom - 16), juce::Justification::centredLeft); // above the axis's times
    auto shown = [&](int id) { auto t = show(engine_info(s.track().engine).params[id], v[size_t(id)]); return t.value + " " + t.unit; };
    axis(p, shown(a), shown(d), shown(r));
}

void wave_picture(Paint& p, const Snapshot& s, const EditPage& page, bool b) {
    using namespace prophet;
    const auto& v = s.track().params;
    float saw = v[b ? SawB : SawA], pulse = v[b ? PulseB : PulseA], tri = b ? v[TriB] : 0;
    float width = v[b ? PwB : PwA] / 100.0f;
    float left = legend(p, s, page, {}); // the page's values in a column, the wave right of it, never across them
    float wx = left > 0 ? px + left + 10 : px, ww = px + pw - wx;
    juce::Path line;
    float mid = py + ph / 2, amp = ph / 2 - 8;
    for (int n = 0; n <= 300; ++n) {
        float phase = std::fmod(n / 150.0f, 1.0f);
        float y = saw * (2 * phase - 1) + pulse * (phase < width ? 1 : -1) + tri * (1 - 4 * std::fabs(phase - 0.5f));
        float sum = saw + pulse + tri;
        if (sum > 0) y /= sum;
        float x = wx + ww * n / 300.0f;
        if (n == 0) line.startNewSubPath(x, mid - y * amp); else line.lineTo(x, mid - y * amp);
    }
    p.fill(wx, mid, ww, 1, colour::line);
    if (saw + pulse + tri == 0) { axis(p, "", "silent", ""); return; }
    p.g.setColour(colour_of(s));
    p.g.strokePath(line, juce::PathStrokeType(2, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    auto f = show(engine_info(s.track().engine).params[b ? FreqB : FreqA], v[b ? FreqB : FreqA]);
    axis(p, "", f.value + " " + f.unit, "");
}

void lfo_picture(Paint& p, const Snapshot& s, const EditPage& page) {
    using namespace prophet;
    const auto& v = s.track().params;
    float left = legend(p, s, page, {}); // the page's values in a column, the wave right of it
    float wx = left > 0 ? px + left + 10 : px, ww = px + pw - wx;
    juce::Path line;
    float mid = py + ph / 2, amp = ph / 2 - 8;
    for (int n = 0; n <= 300; ++n) {
        float phase = std::fmod(n / 100.0f, 1.0f), y;
        switch (v[LfoShape]) {
        case 1: y = 2 * phase - 1; break;
        case 2: y = phase < 0.5f ? 1 : -1; break;
        case 3: y = std::sin(float(int(n / 25) * 12.9898f)) ; break;
        default: y = 1 - 4 * std::fabs(phase - 0.5f);
        }
        float x = wx + ww * n / 300.0f;
        if (n == 0) line.startNewSubPath(x, mid - y * amp); else line.lineTo(x, mid - y * amp);
    }
    p.fill(wx, mid, ww, 1, colour::line);
    p.g.setColour(colour_of(s));
    p.g.strokePath(line, juce::PathStrokeType(2, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    auto r = show(engine_info(s.track().engine).params[LfoRate], v[LfoRate]);
    axis(p, "", r.value + " " + r.unit, "");
}

// Every parameter of the page as a labelled gauge: the default picture.
void bars_picture(Paint& p, const Snapshot& s, const EditPage& page) {
    const auto& info = engine_info(s.track().engine);
    const auto& v = s.track().params;
    unsigned rows = std::max(page.row_count(), 1u);
    float row_h = std::min(34.0f, (ph + 10) / rows), cw = (pw - 16) / 3;
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned n = 0; n < 3; ++n) {
            int id = page.rows[r][n];
            if (id < 0) continue;
            const auto& param = info.params[id];
            float x = px + n * (cw + 8), y = py + r * row_h;
            auto shown = show(param, v[size_t(id)]);
            p.text(param.label, x, y, cw, 10, caps_text(shade_of(s, page, id)));
            p.text(shown.value + (shown.unit.empty() ? "" : " " + shown.unit), x, y + 10, cw, 13, plain(colour::sec, type::small, Weight::Medium));
            gauge(p, x, y + 25, cw, gauge_of(param, v[size_t(id)]), shade_of(s, page, id));
        }
}

void mixer_picture(Paint& p, const Snapshot& s, const EditPage& page) { bars_picture(p, s, page); }

// A MIDI track's DEVICE page (docs/MIDI_TRACKS.md; round 2, midi-track A): the route as Stage draws it (common.cpp).
void route_picture(Paint& p, const Snapshot& s) { midi_route(p, s, px, py, pw, ph); }

// MOD MATRIX: eight routes, all on one screen; the wheel picks the route.
void mod_picture(Paint& p, const Snapshot& s) {
    const auto& mods = s.track().mods;
    auto c = colour_of(s);
    unsigned current = s.ui.edit_row[s.ui.track] % mod_routes;
    float y = zone::picture_y + 1, rh = 17;
    for (unsigned r = 0; r < mod_routes; ++r, y += rh + 1) {
        const auto& route = mods[r];
        bool on = r == current;
        float x = zone::pad;
        if (on && route.used()) p.fill(x, y, zone::width - 2 * zone::pad, rh, colour::raised, 4);
        if (on) {
            p.g.saveState();
            p.g.reduceClipRegion(juce::Rectangle<int>(int(x), int(y), 3, int(rh)));
            p.fill(x, y, 8, rh, c, 4);
            p.g.restoreState();
        }
        p.text(juce::String(r + 1), x + 9, y, 10, rh, plain(route.used() ? c : colour::dim, type::small, Weight::Bold));
        if (!route.used()) { p.text("Empty", x + 24, y, 100, rh, plain(colour::dim, type::small, Weight::Medium)); continue; }
        p.text(mod_source_name(ModSource(route.source)), x + 24, y, 75, rh,
               plain(on ? knob_shade(s.ui.track, 0) : colour::sec, type::small, Weight::Medium));
        juce::Path arrow;
        float ax = x + 107, ay = y + rh / 2;
        arrow.startNewSubPath(ax, ay); arrow.lineTo(ax + 10, ay);
        arrow.startNewSubPath(ax + 7, ay - 3); arrow.lineTo(ax + 10, ay); arrow.lineTo(ax + 7, ay + 3);
        p.g.setColour(colour::mid);
        p.g.strokePath(arrow, juce::PathStrokeType(1, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // A destination this engine has none of (a route kept from another engine) is drawn dim; it does nothing.
        const char* dest = mod_dest_label(engine_info(s.track().engine), ModDest(route.destination));
        p.text(dest ? dest : mod_dest_name(ModDest(route.destination)), x + 123, y, 90, rh,
               plain(!dest ? colour::dim : on ? knob_shade(s.ui.track, 1) : colour::sec, type::small, Weight::Medium));
        float bx = x + 214, bw = 55, amount = route.amount / float(mod_amount_max);
        p.fill(bx, ay - 1, bw, 2, colour::line, 1);
        float from = std::min(0.0f, amount), to = std::max(0.0f, amount);
        p.fill(bx + bw / 2 + from * bw / 2, ay - 1, (to - from) * bw / 2, 2, knob_shade(s.ui.track, 2), 1);
        p.fill(bx + bw / 2 - 0.5f, ay - 2.5f, 1, 5, colour::mid);
        juce::String value = (route.amount > 0 ? "+" : route.amount < 0 ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String())
            + juce::String(std::abs(int(route.amount)));
        p.text(value, x + 270, y, zone::width - 2 * zone::pad - 277, rh, plain(on ? colour::text : colour::sec, type::small, Weight::Medium),
               juce::Justification::centredRight);
    }
}

// PAGE MAP (B2): every page of the engine, two columns; the wheel moves, Push jumps.
void page_map(Paint& p, const Snapshot& s, const EngineInfo& info) {
    auto c = colour_of(s);
    unsigned count = edit_page_count(info), per_column = (count + 1) / 2;
    float cw = (zone::width - 2 * zone::pad - 4) / 2, rh = 19;
    for (unsigned n = 0; n < count; ++n) {
        float x = zone::pad + (n / per_column) * (cw + 4), y = zone::picture_y + 1 + (n % per_column) * (rh + 2);
        bool on = n == s.ui.map_cursor;
        if (on) p.fill(x, y, cw, rh, c, 4);
        p.text(juce::String(n + 1), x + 4, y, 14, rh, plain(on ? colour::bg : colour::dim, type::small, Weight::SemiBold));
        p.text(edit_page_at(info, n).title, x + 23, y, cw - 26, rh, plain(on ? colour::bg : colour::sec, type::small, Weight::Medium));
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& info = engine_info(s.track().engine);
    auto c = colour_of(s);
    Header h = header_of(s);
    if (s.track().engine == EngineKind::Plugin) { // the plug-in's own parameters, three at a time; B1 another from the card, B2 its presets
        const auto& panel = s.ui.plugin_panel;
        bool ours = panel.entry == s.track().plugin;
        if (ours && panel.status == PluginStatus::Ready) // the share of each audio block it takes
            h.left.push_back(pill(Icon::Chip, juce::String(panel.load / 10) + " %", panel.load > 700 ? colour::amber : colour::mid));
        h.right.push_back(tag_of(s, s.ui.track));
        bool presets = ours && panel.programs > 0;
        frame(p, s, {{"PLUG-INS", presets ? "PRESETS" : "", ""}, -1, c}, h);
        if (s.track().plugin >= max_plugins) { // none yet: B1 picks one from the card
            icon(p, Icon::Plug, zone::width / 2.0f - 14, zone::picture_y + 40, 28, colour::dim, 1.4f);
            p.text("NONE", 0, zone::picture_y + 76, zone::width, 14, caps_text(colour::dim), juce::Justification::centred);
            return;
        }
        draw_plugin_view(p, s, s.track().plugin, zone::picture_y);
        return;
    }
    if (!info.page_count) {
        h.right.push_back(tag_of(s, s.ui.track));
        frame(p, s, {{"", "", ""}, -1, c}, h);
        p.text("Nothing to edit yet", zone::pad, zone::picture_y, zone::width - 12, 40, plain(colour::dim, type::small, Weight::Medium),
               juce::Justification::centred);
        return;
    }
    unsigned index = 0;
    const auto& page = current_page(s, index);
    unsigned count = edit_page_count(info);
    const auto& before = edit_page_at(info, index + count - 1);
    const auto& after = edit_page_at(info, index + 1);
    h.left.push_back(pill(juce::String(page.title).toUpperCase(), c)); // the page in view: no title row under it
    if (page.picture == Picture::Mod && !s.ui.map_open) {
        unsigned used = 0;
        for (const auto& r : s.track().mods) used += r.used();
        h.left.push_back(pill(Icon::Modulation, juce::String(used) + " / " + juce::String(mod_routes)));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    // Either side, the pages before and after; with two pages (a MIDI track's) one key, B3, goes to the other.
    SoftKeys keys {{count > 2 ? before.key : "", "MAP", count > 1 ? after.key : ""}, s.ui.map_open ? 1 : -1, c};
    if (count > 2) keys.glyphs[0] = Icon::ChevronLeft;
    if (count > 1) keys.after[2] = Icon::ChevronRight;
    keys.dots[1] = uint8_t(std::min(count, 16u)); keys.dot_at[1] = uint8_t(index);
    frame(p, s, keys, h);
    if (s.ui.map_open) { page_map(p, s, info); return; }
    if (page.picture == Picture::Mod) { mod_picture(p, s); return; }
    switch (page.picture) {
    case Picture::Filter: filter_picture(p, s, page); break;
    case Picture::Envelope: envelope_picture(p, s, page); break;
    case Picture::Wave: wave_picture(p, s, page, false); break;
    case Picture::WaveB: wave_picture(p, s, page, true); break;
    case Picture::Lfo: lfo_picture(p, s, page); break;
    case Picture::Mixer: mixer_picture(p, s, page); break;
    case Picture::Bars: bars_picture(p, s, page); break;
    case Picture::Route: route_picture(p, s); break;
    case Picture::Mod: break;
    }
}

unsigned page_count(Context& c) { return std::max(edit_page_count(engine_info(c.track().engine)), 1u); }

void soft(Context& c, int key) {
    if (c.track().engine == EngineKind::Plugin) {
        if (key == 0) c.open_plugin_picker(UiState::PluginTarget::Instrument);
        if (key == 1 && c.ui.plugin_panel.entry == c.track().plugin && c.ui.plugin_panel.programs > 0) c.open_plugin_presets(c.track().plugin);
        return;
    }
    unsigned count = page_count(c), t = c.ui.track;
    if (key == 1) { // B2: the page map, starting on the page in view
        c.ui.map_open = !c.ui.map_open;
        c.ui.map_cursor = uint8_t(c.ui.edit_page[t] % count);
        return;
    }
    if (key == 0 && count <= 2) return; // two pages: B3 alone goes to the other
    c.ui.map_open = false;
    c.ui.edit_page[t] = uint8_t((c.ui.edit_page[t] + (key == 0 ? count - 1 : 1)) % count);
    c.ui.edit_row[t] = 0;
}

// A PLUGIN track's rows of parameters (the panel knows how many).
void plugin_rows(Context& c, int delta) {
    unsigned entry = c.track().plugin;
    if (entry >= max_plugins || c.ui.plugin_panel.entry != entry || !c.ui.plugin_panel.rows) return;
    auto& row = c.ui.plugin_row[entry];
    row = uint16_t(std::clamp<int>(row + delta, 0, c.ui.plugin_panel.rows - 1));
    c.ui.plugin_panel.row = row;
    c.plugin_panel_at = 0; // its cards again at once
}

void wheel(Context& c, int delta) {
    if (c.track().engine == EngineKind::Plugin) { plugin_rows(c, delta); return; }
    const auto& info = engine_info(c.track().engine);
    if (!info.page_count) return;
    unsigned t = c.ui.track, count = edit_page_count(info);
    if (c.ui.map_open) {
        c.ui.map_cursor = uint8_t(std::clamp<int>(c.ui.map_cursor + delta, 0, int(count) - 1));
        return;
    }
    unsigned rows = is_mod_page(info, c.ui.edit_page[t]) ? mod_routes : std::max(edit_page_at(info, c.ui.edit_page[t]).row_count(), 1u);
    c.ui.edit_row[t] = uint8_t(std::clamp<int>(c.ui.edit_row[t] + delta, 0, int(rows) - 1));
}

void push(Context& c) {
    const auto& info = engine_info(c.track().engine);
    if (!info.page_count) return;
    unsigned t = c.ui.track;
    if (c.ui.map_open) { // jump there
        c.ui.edit_page[t] = c.ui.map_cursor;
        c.ui.edit_row[t] = 0;
        c.ui.map_open = false;
        return;
    }
    if (is_mod_page(info, c.ui.edit_page[t])) {
        // Push on an empty row starts a route: the LFO into the cutoff, nothing yet.
        unsigned r = c.ui.edit_row[t] % mod_routes;
        if (!c.track().mods[r].used()) c.set_route(r, {uint8_t(ModSource::Lfo), uint8_t(ModDest::Cutoff), 0});
    }
}

// Minus / Plus: the octave, or on a PLUGIN track its rows as the wheel.
void nudge(Context& c, int way) {
    if (c.track().engine == EngineKind::Plugin) plugin_rows(c, way);
    else octave(c, way);
}
} // namespace

const PageSpec edit_page {draw, play_pad, wheel, push, soft, nudge};

} // namespace melodee
