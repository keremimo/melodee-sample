// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// An MPC keygroup program's keygroups and layers as zones. The tags are read
// as the MPC's own files and the open converters that read them have them:
// RootNote is one past the MIDI note, a layer's Volume 0.353..1 is -12..+6 dB,
// Pan 0..1 is left to right, an envelope's 0..1 is 1 ms to 100 s, and the
// sample is its SampleName with ".WAV", beside the program.
#include "melodee/engine/keygroup.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace melodee {
namespace {
constexpr uint32_t to_its_end = 0x7fffffff; // a loop's end: the file's last frame (the loader clamps it there)

std::string trimmed(const std::string& text) {
    size_t from = 0, to = text.size();
    while (from < to && std::isspace(static_cast<unsigned char>(text[from]))) ++from;
    while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1]))) --to;
    return text.substr(from, to - from);
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::optional<std::string> text_at(const KeygroupTags& tags, const char* name) {
    auto found = tags.find(name);
    if (found == tags.end()) return std::nullopt;
    auto text = trimmed(found->second);
    if (text.empty()) return std::nullopt;
    return text;
}

std::optional<double> number_at(const KeygroupTags& tags, const char* name) {
    auto text = text_at(tags, name);
    if (!text) return std::nullopt;
    char* end = nullptr;
    double value = std::strtod(text->c_str(), &end);
    if (end == text->c_str() || *end || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<int> int_at(const KeygroupTags& tags, const char* name) {
    auto value = number_at(tags, name);
    if (!value || std::fabs(*value) > 1e9) return std::nullopt;
    return int(std::lround(*value));
}

std::optional<bool> flag_at(const KeygroupTags& tags, const char* name) {
    auto text = text_at(tags, name);
    if (!text) return std::nullopt;
    auto t = lowered(*text);
    if (t == "true" || t == "1") return true;
    if (t == "false" || t == "0") return false;
    return std::nullopt;
}

bool ends_with(const std::string& text, const std::string& end) {
    return text.size() >= end.size() && !text.compare(text.size() - end.size(), end.size(), end);
}

// The file a layer plays, beside the program. SampleName is kept whole: some
// end with a space, and the file's name with it.
std::string sample_of(const KeygroupTags& layer) {
    std::string name;
    if (auto found = layer.find("SampleName"); found != layer.end()) {
        name = found->second;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (trimmed(name).empty()) name.clear();
    }
    if (name.empty()) {
        // SampleFile, where a program has it, is a path on the machine that wrote it: only its last part.
        auto file = text_at(layer, "SampleFile");
        if (!file) return {};
        auto cut = file->find_last_of("/\\");
        return cut == std::string::npos ? *file : file->substr(cut + 1);
    }
    for (const char* known : {".wav", ".aif", ".aiff", ".flac"})
        if (ends_with(lowered(name), known)) return name;
    return name + ".WAV";
}

template <typename T> T clamped(int value, int low, int high) { return T(std::clamp(value, low, high)); }

// A keygroup's amp envelope, as an SFZ region's: seconds and percent, a hold on its decay.
SfzEnvelope envelope_of(const KeygroupTags& tags) {
    auto stage = [&](const char* name, double plain) {
        auto value = number_at(tags, name);
        return value && *value >= 0 ? *value : plain;
    };
    SfzEnvelope made;
    made.attack = keygroup_seconds(stage("VolumeAttack", 0));
    made.decay = keygroup_seconds(stage("VolumeDecay", 0));
    made.sustain = std::clamp(stage("VolumeSustain", 1), 0.0, 1.0) * 100;
    made.release = keygroup_seconds(stage("VolumeRelease", 0.63));
    if (auto hold = number_at(tags, "VolumeHold"); hold && *hold > 0) *made.decay += keygroup_seconds(*hold);
    return made;
}
} // namespace

double keygroup_seconds(double value) { return 0.001 * std::pow(100000.0, std::clamp(value, 0.0, 1.0)); }

double keygroup_decibels(double value) { return (value - 0.353) * 18.0 / (1.0 - 0.353) - 12.0; }

SfzRead read_keygroup(const KeygroupProgram& program) {
    SfzRead read;
    std::vector<SfzEnvelope> envelopes;
    const int keygroups = int_at(program.tags, "KeygroupNumKeygroups").value_or(128);
    bool velocity_taken = false;
    for (const auto& instrument : program.instruments) {
        if (instrument.number > keygroups) continue; // past the program's count: left over from a longer one
        const auto& t = instrument.tags;
        if (flag_at(t, "Mute").value_or(false)) continue;
        std::vector<const KeygroupTags*> layers;
        for (const auto& layer : instrument.layers) {
            if (!flag_at(layer, "Active").value_or(true) || flag_at(layer, "Mute").value_or(false)) continue;
            if (sample_of(layer).empty()) continue;
            layers.push_back(&layer);
        }
        if (layers.empty()) continue;
        const int low = std::clamp(int_at(t, "LowNote").value_or(0), 0, 127);
        const int high = std::clamp(int_at(t, "HighNote").value_or(127), 0, 127);
        if (low > high) { read.left += unsigned(layers.size()); continue; }
        // TriggerMode: 0 one shot, 1 on the key's release, 2 (and older files' absence) on the press.
        const int trigger = int_at(t, "TriggerMode").value_or(-1);
        if (trigger == 1) { read.left += unsigned(layers.size()); read.left_by.insert("TriggerMode"); continue; }
        const bool one_shot = trigger == 0 || (trigger < 0 && flag_at(t, "OneShot").value_or(false));
        // ZonePlay: 0 cycles the layers a press at a time, 1 plays those its velocity reaches, 2 picks one at random.
        int play = int_at(t, "ZonePlay").value_or(1);
        if (play < 0 || play > 2) { read.ignored.insert("ZonePlay"); play = 1; }
        if (int_at(t, "FilterType").value_or(0) > 0) read.ignored.insert("FilterType");
        const bool no_keytrack = flag_at(t, "IgnoreBaseNote").value_or(false);
        const auto envelope = envelope_of(t);
        if (!velocity_taken) {
            velocity_taken = true;
            if (auto amount = number_at(t, "VelocitySensitivity"); amount && *amount > 0) read.velocity = std::clamp(*amount, 0.0, 1.0) * 100;
        }
        const size_t count = layers.size();
        for (size_t n = 0; n < count; ++n) {
            if (read.regions.size() >= max_zones) { ++read.left; continue; }
            const auto& l = *layers[n];
            SfzRegion made;
            made.sample = sample_of(l);
            auto& z = made.zone;
            z.low_key = uint8_t(low);
            z.high_key = uint8_t(high);
            z.root = clamped<uint8_t>(int_at(l, "RootNote").value_or(61) - 1, 0, 127);
            // velocity 0 is a release: the lowest that plays is 1
            z.low_velocity = clamped<uint8_t>(int_at(l, "VelStart").value_or(1), 1, 127);
            z.high_velocity = clamped<uint8_t>(int_at(l, "VelEnd").value_or(127), 1, 127);
            if (z.low_velocity > z.high_velocity) { ++read.left; continue; }
            if (count > 1 && play != 1) {
                // a cycle or a pick: the press plays one layer whatever its velocity
                z.low_velocity = 1;
                z.high_velocity = 127;
                if (play == 0) { z.sequence = uint32_t(n + 1); z.sequence_length = uint32_t(count); }
                else { z.low_random = float(double(n) / count); z.high_random = float(double(n + 1) / count); }
            }
            // Pitch is semitones; older files have it as TuneCoarse and TuneFine (cents) alone.
            double cents = 100.0 * int_at(l, "TuneCoarse").value_or(0) + int_at(l, "TuneFine").value_or(0);
            if (auto pitch = number_at(l, "Pitch"); pitch && *pitch != 0) cents = *pitch * 100;
            z.tune = clamped<int16_t>(int(std::lround(cents)), -32000, 32000);
            if (no_keytrack && !flag_at(l, "KeyTrack").value_or(false)) z.keytrack = 0;
            if (auto volume = number_at(l, "Volume")) z.gain = float(std::clamp(keygroup_decibels(*volume), -96.0, 6.0));
            if (auto pan = number_at(l, "Pan")) z.pan = float(std::clamp(*pan * 2 - 1, -1.0, 1.0));
            // Where it plays from and to: the Slice* tags, older files' Sample*.
            auto start = int_at(l, "SliceStart");
            if (!start) start = int_at(l, "SampleStart");
            auto end = int_at(l, "SliceEnd");
            if (!end) end = int_at(l, "SampleEnd");
            if (start && *start > 0) made.offset = *start;
            if (end && *end > 0) made.end = *end;
            z.one_shot = one_shot;
            made.loop_named = one_shot;
            if (!one_shot) {
                if (auto slice_loop = int_at(l, "SliceLoop")) {
                    made.loop_named = true;
                    if (*slice_loop > 0) {
                        if (*slice_loop != 1) read.ignored.insert("SliceLoop"); // reversed or back and forth: played forward
                        z.loop = ZoneLoop::On;
                        z.loop_start = uint32_t(std::max(int_at(l, "SliceLoopStart").value_or(0), 0));
                        z.loop_end = end && *end > 0 ? uint32_t(*end) : to_its_end;
                        if (z.loop_end <= z.loop_start) z.loop_end = to_its_end;
                    }
                } else if (auto loop = flag_at(l, "Loop")) {
                    made.loop_named = true;
                    if (*loop) {
                        z.loop = ZoneLoop::On;
                        auto from = int_at(l, "LoopStart").value_or(0), to = int_at(l, "LoopEnd").value_or(0);
                        if (to > from && from >= 0) { z.loop_start = uint32_t(from); z.loop_end = uint32_t(to); }
                    }
                }
                made.loop_asked = z.loop != ZoneLoop::Off;
            }
            read.regions.push_back(std::move(made));
            envelopes.push_back(envelope);
        }
    }
    sfz_share_envelopes(envelopes, read);
    return read;
}

} // namespace melodee
