// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The effects as Stage's FX page and the FX Select pages draw them alike: a
// chain's six slots as a strip, and under it the effect in hand drawn as what
// it does to sound, from its own values (pages/common.h).
#include "melodee/pages/common.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace melodee {
using namespace ui;

namespace {
// A parameter of the slot found by its label: its value, and its place over its range (0..1).
int raw(const EffectSlot& slot, const EffectInfo& info, const char* label, int fallback) {
    for (unsigned n = 0; n < effect_params; ++n)
        if (info.params[n].label && !std::strcmp(info.params[n].label, label)) return slot.params[n];
    return fallback;
}
float norm(const EffectSlot& slot, const EffectInfo& info, const char* label, float fallback) {
    for (unsigned n = 0; n < effect_params; ++n)
        if (info.params[n].label && !std::strcmp(info.params[n].label, label)) {
            const auto& pi = info.params[n];
            return std::clamp(float(slot.params[n] - pi.min) / float(std::max(1, pi.max - pi.min)), 0.0f, 1.0f);
        }
    return fallback;
}

// A curve y = f(u), u 0..1 across x..x+w, as a path.
juce::Path curve(const std::function<float(float)>& f, float x, float w, int steps = 96) {
    juce::Path path;
    for (int i = 0; i <= steps; ++i) {
        float u = float(i) / float(steps);
        if (i == 0) path.startNewSubPath(x, f(u)); else path.lineTo(x + w * u, f(u));
    }
    return path;
}
void stroke(Paint& p, const juce::Path& path, juce::Colour c, float width) {
    p.g.setColour(c);
    p.g.strokePath(path, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
// The area between a curve and a base line, faint.
void under(Paint& p, juce::Path path, float x, float w, float base, juce::Colour c) {
    path.lineTo(x + w, base); path.lineTo(x, base); path.closeSubPath();
    p.g.setColour(c.withAlpha(0.14f)); p.g.fillPath(path);
}

// Space: the dry hit, then the tail it leaves, as long as the decay and as loud as the mix, after its predelay.
void draw_space(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    float decay = norm(slot, info, "DECAY", norm(slot, info, "TIME", 0.5f)), mix = norm(slot, info, "MIX", 0.35f);
    float pre = norm(slot, info, "PREDELAY", 0), base = y + h;
    p.fill(x + 2, y + 2, 2, h - 2, colour::text, 1);
    float x0 = x + 10 + pre * w * 0.12f, length = (w - (x0 - x)) * (0.3f + 0.7f * decay), height = (h - 6) * (0.35f + 0.65f * mix);
    auto tail = curve([&](float u) { float t = u * (w - (x0 - x)) / std::max(1.0f, length); return base - height * std::exp(-4.0f * t) * (t <= 1.2f ? 1.0f : 0.0f); },
                      x0, w - (x0 - x));
    under(p, tail, x0, w - (x0 - x), base, c);
    stroke(p, tail, c, 1.8f);
}

// Time: the dry hit, then the repeats, a time apart, each fainter by the feedback.
void draw_time(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    float t = norm(slot, info, "TIME", norm(slot, info, "LENGTH", 0.4f)), fb = norm(slot, info, "FEEDBACK", norm(slot, info, "KEEP", 0.4f));
    float mix = norm(slot, info, "MIX", 0.3f), base = y + h, gap = w * (0.07f + 0.2f * t);
    bool ping = EffectKind(slot.kind) == EffectKind::PingPong;
    if (EffectKind(slot.kind) == EffectKind::SampleDelay) { // two delays, left and right, in milliseconds
        float l = norm(slot, info, "LEFT", 0), r = norm(slot, info, "RIGHT", 0.05f);
        p.fill(x + 2, y + 2, 3, h - 2, colour::text, 1);
        p.fill(x + 10 + l * (w - 20), y + h * 0.15f, 3, h * 0.85f, c, 1);
        p.fill(x + 10 + r * (w - 20), y + h * 0.15f, 3, h * 0.85f, c.withAlpha(0.6f), 1);
        return;
    }
    p.fill(x + 2, y + 2, 3, h - 2, colour::text, 1);
    float level = (h - 4) * (0.3f + 0.7f * mix);
    for (int i = 1; x + 2 + i * gap < x + w - 3 && level > 1.5f; ++i) {
        float tx = x + 2 + i * gap;
        if (ping && i % 2 == 0) p.fill(tx, y, 3, level * 0.5f, c.withAlpha(0.7f), 1); // the other side: from the top
        else p.fill(tx, base - level, 3, level, c, 1);
        level *= std::max(0.05f, fb);
    }
}

// Motion: the sweep as a wave, as many turns as its rate and as deep as its depth.
void draw_motion(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    float rate = norm(slot, info, "RATE", norm(slot, info, "SPEED", 0.3f)), depth = norm(slot, info, "DEPTH", 0.5f);
    float mid = y + h / 2, amp = (h / 2 - 2) * (0.15f + 0.85f * depth), turns = 1 + rate * 5;
    p.fill(x, mid - 0.5f, w, 1, colour::line);
    bool square = EffectKind(slot.kind) == EffectKind::Tremolo && raw(slot, info, "SHAPE", 0) == 1;
    auto wave = [&](float u, float sign) {
        float v = std::sin(u * turns * juce::MathConstants<float>::twoPi);
        if (square) v = v >= 0 ? 1.0f : -1.0f;
        return mid - sign * amp * v;
    };
    if (EffectKind(slot.kind) == EffectKind::AutoPan) stroke(p, curve([&](float u) { return wave(u, -1); }, x, w, 160), colour::mid, 1.2f); // the other side
    stroke(p, curve([&](float u) { return wave(u, 1); }, x, w, 160), c, 1.8f);
}

// Dirt: a wave before (faint) and after, and on the right the curve that bends it.
void draw_dirt(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    float drive = norm(slot, info, "DRIVE", norm(slot, info, "FUZZ", norm(slot, info, "AMOUNT", norm(slot, info, "SATURATION", 0.5f))));
    float k = 1 + drive * 6, wave_w = w * 0.66f, mid = y + h / 2, amp = h / 2 - 2;
    bool crush = EffectKind(slot.kind) == EffectKind::Crush;
    int levels = 3 + int(std::lround((raw(slot, info, "BITS", 8) - 2) / 14.0f * 9));
    auto shape = [&](float v) {
        if (crush) return std::round(v * levels) / levels;
        return std::tanh(k * v) / std::tanh(k);
    };
    auto sine = [&](float u) { return std::sin(u * 3 * juce::MathConstants<float>::twoPi); };
    stroke(p, curve([&](float u) { return mid - amp * sine(u); }, x, wave_w, 120), colour::dim, 1.2f);
    stroke(p, curve([&](float u) { return mid - amp * shape(sine(u)); }, x, wave_w, crush ? 360 : 120), c, 1.8f);
    float tx = x + w * 0.72f, tw = w - (tx - x);
    juce::Path diagonal; diagonal.startNewSubPath(tx, y + h); diagonal.lineTo(tx + tw, y);
    juce::Path dashed; juce::PathStrokeType(1).createDashedStroke(dashed, diagonal, std::array<float, 2> {2, 3}.data(), 2);
    p.g.setColour(colour::dim); p.g.fillPath(dashed);
    stroke(p, curve([&](float u) { return mid - (h / 2) * shape(u * 2 - 1); }, tx, tw, crush ? 200 : 60), c, 1.8f);
}

// Tone: what it does to each frequency, low to high, 0 dB the middle line.
void draw_tone(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    float mid = y + h / 2, scale = (h / 2 - 3) / 24.0f;
    std::function<float(float)> db;
    switch (EffectKind(slot.kind)) {
    case EffectKind::Filter: {
        float cut = norm(slot, info, "CUTOFF", 0.7f), reso = norm(slot, info, "RESO", 0.2f);
        int type = raw(slot, info, "TYPE", 0);
        db = [=](float u) {
            float d = u - cut, bump = reso * 16 * std::exp(-(d / 0.045f) * (d / 0.045f));
            float slope = type == 0 ? (d > 0 ? -60 * d : 0) : type == 2 ? (d < 0 ? 60 * d : 0) : -60 * std::abs(d);
            return std::max(-24.0f, slope + bump);
        };
        break;
    }
    case EffectKind::Eq: {
        float low = float(raw(slot, info, "LOW", 0)), midg = float(raw(slot, info, "MID", 0)), high = float(raw(slot, info, "HIGH", 0));
        float f = 0.25f + 0.5f * norm(slot, info, "MID FREQ", 0.5f);
        db = [=](float u) {
            auto shelf = [](float v) { return 1 / (1 + std::exp(-v)); };
            return low * shelf((0.18f - u) / 0.05f) + high * shelf((u - 0.82f) / 0.05f) + midg * std::exp(-((u - f) / 0.12f) * ((u - f) / 0.12f));
        };
        break;
    }
    case EffectKind::Width: { // the stereo field: a centre and two sides as wide as the width
        float width = norm(slot, info, "WIDTH", 0.7f), cx = x + w / 2;
        p.fill(x, mid - 0.5f, w, 1, colour::line);
        for (float side : {-1.0f, 1.0f}) {
            juce::Path ray; ray.startNewSubPath(cx, y + h); ray.lineTo(cx + side * w * 0.48f * width, y + 2);
            stroke(p, ray, c, 1.8f);
        }
        p.circle(cx, y + h - 2, 3, c);
        return;
    }
    default: { // Formant and Auto-wah: the peaks they sweep
        float centre = norm(slot, info, "CENTER", norm(slot, info, "VOWEL", 0.4f)), reso = norm(slot, info, "RESO", 0.6f);
        bool formant = EffectKind(slot.kind) == EffectKind::Formant;
        float f1 = formant ? 0.22f + 0.25f * centre : 0.15f + 0.7f * centre, f2 = 0.55f + 0.3f * (1 - centre);
        db = [=](float u) {
            float a = 18 * (0.4f + 0.6f * reso) * std::exp(-((u - f1) / 0.05f) * ((u - f1) / 0.05f));
            float b = formant ? 14 * (0.4f + 0.6f * reso) * std::exp(-((u - f2) / 0.05f) * ((u - f2) / 0.05f)) : 0;
            return std::max(-24.0f, a + b - 6);
        };
        break;
    }
    }
    p.fill(x, mid - 0.5f, w, 1, colour::line);
    auto path = curve([&](float u) { return mid - std::clamp(db(u), -24.0f, 24.0f) * scale; }, x, w, 140);
    under(p, path, x, w, mid, c);
    stroke(p, path, c, 1.8f);
}

// Dynamics: what comes out for what goes in (in dB), the threshold marked; the pumper its shape over the bar.
void draw_dynamics(Paint& p, const EffectSlot& slot, const EffectInfo& info, float x, float y, float w, float h, juce::Colour c) {
    auto kind = EffectKind(slot.kind);
    if (kind == EffectKind::Pumper || kind == EffectKind::Transient) { // a shape in time
        float depth = norm(slot, info, "DEPTH", 0.7f), attack = norm(slot, info, "ATTACK", 0.5f), sustain = norm(slot, info, "SUSTAIN", 0.5f);
        auto path = kind == EffectKind::Pumper
            ? curve([&](float u) { float t = std::fmod(u * 4, 1.0f); return y + 2 + (h - 4) * depth * std::exp(-5.0f * t); }, x, w, 160)
            : curve([&](float u) { return y + h - (h - 4) * (u < 0.04f ? u / 0.04f * (0.4f + 0.6f * attack) : (0.4f + 0.6f * attack) * std::exp(-(u - 0.04f) * (6 - 5 * sustain))); }, x, w, 160);
        under(p, path, x, w, y + h, c);
        stroke(p, path, c, 1.8f);
        return;
    }
    float side = std::min(w, h * 2.2f), bx = x + (w - side) / 2; // a square-ish graph, input along, output up
    auto px = [&](float db) { return bx + (db + 48) / 48 * side; };
    auto py = [&](float db) { return y + h - (db + 48) / 48 * h; };
    juce::Path diagonal; diagonal.startNewSubPath(px(-48), py(-48)); diagonal.lineTo(px(0), py(0));
    juce::Path dashed; juce::PathStrokeType(1).createDashedStroke(dashed, diagonal, std::array<float, 2> {2, 3}.data(), 2);
    p.g.setColour(colour::dim); p.g.fillPath(dashed);
    float threshold = float(raw(slot, info, "THRESH", raw(slot, info, "CEILING", -18)));
    float ratio = float(std::max(1, raw(slot, info, "RATIO", kind == EffectKind::Limiter ? 20 : 4)));
    float makeup = float(raw(slot, info, "MAKEUP", 0));
    auto out = [&](float in) {
        if (kind == EffectKind::Gate) return in < threshold ? -48.0f : in;
        if (kind == EffectKind::Limiter) return std::min(in, threshold);
        return std::min(0.0f, (in < threshold ? in : threshold + (in - threshold) / ratio) + makeup);
    };
    juce::Path path;
    for (int i = 0; i <= 96; ++i) {
        float in = -48 + 48.0f * float(i) / 96, o = std::max(-48.0f, out(in));
        if (i == 0) path.startNewSubPath(px(in), py(o)); else path.lineTo(px(in), py(o));
    }
    stroke(p, path, c, 1.8f);
    float t = std::clamp(threshold, -48.0f, 0.0f);
    p.circle(px(t), py(std::max(-48.0f, out(t))), 3, colour::text);
}
} // namespace

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

void effect_drawing(Paint& p, const EffectSlot& slot, float x, float y, float w, float h, juce::Colour c) {
    const auto& info = effect_info(EffectKind(slot.kind));
    if (slot.bypass) c = colour::dim;
    switch (info.family) {
    case Family::Space: draw_space(p, slot, info, x, y, w, h, c); break;
    case Family::Time: draw_time(p, slot, info, x, y, w, h, c); break;
    case Family::Motion: draw_motion(p, slot, info, x, y, w, h, c); break;
    case Family::Dirt: draw_dirt(p, slot, info, x, y, w, h, c); break;
    case Family::Tone: draw_tone(p, slot, info, x, y, w, h, c); break;
    case Family::Dynamics: draw_dynamics(p, slot, info, x, y, w, h, c); break;
    default: break;
    }
}

void fx_strip(Paint& p, const Chain& chain, unsigned cursor, juce::Colour c, float y) {
    constexpr float gap = 4, h = 40;
    unsigned count = chain.count(), size = std::clamp<unsigned>(chain.size, 1, max_slots);
    float w = (zone::width - 2 * zone::pad - (size - 1) * gap) / size;
    for (unsigned n = 0; n < size; ++n) {
        float x = zone::pad + n * (w + gap), cx = x + w / 2;
        if (n >= count) { // open: a dot, the next one to fill a plus (lit when it is the one in hand)
            if (n == count) {
                if (n == cursor) p.fill(x, y, w, h, c.withAlpha(0.16f), 4);
                icon(p, Icon::Plus, cx - 7, y + h / 2 - 7, 14, n == cursor ? c : colour::dim, 1.8f);
            } else p.circle(cx, y + h / 2, 2, colour::dim);
            continue;
        }
        const auto& slot = chain.slots[n];
        const auto& info = effect_info(EffectKind(slot.kind));
        bool here = n == cursor;
        p.fill(x, y, w, h, here ? c.withAlpha(0.2f) : colour::surf, 4);
        auto ink = slot.bypass ? colour::dim : here ? c : colour::sec;
        icon(p, family_icon(info.family), cx - 9, y + 4, 18, ink, 1.5f);
        juce::String name = info.family == Family::Plugins ? juce::String("Plug-in") : juce::String(info.name);
        auto style = plain(slot.bypass ? colour::dim : here ? colour::text : colour::sec, type::small, Weight::Medium);
        if (slot.kind == uint8_t(EffectKind::Compressor) && p.width(name, p.fit(name, style, w - 4, type::floor)) > w - 4) name = "Comp"; // too long for a slot
        p.text(name, x + 2, y + 24, w - 4, 14, p.fit(name, style, w - 4, type::floor), juce::Justification::centred);
        if (slot.bypass) p.fill(x + 6, y + h / 2, w - 12, 1.2f, colour::dim); // struck through
    }
}

void fx_in_hand(Paint& p, const Snapshot& s, const Chain& chain, unsigned cursor, juce::Colour c, float y, float bottom) {
    constexpr float x = zone::pad, w = zone::width - 2 * zone::pad;
    unsigned count = chain.count();
    if (cursor >= count) { // the open slot: Push adds one
        if (count >= chain.size) return;
        float mid = zone::width / 2.0f, gy = y + (bottom - y) / 2 - 13;
        icon(p, Icon::Push, mid - 28, gy, 26, colour::sec, 1.5f);
        icon(p, Icon::Plus, mid + 6, gy + 4, 18, colour::sec, 1.8f);
        return;
    }
    const auto& slot = chain.slots[cursor];
    const auto& info = effect_info(EffectKind(slot.kind));
    bool plugin = info.family == Family::Plugins;
    juce::String name = plugin ? plugin_name(s, unsigned(slot.params[0])) : juce::String(info.name);
    juce::String family = plugin ? juce::String("PLUG-IN") : juce::String(family_name(info.family)).toUpperCase();
    auto fam = caps_text(colour::mid);
    float fw = p.width(family, fam) + 8;
    auto title = plain(slot.bypass ? colour::mid : colour::text, type::title, Weight::SemiBold);
    float room = w - fw - 28;
    float nw = p.text(name, x, y, room, 24, p.fit(name, title, room, type::floor));
    p.fill(x + nw + 8, y + 5, fw, 15, colour::surf, 3);
    p.text(family, x + nw + 8, y + 5, fw, 15, fam, juce::Justification::centred);
    if (slot.bypass) draw_glyph(p, Glyph::LedOff, x + nw + 8 + fw + 6, y + 12.5f, colour::mid);
    float dy = y + 30, dh = bottom - dy;
    if (!plugin) { effect_drawing(p, slot, x, dy, w, dh, c); return; }
    // A plug-in draws nothing of its own here: its state, and which row of its parameters the cards hold.
    const auto& panel = s.ui.plugin_panel;
    bool ours = panel.entry == unsigned(slot.params[0]);
    icon(p, panel.status == PluginStatus::Failed || panel.status == PluginStatus::Crashed ? Icon::PlugBroken : Icon::Plug, x, dy + 4, 28, colour::dim, 1.4f);
    juce::String state = !ours || panel.status == PluginStatus::Loading || panel.status == PluginStatus::None ? "OPENING"
                       : panel.status == PluginStatus::Failed ? "WON'T OPEN" : panel.status == PluginStatus::Crashed ? "STOPPED" : "";
    if (state.isNotEmpty()) p.text(state, x + 38, dy + 10, w - 38, 16, caps_text(panel.status == PluginStatus::Loading || !ours ? colour::mid : colour::rec));
    else if (panel.rows > 1) page_dots(p, x + 38, dy + 18, int(std::min<unsigned>(panel.rows, 24)), int(panel.row), c);
}

} // namespace melodee
