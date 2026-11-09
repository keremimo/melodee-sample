// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_info.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/sampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace melodee {

const EngineInfo& engine_info(EngineKind kind) {
    // Engines still to be written keep their name so a project can say what it needs.
    static const EngineInfo none {EngineKind::None, "EMPTY", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    static const EngineInfo fm6 {EngineKind::Fm6, "FM6", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    static const EngineInfo cz {EngineKind::Cz, "CZ", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    static const EngineInfo sid {EngineKind::Sid, "SID", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    // The sampler's kit-wide values; each pad's own live in Track::pads.
    static const char* const filter_types[] {"Low pass", "Band pass", "High pass"};
    static const ParamInfo kit_params[] {
        {"TUNE", -24, 24, 0, Format::Semitones, nullptr}, {"FILTER", 0, 100, 100, Format::Percent, nullptr},
        {"RESO", 0, 100, 10, Format::Percent, nullptr}, {"DECAY", 0, 100, 100, Format::Percent, nullptr},
        {"LEVEL", 0, 127, 100, Format::Number, nullptr},
        {"TYPE", 0, 2, 0, Format::Choice, filter_types}, {"FOLLOW", 0, 1, 0, Format::Switch, nullptr},
        {"CLOUD", 0, 1, 0, Format::Switch, nullptr}, {"POSITION", 0, 100, 38, Format::Percent, nullptr},
        {"SIZE", 0, 100, 50, Format::Percent, nullptr}, {"DENSITY", 0, 100, 24, Format::Number, nullptr},
        {"SCAN", 0, 100, 50, Format::Percent, nullptr}, {"SPREAD", 0, 100, 12, Format::Cents, nullptr},
    };
    static const Sound kits[] {{"NEW KIT", {{{-1, 0}}}}};
    static const EngineInfo sampler {EngineKind::Sampler, "SAMPLER", true, kit_params, sampler::Count, nullptr, 0,
                                     {sampler::Tune, sampler::Filter, sampler::Decay}, kits, 1};
    switch (kind) {
    case EngineKind::Prophet: return prophet::info();
    case EngineKind::Fm6: return fm6;
    case EngineKind::Cz: return cz;
    case EngineKind::Sid: return sid;
    case EngineKind::Kit909: return kit909::info();
    case EngineKind::Sampler: return sampler;
    default: return none;
    }
}

unsigned edit_page_count(const EngineInfo& info) { return info.page_count ? info.page_count + 1 : 0; }
bool is_mod_page(const EngineInfo& info, unsigned index) { return info.page_count && index % edit_page_count(info) == info.page_count; }
const EditPage& edit_page_at(const EngineInfo& info, unsigned index) {
    static const EditPage mod {"Mod matrix", "MOD", Picture::Mod, {{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}}};
    if (!info.page_count || is_mod_page(info, index)) return mod;
    return info.pages[index % edit_page_count(info)];
}

unsigned available_engines(std::array<EngineKind, size_t(EngineKind::Count)>& out) {
    unsigned n = 0;
    for (unsigned k = 1; k < unsigned(EngineKind::Count); ++k)
        if (engine_info(EngineKind(k)).available) out[n++] = EngineKind(k);
    return n;
}

void sound_values(const EngineInfo& info, unsigned sound, std::array<int16_t, max_params>& out) {
    out.fill(0);
    for (unsigned p = 0; p < info.param_count && p < max_params; ++p) out[p] = info.params[p].initial;
    if (sound >= info.sound_count) return;
    for (const auto& change : info.sounds[sound].changes) {
        if (change[0] < 0) break;
        const auto& p = info.params[change[0]];
        out[size_t(change[0])] = int16_t(std::clamp<int>(change[1], p.min, p.max));
    }
}

void load_sound(Track& track, EngineKind kind, unsigned sound) {
    const auto& info = engine_info(kind);
    track.engine = kind;
    track.preset = uint16_t(sound < info.sound_count ? sound : 0);
    sound_values(info, track.preset, track.params);
    track.set_name(info.sound_count ? info.sounds[track.preset].name : "");
}

float time_seconds(int value) { return 0.001f * std::pow(10000.0f, std::clamp(value, 0, 100) / 100.0f); }
float rate_hertz(int value) { return 0.05f * std::pow(600.0f, std::clamp(value, 0, 100) / 100.0f); }

Shown show(const ParamInfo& p, int v) {
    char text[32];
    auto number = [&](const char* format, auto x) { std::snprintf(text, sizeof(text), format, x); return std::string(text); };
    switch (p.format) {
    case Format::Percent: return {std::to_string(v), "%"};
    case Format::Semitones: return {v > 0 ? "+" + std::to_string(v) : std::to_string(v), "st"};
    case Format::Cents: return {v > 0 ? "+" + std::to_string(v) : std::to_string(v), "ct"};
    case Format::Switch: return {v ? "On" : "Off", ""};
    case Format::Choice: return {p.choices ? p.choices[std::clamp<int>(v, p.min, p.max) - p.min] : std::to_string(v), ""};
    case Format::Time: {
        float s = time_seconds(v);
        if (s < 0.1f) return {number("%.0f", s * 1000), "ms"};
        if (s < 1) return {number("%.0f", s * 1000), "ms"};
        return {number(s < 10 ? "%.1f" : "%.0f", s), "s"};
    }
    case Format::Rate: {
        float hz = rate_hertz(v);
        return {number(hz < 1 ? "%.2f" : hz < 10 ? "%.1f" : "%.0f", hz), "Hz"};
    }
    case Format::Number:
    default: return {std::to_string(v), ""};
    }
}

float gauge_of(const ParamInfo& p, int value) {
    return p.max > p.min ? float(value - p.min) / float(p.max - p.min) : 0;
}
bool bipolar(const ParamInfo& p) { return p.min < 0 && p.max > 0; }

} // namespace melodee
