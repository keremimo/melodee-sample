// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_info.h"
#include "melodee/engine/midi_engine.h"
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
        {"MODE", 0, 2, 0, Format::Choice, follow_hows}, {"PITCH", 0, 1, 0, Format::Choice, pitch_hows},
    };
    static const Sound kits[] {{"NEW KIT", {{{-1, 0}}}}};
    static const EngineInfo sampler {EngineKind::Sampler, "SAMPLER", true, kit_params, sampler::Count, nullptr, 0,
                                     {sampler::Tune, sampler::Filter, sampler::Decay}, kits, 1};
    // MIDI (docs/MIDI_TRACKS.md): where the track's notes go out; bank and program (Off sends none); 24 knob slots
    // (the first three on Stage's CC page), each slot's address; pressure, bend, delay, pads, an input as its sound,
    // SysEx on load, each pattern's own program (roadmap P4, M2-M5).
    static const char* const midi_ports[] {"External", "USB-C", "All USB", "USB device"};
    static const char* const pressure_outs[] {"Poly", "Channel", "Off"};
    static const char* const audio_ins[] {"None", "Rear L", "Rear R", "Rear", "Mic", "USB"};
    static const auto midi_params = [] {
        using namespace midi_track;
        std::array<ParamInfo, Count> p {};
        p[Port] = {"PORT", 0, PortCount - 1, External, Format::Choice, midi_ports};
        p[Channel] = {"CHANNEL", 1, 16, 1, Format::Number, nullptr};
        p[BankMsb] = {"BANK MSB", 0, 128, 0, Format::Bank, nullptr};
        p[BankLsb] = {"BANK LSB", 0, 128, 0, Format::Bank, nullptr};
        p[Program] = {"PROGRAM", 0, 128, 0, Format::Program, nullptr};
        const int16_t first[3] {74, 71, 1}, value[3] {64, 64, 0};
        for (int k = 0; k < slots; ++k) {
            p[size_t(slot_address(k))] = {"SLOT", -1, address_last, int16_t(k < 3 ? first[k] : -1), Format::MidiSlot, nullptr};
            p[size_t(slot_value(k))] = {"CC", 0, 127, int16_t(k < 3 ? value[k] : 0), Format::Number, nullptr};
        }
        p[PressureOut] = {"PRESSURE", 0, 2, PressurePoly, Format::Choice, pressure_outs};
        p[Bend] = {"BEND", -64, 63, 0, Format::Number, nullptr};
        p[Delay] = {"DELAY", -50, 50, 0, Format::Ms, nullptr};
        p[AudioIn] = {"INPUT", 0, AudioCount - 1, AudioNone, Format::Choice, audio_ins};
        p[AudioMonitor] = {"MONITOR", 0, 1, 1, Format::Switch, nullptr};
        p[AudioLevel] = {"LEVEL", 0, 127, 100, Format::Level, nullptr};
        p[SysexOnLoad] = {"ON LOAD", 0, 1, 0, Format::Switch, nullptr};
        for (int n = 0; n < 16; ++n) p[size_t(PatternProgram0 + n)] = {"PATTERN PROG", 0, 128, 0, Format::Program, nullptr};
        return p;
    }();
    static const auto midi_pages = [] {
        using namespace midi_track;
        std::array<EditPage, 6 + slot_pages> pages {};
        constexpr int8_t no = -1;
        pages[0] = {"Device", "DEVICE", Picture::Route, {{{Port, Channel, Delay}, {no, no, no}, {no, no, no}}}};
        // PROGRAM's second row: K3 the pattern playing's own program (shell/bindings.cpp turns it to that pattern's).
        pages[1] = {"Program", "PROGRAM", Picture::Bars, {{{BankMsb, BankLsb, Program}, {no, no, PatternProgram0}, {no, no, no}}}};
        static const char* const slot_titles[] {"Slots 1", "Slots 2", "Slots 3", "Slots 4", "Slots 5", "Slots 6", "Slots 7", "Slots 8"};
        static const char* const slot_keys[] {"SLOTS 1", "SLOTS 2", "SLOTS 3", "SLOTS 4", "SLOTS 5", "SLOTS 6", "SLOTS 7", "SLOTS 8"};
        for (int s = 0; s < slot_pages; ++s) // values on the first row, what each sends on the second (Minus / Plus)
            pages[size_t(2 + s)] = {slot_titles[s], slot_keys[s], Picture::Bars,
                                    {{{int8_t(slot_value(3 * s)), int8_t(slot_value(3 * s + 1)), int8_t(slot_value(3 * s + 2))},
                                      {int8_t(slot_address(3 * s)), int8_t(slot_address(3 * s + 1)), int8_t(slot_address(3 * s + 2))}, {no, no, no}}}};
        // PADS: K2 the last pad's note on Drum (shell/bindings.cpp); EXPRESS: bend and pressure; AUDIO; SYSEX.
        pages[2 + slot_pages] = {"Pads", "PADS", Picture::Pads, {{{no, no, no}, {no, no, no}, {no, no, no}}}}; // K1 Keys / Drum, K2 a drum's note (bindings)
        pages[3 + slot_pages] = {"Express", "EXPRESS", Picture::Bars, {{{Bend, PressureOut, no}, {no, no, no}, {no, no, no}}}};
        pages[4 + slot_pages] = {"Audio", "AUDIO", Picture::Bars, {{{AudioIn, AudioMonitor, AudioLevel}, {no, no, no}, {no, no, no}}}};
        pages[5 + slot_pages] = {"SysEx", "SYSEX", Picture::Bars, {{{no, SysexOnLoad, no}, {no, no, no}, {no, no, no}}}};
        return pages;
    }();
    static const Sound midi_sounds[] {{"MIDI OUT", {{{-1, 0}}}}};
    static const EngineInfo midi {EngineKind::Midi, "MIDI", true, midi_params.data(), midi_track::Count, midi_pages.data(), unsigned(midi_pages.size()),
                                  {midi_track::Port, midi_track::Channel, -1}, midi_sounds, 1, nullptr, 0, false};
    // PLUGIN (docs/MELODEE.md, Plug-ins): a plug-in instrument from the card; its parameters are its own (the
    // rack's), so the engine states none here and its Edit page draws the plug-in's. Melodee's synths are plug-ins
    // of this kind (Melodee Poly, PD and Chip, docs/MELODEE.md, Synths).
    static const Sound plugin_sounds[] {{"PLUG-IN", {{{-1, 0}}}}};
    // B1 the plug-in: its presets (or its face when it has none), then its parameters; B2 ARP, CHORD; B3 MIX,
    // SENDS, FX (engine_info.h, stage_mix).
    static const StageGroups plugin_stage {{{
        {{{{"PRESETS", StageKind::PluginSound, StagePicture::PluginSound}, {"EDIT", StageKind::PluginEdit, StagePicture::PluginEdit}}}, 2},
        {{{stage_arp, stage_chord}}, 2},
        {{{stage_mix, stage_sends, stage_fx}}, 3},
    }}};
    static const EngineInfo plugin {EngineKind::Plugin, "PLUGIN", true, nullptr, 0, nullptr, 0, {-1, -1, -1},
                                    plugin_sounds, 1, nullptr, 0, false, &plugin_stage};
    switch (kind) {
    case EngineKind::Midi: return midi;
    case EngineKind::Plugin: return plugin;
    case EngineKind::Sampler: return sampler;
    default: return none;
    }
}

