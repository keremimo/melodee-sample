// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_info.h"
#include "melodee/engine/prophet_params.h"
#include "melodee/engine/cz.h"
#include "melodee/engine/fm6.h"
#include "melodee/engine/kit909.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/sid.h"
#include "melodee/engine/sampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace melodee {

const EngineInfo& engine_info(EngineKind kind) {
    // Engines still to be written keep their name so a project can say what it needs.
    static const EngineInfo none {EngineKind::None, "EMPTY", false, nullptr, 0, nullptr, 0, {-1, -1, -1}, nullptr, 0};
    // The sampler's kit-wide values; each pad's own live in Track::pads.
    static const char* const filter_types[] {"Low pass", "Band pass", "High pass"};
    static const char* const follow_hows[] {"Beats", "Tones", "Tape"};
    static const char* const pitch_hows[] {"Speed", "Stretch"};
    static const ParamInfo kit_params[] {
        {"TUNE", -24, 24, 0, Format::Semitones, nullptr}, {"FILTER", 0, 100, 100, Format::Percent, nullptr},
        {"RESO", 0, 100, 10, Format::Percent, nullptr}, {"DECAY", 0, 100, 100, Format::Percent, nullptr},
        {"KIT VOL", 0, 127, 100, Format::Level, nullptr},
        {"TYPE", 0, 2, 0, Format::Choice, filter_types}, {"FOLLOW", 0, 1, 0, Format::Switch, nullptr},
        {"CLOUD", 0, 1, 0, Format::Switch, nullptr}, {"POSITION", 0, 100, 38, Format::Percent, nullptr},
        {"SIZE", 0, 100, 50, Format::Percent, nullptr}, {"DENSITY", 0, 100, 24, Format::Number, nullptr},
        {"SCAN", 0, 100, 50, Format::Percent, nullptr}, {"SPREAD", 0, 100, 12, Format::Cents, nullptr},
        {"HOW", 0, 2, 0, Format::Choice, follow_hows}, {"PITCH", 0, 1, 0, Format::Choice, pitch_hows},
    };
    static const Sound kits[] {{"NEW KIT", {{{-1, 0}}}}};
    static const EngineInfo sampler {EngineKind::Sampler, "SAMPLER", true, kit_params, sampler::Count, nullptr, 0,
                                     {sampler::Tune, sampler::Filter, sampler::Decay}, kits, 1};
    // MIDI (docs/MIDI_TRACKS.md, phase M1): where the track's notes go out.
    static const char* const midi_ports[] {"External", "USB-C", "All USB"};
    static const ParamInfo midi_params[] {
        {"PORT", 0, midi_track::PortCount - 1, midi_track::External, Format::Choice, midi_ports},
        {"CHANNEL", 1, 16, 1, Format::Number, nullptr},
    };
    static const EditPage midi_pages[] {{"Device", "DEVICE", Picture::Route, {{{midi_track::Port, midi_track::Channel, -1}, {-1, -1, -1}, {-1, -1, -1}}}}};
    static const Sound midi_sounds[] {{"MIDI OUT", {{{-1, 0}}}}};
    static const EngineInfo midi {EngineKind::Midi, "MIDI", true, midi_params, midi_track::Count, midi_pages, 1,
                                  {midi_track::Port, midi_track::Channel, -1}, midi_sounds, 1, nullptr, 0, false};
    switch (kind) {
    case EngineKind::Midi: return midi;
    case EngineKind::Prophet: return prophet::info();
    case EngineKind::Fm6: return fm6::info();
    case EngineKind::Cz: return cz::info();
    case EngineKind::Sid: return sid::info();
    case EngineKind::Kit909: return kit909::info();
    case EngineKind::Sampler: return sampler;
    default: return none;
    }
}

unsigned edit_page_count(const EngineInfo& info) { return info.page_count ? info.page_count + (info.mod_matrix ? 1 : 0) : 0; }
bool is_mod_page(const EngineInfo& info, unsigned index) {
    return info.page_count && info.mod_matrix && index % edit_page_count(info) == info.page_count;
}
const EditPage& edit_page_at(const EngineInfo& info, unsigned index) {
    static const EditPage mod {"Mod matrix", "MOD", Picture::Mod, {{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}}};
    if (!info.page_count || is_mod_page(info, index)) return mod;
    return info.pages[index % edit_page_count(info)];
}

const char* mod_dest_label(const EngineInfo& info, ModDest dest) {
    if (unsigned(dest) >= unsigned(ModDest::Count)) return nullptr;
    if (!info.mod_dests || dest == ModDest::None) return mod_dest_name(dest);
    return info.mod_dests[unsigned(dest)];
}
unsigned mod_dest_list(const EngineInfo& info, std::array<uint8_t, size_t(ModDest::Count)>& out) {
    unsigned n = 0;
    for (unsigned d = 0; d < unsigned(ModDest::Count); ++d)
        if (mod_dest_label(info, ModDest(d))) out[n++] = uint8_t(d);
    return n;
}

void upgrade_params(EngineKind kind, unsigned version, std::array<int16_t, max_params>& params) {
    const auto& info = engine_info(kind);
    if (version >= 1 || !info.legacy_params) return;
    for (unsigned p = info.legacy_params; p < info.param_count && p < max_params; ++p) params[p] = info.params[p].initial;
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

std::string decibels(int level) {
    if (level <= 0) return "-inf";
    char text[16];
    float db = volume_db(level);
    std::snprintf(text, sizeof(text), db > 0.005f ? "+%.1f" : "%.1f", db);
    return text;
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
    case Format::Level: return {decibels(v), "dB"};
    case Format::Ms: return {std::to_string(v), "ms"};
    case Format::Decibels: return {v > 0 ? "+" + std::to_string(v) : std::to_string(v), "dB"};
    case Format::Tenths: return {number("%.1f", v / 10.0), ""};
    case Format::Seconds: return {number("%.1f", v / 10.0), "s"};
    case Format::Hertz: return v >= 1000 ? Shown {number("%.1f", v / 1000.0), "kHz"} : Shown {std::to_string(v), "Hz"};
    case Format::CentiHz: return {number(v < 1000 ? "%.2f" : "%.1f", v / 100.0), "Hz"};
    case Format::Number:
    default: return {std::to_string(v), ""};
    }
}

float gauge_of(const ParamInfo& p, int value) {
    return p.max > p.min ? float(value - p.min) / float(p.max - p.min) : 0;
}
bool bipolar(const ParamInfo& p) { return p.min < 0 && p.max > 0; }

} // namespace melodee
