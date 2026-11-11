// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/mpc_project.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/dsp.h"
#include "melodee/model/scales.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace melodee {
namespace fs = std::filesystem;
namespace {

constexpr ssize_t largest_document = 256 * 1024 * 1024;
constexpr unsigned mpc_ticks_per_tick = 960 / ticks_per_quarter; // the MPC counts 960 a quarter
constexpr int note_event = 3;                                    // an event's "type"
constexpr int mpc_pads = 128;

// The document's "data": unpacked, its header checked, parsed. Void on failure.
juce::var read_document(const std::vector<uint8_t>& file) {
    juce::MemoryBlock text;
    {
        juce::MemoryInputStream raw(file.data(), file.size(), false);
        juce::GZIPDecompressorInputStream inflated(&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
        inflated.readIntoMemoryBlock(text, largest_document);
    }
    const char* begin = static_cast<const char*>(text.getData());
    const char* end = begin + text.getSize();
    std::array<std::string, 5> header;
    const char* at = begin;
    for (auto& line : header) {
        const char* stop = std::find(at, end, '\n');
        if (stop == end) return {};
        line.assign(at, stop);
        at = stop + 1;
    }
    if (header[0] != "ACVS" || header[2] != "SerialisableProjectData" || header[3] != "json") return {};
    return juce::JSON::parse(juce::String::fromUTF8(at, int(end - at)))["data"];
}

double number(const juce::var& object, const char* key, double fallback = 0) {
    const auto& value = object[key];
    return value.isVoid() ? fallback : double(value);
}
int integer(const juce::var& object, const char* key, int fallback = 0) { return int(number(object, key, fallback)); }
bool flag(const juce::var& object, const char* key, bool fallback = false) {
    const auto& value = object[key];
    return value.isVoid() ? fallback : bool(value);
}
uint8_t percent(double value) { return uint8_t(std::clamp(std::lround(value * 100), 0l, 100l)); }
uint16_t tenths(double bpm) { return uint16_t(std::clamp(std::lround(bpm * 10), 300l, 3000l)); }

// A volume in the file (1.0 is +6 dB, 0.7079 is 0 dB, squared between) as Melodee's 0..127 (sampler.h).
uint8_t level_of(double value) {
    if (value <= 0) return 0;
    double db = 40 * std::log10(value) + level_top_db;
    if (db >= 0) return uint8_t(std::clamp(std::lround(100 + db / level_top_db * 27), 100l, 127l));
    return uint8_t(std::clamp(std::lround(100 * std::pow(10.0, db / 37)), 1l, 100l));
}
int8_t pan_of(double value) { return int8_t(std::clamp(std::lround((value - 0.5) * 100), -50l, 50l)); }

// The stock app's filter table, thirty types of which its Sample Mode offers these eight.
PadFilter filter_of(int type) {
    switch (type) {
    case 2: return PadFilter::Lpf2;
    case 3: return PadFilter::Lpf4;
    case 7: return PadFilter::Hpf2;
    case 8: return PadFilter::Hpf4;
    case 11: return PadFilter::Bpf2;
    case 12: return PadFilter::Bpf4;
    case 29: return PadFilter::Classic; // "MPC3000 LPF"; every pad starts on it, wide open
    default: return PadFilter::Off;
    }
}

// The sample rate of a WAV file; 0 when it cannot tell.
unsigned wav_rate(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    char head[12];
    if (!file.read(head, sizeof(head)) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) return 0;
    char chunk[8];
    while (file.read(chunk, sizeof(chunk))) {
        uint32_t size = juce::ByteOrder::littleEndianInt(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            char format[8];
            return file.read(format, sizeof(format)) ? juce::ByteOrder::littleEndianInt(format + 4) : 0;
        }
        file.seekg(std::streamoff(size) + (size & 1), std::ios::cur);
    }
    return 0;
}

// The project's sample slots, each file once.
class Samples {
public:
    Samples(Project& p, fs::path f) : project(p), folder(std::move(f)) {}
    // A slot for the file, -1 when it does not fit; `missing` is set when it is not there.
    int slot(const juce::String& file, bool& missing) {
        auto name = fs::path(file.toStdString()).filename(); // the name alone: never out of the data folder
        auto path = (folder / name).string();
        std::error_code error;
        missing = !fs::exists(path, error);
        if (path.size() >= sizeof(SampleRef::path)) return -1;
        for (unsigned n = 0; n < sample_slots; ++n) {
            auto& ref = project.samples[n];
            if (ref.used() && path == ref.path) return int(n);
            if (ref.used()) continue;
            std::strncpy(ref.path, path.c_str(), sizeof(ref.path) - 1);
            std::strncpy(ref.name, name.stem().string().c_str(), sizeof(ref.name) - 1);
            auto rate = wav_rate(path);
            rates[n] = rate ? double(dsp::model_rate) / rate : 1.0;
            return int(n);
        }
        return -1;
    }
    double to_model(unsigned slot) const { return rates[slot % sample_slots]; }
private:
    Project& project;
    fs::path folder;
    std::array<double, sample_slots> rates {};
};

const juce::var nothing; // what the lookups below hand back when there is no such thing

const juce::var& sounding_layer(const juce::var& instrument) {
    const auto* layers = instrument["layersv"].getArray();
    if (layers)
        for (const auto& layer : *layers)
            if (flag(layer, "active", true) && layer["sampleFile"].toString().isNotEmpty()) return layer;
    return nothing;
}

void read_pad(const juce::var& instrument, const juce::var& layer, unsigned slot, double to_model, PadSound& pad) {
    const auto frame = [&](double at) { return uint32_t(std::max(std::lround(at * to_model), 0l)); };
    const auto& slice = layer["sliceInfo"];
    const auto& synth = instrument["synthSection"];
    const auto& amp = synth["ampEnvelope"];
    const auto& mix = instrument["mixable"];
    pad.sample = uint16_t(slot);
    pad.start = frame(number(slice, "Start"));
    pad.end = frame(number(slice, "End"));
    pad.pitch = int8_t(std::clamp(integer(instrument, "coarseTune") + integer(layer, "coarseTune"), -48, 48));
    pad.fine = int8_t(std::clamp(integer(instrument, "fineTune") + integer(layer, "fineTune"), -99, 99));
    pad.level = level_of(number(mix, "volume", 0.7079));
    pad.pan = pan_of(number(mix, "pan", 0.5));
    pad.play = flag(amp["OneShot"], "value0", true) ? PadPlay::OneShot : PadPlay::Gate;
    pad.reverse = integer(layer, "direction") == 1;
    pad.group = uint8_t(std::clamp(integer(instrument, "whichMuteGroup"), 0, 16));
    pad.mono = flag(instrument, "monophonic");
    pad.attack = percent(number(amp["Attack"], "value0"));
    pad.decay = percent(number(amp["Decay"], "value0"));
    pad.sustain = percent(number(amp["Sustain"], "value0", 1));
    pad.release = percent(number(amp["Release"], "value0"));
    pad.decay_from_start = !flag(amp["DecayFromEnd"], "value0", true);
    pad.velocity_sense = percent(number(synth, "velocitySensitivity", 1));
    pad.loop = (flag(layer, "layerLoopModeOverridesSliceLoopMode", true) ? integer(layer, "loopMode") : integer(slice, "LoopMode")) != 0;
    pad.loop_lock = flag(instrument, "loopLock", true);
    pad.loop_start = frame(number(slice, "LoopStart"));
    const auto& filter = synth["filterData"]["value0"];
    pad.cutoff = percent(number(filter, "filterCutoff", 1));
    pad.resonance = percent(number(filter, "filterResonance"));
    pad.filter_amount = percent(number(filter, "filterEnvelopeAmount"));
    pad.filter = filter_of(integer(filter, "filterType", 29));
    if (pad.filter == PadFilter::Classic && pad.cutoff == 100 && !pad.resonance && !pad.filter_amount) pad.filter = PadFilter::Off; // wide open: nothing to hear
    if (pad.filter_amount) {
        const auto& env = synth["filterEnvelope"];
        pad.filter_attack = percent(number(env["Attack"], "value0"));
        pad.filter_decay = percent(number(env["Decay"], "value0"));
        pad.filter_sustain = percent(number(env["Sustain"], "value0"));
        pad.filter_release = percent(number(env["Release"], "value0"));
    }
    if (flag(instrument, "warpEnable")) {
        pad.warp = flag(instrument, "bpmLock", true) ? warp_seq : uint8_t(std::clamp(integer(instrument, "stretchPercentage", 100), int(warp_min), int(warp_max)));
        pad.own_tempo = tenths(number(instrument, "tempo", 120));
    }
}

// A track's clip in a sequence, by the track's name; void when it has none.
const juce::var& clip_of(const juce::var& sequence, const juce::String& track) {
    const auto* maps = sequence["trackClipMaps"].getArray();
    if (maps)
        for (const auto& map : *maps)
            if (const auto* clips = map.getArray())
                for (const auto& clip : *clips)
                    if (clip["key"].toString() == track) return clip["value"];
    return nothing;
}

bool has_notes(const juce::var& clip) {
    if (const auto* events = clip["eventList"]["events"].getArray())
        for (const auto& event : *events)
            if (integer(event, "type") == note_event) return true;
    return false;
}

unsigned read_notes(const juce::var& clip, const std::array<int, 128>& pad_of_note, Pattern& pattern) {
    unsigned added = 0;
    const auto* events = clip["eventList"]["events"].getArray();
    if (!events) return 0;
    for (const auto& event : *events) {
        if (integer(event, "type") != note_event || flag(event, "muted")) continue;
        const auto& note = event["note"];
        int key = integer(note, "note", -1);
        if (key < 0 || key > 127 || pad_of_note[size_t(key)] < 0) continue;
        uint32_t tick = uint32_t(std::lround(number(event, "time") / mpc_ticks_per_tick));
        if (tick >= pattern.length) continue;
        Event e;
        e.tick = tick;
        e.length = uint16_t(std::clamp(std::lround(number(note, "length") / mpc_ticks_per_tick), 1l, 65535l));
        e.key = uint8_t(sampler_first_note + unsigned(pad_of_note[size_t(key)]));
        e.velocity = uint8_t(std::clamp(std::lround(number(note, "velocity", 1) * 127), 1l, 127l));
        e.chance = uint8_t(std::clamp(integer(note, "probability", 100), 0, 100));
        added += pattern.add(e);
    }
    return added;
}

} // namespace

bool is_mpc_project(const std::string& path) { return fs::path(path).extension() == ".xpj"; }

std::string import_mpc_project(const std::vector<uint8_t>& file, const std::string& path, Project& project, PatternBank& patterns, MpcImport& report) {
    const auto data = read_document(file);
    const auto* tracks = data["tracks"].getArray();
    if (!tracks) return "Not an MPC Sample project";

    project = Project {};
    patterns = PatternBank {};
    const fs::path where(path);
    std::snprintf(project.name, sizeof(project.name), "%s", where.stem().string().c_str());
    project.root = 0;
    project.scale = chromatic_scale;

    // Sequences by their number (the file's order is not): the ones Melodee has patterns for.
    std::map<unsigned, const juce::var*> sequences;
    if (const auto* list = data["sequences"].getArray())
        for (const auto& entry : *list) {
            unsigned key = unsigned(integer(entry, "key"));
            if (key < pattern_count) sequences[key] = &entry["value"];
            else ++report.over_limit;
        }

    Samples samples(project, where.parent_path() / (where.stem().string() + "_[ProjectData]"));
    unsigned used = 0;
    for (const auto& source : *tracks) {
        const auto& program = source["program"];
        const auto* instruments = program["drum"]["instruments"].getArray();
        if (!instruments) continue; // submixes and outputs are tracks in the file too
        const auto name = source["name"].toString();
        bool wanted = false;
        for (const auto& instrument : *instruments) wanted = wanted || !sounding_layer(instrument).isVoid();
        for (const auto& [n, sequence] : sequences) wanted = wanted || has_notes(clip_of(*sequence, name));
        if (!wanted) continue;
        if (used == track_count) { ++report.over_limit; continue; }

        const unsigned index = used++;
        auto& track = project.tracks[index];
        load_sound(track, EngineKind::Sampler, 0);
        track.set_name(name.toRawUTF8());
        const auto& mix = program["mixable"];
        track.mix.level = level_of(number(mix, "volume", 0.7079));
        track.mix.pan = pan_of(number(mix, "pan", 0.5));
        track.mix.mute = flag(mix, "mute") || flag(source, "mute");
        track.mix.solo = flag(mix, "solo");

        for (int n = 0; n < std::min(instruments->size(), mpc_pads); ++n) {
            const auto& instrument = instruments->getReference(n);
            const auto& layer = sounding_layer(instrument);
            if (layer.isVoid()) continue;
            bool missing = false;
            int slot = samples.slot(layer["sampleFile"].toString(), missing);
            if (slot < 0) { ++report.over_limit; continue; }
            report.missing += missing;
            read_pad(instrument, layer, unsigned(slot), samples.to_model(unsigned(slot)), track.pads[size_t(n)]);
            ++report.pads;
        }

        std::array<int, 128> pad_of_note;
        pad_of_note.fill(-1);
        const auto& pad_notes = program["padNoteMap"]["noteForPad"];
        for (int pad = mpc_pads - 1; pad >= 0; --pad) { // the lowest pad of a note wins
            auto key = juce::String("value") + juce::String(pad);
            int note = pad_notes[key.toRawUTF8()].isVoid() ? 36 + pad : int(pad_notes[key.toRawUTF8()]);
            if (note >= 0 && note < 128) pad_of_note[size_t(note)] = pad;
        }
        for (const auto& [n, sequence] : sequences) {
            auto& pattern = patterns.at[index][n];
            pattern.length = uint32_t(std::clamp<int64_t>(int64_t(number(*sequence, "lengthPulses", 3840) / mpc_ticks_per_tick), 1, project.max_length()));
            report.notes += read_notes(clip_of(*sequence, name), pad_of_note, pattern);
        }
    }
    if (!used) return "Nothing to import";
    report.tracks = used;
    report.sequences = unsigned(sequences.size());

    // The tempo: the project's own when it is on, else each sequence's.
    const bool master = flag(data, "masterTempoEnabled");
    const unsigned current = unsigned(integer(data, "currentSequence"));
    project.seq_tempo = !master && !sequences.empty();
    project.tempo = tenths(number(data, "masterTempo", 120));
    for (const auto& [n, sequence] : sequences) project.pattern_tempo[n] = tenths(number(*sequence, "bpm", 120));
    if (!master && sequences.count(current)) project.tempo = project.pattern_tempo[current];
    if (sequences.count(current))
        for (unsigned t = 0; t < used; ++t) project.tracks[t].pattern = uint8_t(current);

    // One time signature for every pattern, as in Melodee: the first sequence's.
    if (!sequences.empty()) {
        const auto* signatures = (*sequences.begin()->second)["timeSignatureTrack"]["timeSignatures"].getArray();
        if (signatures && !signatures->isEmpty()) {
            const auto& first = signatures->getReference(0);
            int beat = integer(first, "beatLength", 960), unit = beat > 0 ? 3840 / beat : 4, beats = integer(first, "beatsPerBar", 4);
            for (unsigned n = 0; n < time_signature_count; ++n)
                if (time_signatures[n].beats == beats && time_signatures[n].unit == unit) project.signature = uint8_t(n);
        }
    }
    return {};
}

} // namespace melodee