const StageGroups& stage_groups(EngineKind kind) {
    using K = StageKind;
    using P = StagePicture;
    constexpr int8_t _ = -1;
    // A kit, on the last pad hit: B1 the sample (trim, layers, tune, its envelope), B2 how it plays (as
    // the sampler pages, shell/sampler_map.h), B3 where it goes (mix, sends) and its filters.
    static const StageGroups kit {{{
        {{{{"TRIM", K::Trim, P::Wave}, {"LAYERS", K::Layers, P::Wave}, {"TUNE", K::Tune, P::Wave}, {"AMP ENV", K::AmpEnv, P::Wave}}}, 4},
        {{{{"PLAY", K::Play, P::Wave}, {"FOLLOW", K::Follow, P::Wave}, {"CLOUD", K::Cloud, P::Wave}}}, 3},
        {{{{"MIX", K::PadMix, P::Wave}, stage_sends, {"FILTER", K::Filter, P::Wave}, {"FILT ENV", K::FiltEnv, P::Wave}, {"KIT FILT", K::KitFilter, P::Wave}}}, 5},
    }}};
    // MIDI: B1 where the notes go, bank and program, three controllers; B2 the arp and chord; B3 nothing (no audio).
    using namespace midi_track;
    static const StageGroups midi {{{
        {{{{"DEVICE", K::Params, P::Route, {{Port, Channel, _}}},
           {"PROGRAM", K::Params, P::Program, {{BankMsb, BankLsb, Program}}},
           {"CC", K::Params, P::Cc, {{Cc1, Cc2, Cc3}}, {{CcNumber1, CcNumber2, CcNumber3}}}}}, 3},
        {{{stage_arp, stage_chord}}, 2},
        {{{}}, 0},
    }}};
    // An empty track: what it becomes.
    static const StageGroups empty {{{
        {{{{"SYNTH", K::NewSynth, P::Engines}, {}, {}}}, 1},
        {{{{"KIT", K::NewKit, P::Sources}, {}, {}}}, 1},
        {{{{"MIDI", K::NewMidi, P::Ports}, {}, {}}}, 1},
    }}};
    static const StageGroups nothing {}; // an engine that states none (there is none left)
    switch (kind) {
    case EngineKind::None: return empty;
    case EngineKind::Sampler: return kit;
    case EngineKind::Midi: return midi;
    default: {
        const auto* own = engine_info(kind).stage;
        return own ? *own : nothing;
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
    case Format::MidiSlot: {
        using namespace midi_track;
        switch (slot_kind(v)) {
        case SlotKind::Off: return {"Off", ""};
        case SlotKind::Cc: return {std::to_string(v), "CC"};
        case SlotKind::Cc14: return {std::to_string(slot_number(v)), "CC14"};
        case SlotKind::PitchBend: return {"Bend", ""};
        case SlotKind::ChannelPressure: return {"Press", ""};
        case SlotKind::Nrpn: return {std::to_string(slot_number(v)), "NRPN"};
        }
        return {"", ""};
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
