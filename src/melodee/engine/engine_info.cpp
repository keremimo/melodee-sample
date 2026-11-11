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
    // MIDI (docs/MIDI_TRACKS.md, phase M1): where the track's notes go out; bank and
    // program (Off sends none), three controllers for Stage's CC page.
    static const char* const midi_ports[] {"External", "USB-C", "All USB"};
    static const ParamInfo midi_params[] {
        {"PORT", 0, midi_track::PortCount - 1, midi_track::External, Format::Choice, midi_ports},
        {"CHANNEL", 1, 16, 1, Format::Number, nullptr},
        {"BANK MSB", 0, 128, 0, Format::Bank, nullptr}, {"BANK LSB", 0, 128, 0, Format::Bank, nullptr},
        {"PROGRAM", 0, 128, 0, Format::Program, nullptr},
        {"CC NUMBER", 0, 127, 74, Format::CcNumber, nullptr}, {"CC NUMBER", 0, 127, 71, Format::CcNumber, nullptr},
        {"CC NUMBER", 0, 127, 1, Format::CcNumber, nullptr},
        {"CC", 0, 127, 64, Format::Number, nullptr}, {"CC", 0, 127, 64, Format::Number, nullptr}, {"CC", 0, 127, 0, Format::Number, nullptr},
    };
    static const EditPage midi_pages[] {
        {"Device", "DEVICE", Picture::Route, {{{midi_track::Port, midi_track::Channel, -1}, {-1, -1, -1}, {-1, -1, -1}}}},
        {"Program", "PROGRAM", Picture::Bars, {{{midi_track::BankMsb, midi_track::BankLsb, midi_track::Program}, {-1, -1, -1}, {-1, -1, -1}}}},
    };
    static const Sound midi_sounds[] {{"MIDI OUT", {{{-1, 0}}}}};
    static const EngineInfo midi {EngineKind::Midi, "MIDI", true, midi_params, midi_track::Count, midi_pages, 2,
                                  {midi_track::Port, midi_track::Channel, -1}, midi_sounds, 1, nullptr, 0, false};
    // PLUGIN (docs/MELODEE.md, Plug-ins): a plug-in instrument from the card; its parameters are its own (the
    // rack's), so the engine states none here and its Edit page draws the plug-in's.
    static const Sound plugin_sounds[] {{"PLUG-IN", {{{-1, 0}}}}};
    static const StageGroups plugin_stage {{{
        {{{stage_mix, {}, {}}}, 1},
        {{{stage_arp, stage_chord, {}}}, 2},
        {{{stage_fx, {}, {}}}, 1},
    }}};
    static const EngineInfo plugin {EngineKind::Plugin, "PLUGIN", true, nullptr, 0, nullptr, 0, {-1, -1, -1},
                                    plugin_sounds, 1, nullptr, 0, false, &plugin_stage};
    switch (kind) {
    case EngineKind::Midi: return midi;
    case EngineKind::Plugin: return plugin;
    case EngineKind::Prophet: return prophet::info();
    case EngineKind::Fm6: return fm6::info();
    case EngineKind::Cz: return cz::info();
    case EngineKind::Sid: return sid::info();
    case EngineKind::Kit909: return kit909::info();
    case EngineKind::Sampler: return sampler;
    default: return none;
    }
}

const StageGroups& stage_groups(EngineKind kind) {
    using K = StageKind;
    using P = StagePicture;
    constexpr int8_t _ = -1;
    // A kit: the MPC's Sample Mode pages in Akai's order, on the last pad hit; Melodee's own join
    // the group they belong with, as on the sampler pages: Follow and Cloud B2's, Kit filter B3's.
    static const StageGroups kit {{{
        {{{{"TRIM", K::Trim, P::Wave}, {"MIX", K::PadMix, P::Wave}, {"AMP ENV", K::AmpEnv, P::Wave}}}, 3},
        {{{{"TUNE", K::Tune, P::Wave}, {"PLAY", K::Play, P::Wave}, {"FOLLOW", K::Follow, P::Wave}, {"CLOUD", K::Cloud, P::Wave}}}, 4},
        {{{{"FILTER", K::Filter, P::Wave}, {"FILT ENV", K::FiltEnv, P::Wave}, {"KIT FILT", K::KitFilter, P::Wave}}}, 3},
    }}};
    // MIDI: where the notes go, bank and program; three controllers; the arp.
    using namespace midi_track;
    static const StageGroups midi {{{
        {{{{"DEVICE", K::Params, P::Route, {{Port, Channel, _}}},
           {"PROGRAM", K::Params, P::Program, {{BankMsb, BankLsb, Program}}}, {}}}, 2},
        {{{{"CC", K::Params, P::Cc, {{Cc1, Cc2, Cc3}}, {{CcNumber1, CcNumber2, CcNumber3}}}, {}, {}}}, 1},
        {{{stage_arp, stage_chord, {}}}, 2},
    }}};
    // An empty track: what it becomes.
    static const StageGroups empty {{{
        {{{{"SYNTH", K::NewSynth, P::Engines}, {}, {}}}, 1},
        {{{{"KIT", K::NewKit, P::Sources}, {}, {}}}, 1},
        {{{{"MIDI", K::NewMidi, P::Ports}, {}, {}}}, 1},
    }}};
    // An engine that states none: its sounds and mix, the arp, its effects.
    static const StageGroups plain {{{
        {{{stage_preset, stage_mix, {}}}, 2},
        {{{stage_arp, stage_chord, {}}}, 2},
        {{{stage_fx, {}, {}}}, 1},
    }}};
    switch (kind) {
    case EngineKind::None: return empty;
    case EngineKind::Sampler: return kit;
    case EngineKind::Midi: return midi;
    default: {
        const auto* own = engine_info(kind).stage;
        return own ? *own : plain;
    }
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
    case Format::Bank: return {v ? std::to_string(v - 1) : "Off", ""};
    case Format::Program: return {v ? std::to_string(v) : "Off", ""};
    case Format::CcNumber: return {std::to_string(v), ""};
    case Format::Number:
    default: return {std::to_string(v), ""};
    }
}

float gauge_of(const ParamInfo& p, int value) {
    return p.max > p.min ? float(value - p.min) / float(p.max - p.min) : 0;
}
bool bipolar(const ParamInfo& p) { return p.min < 0 && p.max > 0; }

} // namespace melodee
