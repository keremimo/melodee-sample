// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_info.h"
#include "melodee/engine/prophet_params.h"
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
    static const EngineInfo kit {EngineKind::Kit909, "909", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    static const EngineInfo sampler {EngineKind::Sampler, "SAMPLER", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    switch (kind) {
    case EngineKind::Prophet: return prophet::info();
    case EngineKind::Fm6: return fm6;
    case EngineKind::Cz: return cz;
    case EngineKind::Sid: return sid;
    case EngineKind::Kit909: return kit;
    case EngineKind::Sampler: return sampler;
    default: return none;
    }
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
