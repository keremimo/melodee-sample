// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MPC programs as zones and pads: a keygroup program's keygroups and layers,
// and a drum program's pads. The tags are read as the MPC's own files have
// them (a drum .xpm and the same kit's .xpj agree number for number): RootNote
// is one past the MIDI note, a layer's Volume is its gain (1 is 0 dB), a pad's
// Volume is on the MPC's squared curve (0.7079 is 0 dB), Pan 0..1 is left to
// right, an envelope's 0..1 is 1 ms to 100 s, and the sample is its SampleName
// with ".WAV", beside the program.
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
uint8_t percent(double value) { return uint8_t(std::clamp(std::lround(value * 100), 0l, 100l)); }

// A layer's tune in cents: Pitch is semitones; older files have TuneCoarse and TuneFine (cents) alone.
double layer_cents(const KeygroupTags& l) {
    double cents = 100.0 * int_at(l, "TuneCoarse").value_or(0) + int_at(l, "TuneFine").value_or(0);
    if (auto pitch = number_at(l, "Pitch"); pitch && *pitch != 0) cents = *pitch * 100;
    return cents;
}

// One keygroup's layers (a drum pad's: `drum`) as zones over low..high.
void add_layers(const KeygroupInstrument& instrument, int low, int high, bool drum, SfzRead& read,
                std::vector<SfzEnvelope>& envelopes, bool& velocity_taken, double program_cents = 0, double program_db = 0) {
    const auto& t = instrument.tags;
    const auto layers = keygroup_layers(instrument);
    if (layers.empty()) return;
    if (low > high) { read.left += unsigned(layers.size()); return; }
    // TriggerMode: 0 one shot, 1 on the key's release (roadmap P3: a release sample), 2 (and older files' absence) on the press.
    const int trigger = int_at(t, "TriggerMode").value_or(-1);
    const bool on_release = trigger == 1;
    const bool one_shot = trigger == 0 || on_release || (trigger < 0 && flag_at(t, "OneShot").value_or(false));
    // ZonePlay: 0 cycles the layers a press at a time, 1 plays those its velocity reaches, 2 picks one at random.
    int play = int_at(t, "ZonePlay").value_or(1);
    if (play < 0 || play > 2) { read.ignored.insert("ZonePlay"); play = 1; }
    // A drum pad's filter, volume and tune are the pad's own (keygroup_pad()); a keygroup's go on its zones.
    double instrument_cents = program_cents, instrument_db = program_db;
    if (!drum) {
        if (int_at(t, "FilterType").value_or(0) > 0) read.ignored.insert("FilterType");
        instrument_cents += 100.0 * int_at(t, "TuneCoarse").value_or(0) + int_at(t, "TuneFine").value_or(0);
        if (auto volume = number_at(t, "Volume"); volume && *volume > 0) instrument_db += 40 * std::log10(*volume) + 6;
    }
    const bool no_keytrack = !drum && flag_at(t, "IgnoreBaseNote").value_or(false);
    const auto envelope = envelope_of(t);
    if (!velocity_taken) {
        velocity_taken = true;
        // 0 is none at all (an organ's): every press full
        if (auto amount = number_at(t, "VelocitySensitivity")) read.velocity = std::clamp(*amount, 0.0, 1.0) * 100;
    }
    const size_t count = layers.size();
    for (size_t n = 0; n < count; ++n) {
        if (read.regions.size() >= max_zones) { ++read.left; continue; }
        const auto& l = *layers[n];
        SfzRegion made;
        made.sample = keygroup_sample(l);
        auto& z = made.zone;
        z.low_key = uint8_t(low);
        z.high_key = uint8_t(high);
        // RootNote is one past the MIDI note; a drum pad's layers sit at the pad's note.
        z.root = drum ? 60 : clamped<uint8_t>(int_at(l, "RootNote").value_or(61) - 1, 0, 127);
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
        z.tune = clamped<int16_t>(int(std::lround(layer_cents(l) + instrument_cents)), -32000, 32000);
        if (no_keytrack && !flag_at(l, "KeyTrack").value_or(false)) z.keytrack = 0;
        double db = instrument_db;
        if (auto volume = number_at(l, "Volume")) db += keygroup_decibels(*volume);
        z.gain = float(std::clamp(db, -96.0, 12.0));
        if (auto pan = number_at(l, "Pan")) z.pan = float(std::clamp(*pan * 2 - 1, -1.0, 1.0));
        // Where it plays from and to: the Slice* tags, older files' Sample*.
        auto start = int_at(l, "SliceStart");
        if (!start) start = int_at(l, "SampleStart");
        auto end = int_at(l, "SliceEnd");
        if (!end) end = int_at(l, "SampleEnd");
        if (start && *start > 0) made.offset = *start;
        if (end && *end > 0) made.end = *end;
        z.one_shot = one_shot;
        z.on_release = on_release;
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
} // namespace

std::vector<const KeygroupTags*> keygroup_layers(const KeygroupInstrument& instrument) {
    std::vector<const KeygroupTags*> layers;
    if (flag_at(instrument.tags, "Mute").value_or(false)) return layers;
    for (const auto& layer : instrument.layers) {
        if (!flag_at(layer, "Active").value_or(true) || flag_at(layer, "Mute").value_or(false)) continue;
        if (sample_of(layer).empty()) continue;
        layers.push_back(&layer);
    }
    return layers;
}

std::string keygroup_sample(const KeygroupTags& layer) { return sample_of(layer); }

double keygroup_seconds(double value) { return 0.001 * std::pow(100000.0, std::clamp(value, 0.0, 1.0)); }

// The MPC's own projects write a layer's volume as {gainCoefficient, controlValue, law}, the two
// alike for its layers: the number is the gain itself.
double keygroup_decibels(double value) { return value > 0 ? 20 * std::log10(value) : -96.0; }

// Volume on the MPC's curve: squared, 1.0 at +6 dB, 0.7079 at 0 dB (sampler.h's levels).
uint8_t mpc_level(double value) {
    if (value <= 0) return 0;
    double db = 40 * std::log10(value) + level_top_db;
    if (db >= 0) return uint8_t(std::clamp(std::lround(100 + db / level_top_db * 27), 100l, 127l));
    return uint8_t(std::clamp(std::lround(100 * std::pow(10.0, db / 37)), 1l, 100l));
}

int8_t mpc_pan(double value) { return int8_t(std::clamp(std::lround((value - 0.5) * 100), -50l, 50l)); }

// The MPC's filter table, thirty types, as the pad's: a one-pole as the pad's two-pole kind.
PadFilter mpc_filter(int type) {
    switch (type) {
    case 1: case 2: return PadFilter::Lpf2;
    case 3: case 4: case 5: return PadFilter::Lpf4;
    case 6: case 7: return PadFilter::Hpf2;
    case 8: case 9: case 10: return PadFilter::Hpf4;
    case 11: return PadFilter::Bpf2;
    case 12: case 13: case 14: return PadFilter::Bpf4;
    case 29: return PadFilter::Classic; // "MPC3000 LPF"; every pad starts on it, wide open
    default: return PadFilter::Off;
    }
}

SfzRead read_keygroup(const KeygroupProgram& program) {
    SfzRead read;
    std::vector<SfzEnvelope> envelopes;
    const auto& p = program.tags;
    const int keygroups = int_at(p, "KeygroupNumKeygroups").value_or(128);
    // The program's own tune and volume, over every keygroup's: TuneCoarse and TuneFine (an .xpm),
    // transpose in semitones (an MPC 3 program's); Volume on the MPC's curve, 0.7079 at 0 dB.
    double cents = 100.0 * (int_at(p, "TuneCoarse").value_or(0) + int_at(p, "KeygroupTranspose").value_or(0)) + int_at(p, "TuneFine").value_or(0);
    double db = 0;
    if (auto volume = number_at(p, "Volume"); volume && *volume > 0) db = 40 * std::log10(*volume) + 6;
    if (auto transpose = number_at(p, "KeygroupMasterTranspose"); transpose && std::fabs(*transpose - 0.5) > 1e-6) read.ignored.insert("KeygroupMasterTranspose");
    bool velocity_taken = false;
    for (const auto& instrument : program.instruments) {
        if (instrument.number > keygroups) continue; // past the program's count: left over from a longer one
        const int low = std::clamp(int_at(instrument.tags, "LowNote").value_or(0), 0, 127);
        const int high = std::clamp(int_at(instrument.tags, "HighNote").value_or(127), 0, 127);
        add_layers(instrument, low, high, false, read, envelopes, velocity_taken, cents, db);
    }
    sfz_share_envelopes(envelopes, read);
    return read;
}

SfzRead read_drum_pad(const KeygroupInstrument& pad) {
    SfzRead read;
    std::vector<SfzEnvelope> envelopes;
    bool velocity_taken = true; // the pad's own (keygroup_pad())
    add_layers(pad, 0, 127, true, read, envelopes, velocity_taken);
    return read; // the envelope is the pad's: no stage on the zones
}

void keygroup_pad(const KeygroupInstrument& instrument, double to_model, PadSound& pad) {
    const auto& t = instrument.tags;
    const auto layers = keygroup_layers(instrument);
    const auto frame = [&](int at) { return uint32_t(std::max(std::lround(at * to_model), 0l)); };
    double cents = 100.0 * int_at(t, "TuneCoarse").value_or(0) + int_at(t, "TuneFine").value_or(0);
    if (layers.size() == 1) cents += layer_cents(*layers[0]);
    const int whole = int(std::lround(cents / 100));
    pad.pitch = int8_t(std::clamp(whole, -48, 48));
    pad.fine = int8_t(std::clamp(int(std::lround(cents - 100.0 * whole)), -99, 99));
    pad.level = mpc_level(number_at(t, "Volume").value_or(0.7079));
    pad.pan = mpc_pan(number_at(t, "Pan").value_or(0.5));
    pad.play = int_at(t, "TriggerMode").value_or(flag_at(t, "OneShot").value_or(true) ? 0 : 2) == 0 ? PadPlay::OneShot : PadPlay::Gate;
    pad.mono = flag_at(t, "Mono").value_or(false);
    pad.group = uint8_t(std::clamp(int_at(t, "MuteGroup").value_or(0), 0, 16));
    pad.attack = percent(number_at(t, "VolumeAttack").value_or(0));
    pad.decay = percent(number_at(t, "VolumeDecay").value_or(0.05));
    pad.sustain = percent(number_at(t, "VolumeSustain").value_or(1));
    pad.release = percent(number_at(t, "VolumeRelease").value_or(0));
    pad.decay_from_start = !flag_at(t, "VolumeDecayType").value_or(true);
    pad.velocity_sense = percent(number_at(t, "VelocitySensitivity").value_or(1));
    pad.cutoff = percent(number_at(t, "Cutoff").value_or(1));
    pad.resonance = percent(number_at(t, "Resonance").value_or(0));
    pad.filter_amount = percent(number_at(t, "FilterEnvAmt").value_or(0));
    pad.filter = mpc_filter(int_at(t, "FilterType").value_or(0));
    for (unsigned b = 0; b < pad.sends.size(); ++b) // the MPC's per-pad Send 1-4 into Melodee's buses A-D
        pad.sends[b] = percent(number_at(t, ("Send" + std::to_string(b + 1)).c_str()).value_or(0));
    if ((pad.filter == PadFilter::Classic || pad.filter == PadFilter::Lpf2 || pad.filter == PadFilter::Lpf4) && pad.cutoff == 100 &&
        !pad.resonance && !pad.filter_amount) pad.filter = PadFilter::Off; // wide open: nothing to hear
    if (pad.filter_amount) {
        pad.filter_attack = percent(number_at(t, "FilterAttack").value_or(0));
        pad.filter_decay = percent(number_at(t, "FilterDecay").value_or(0));
        pad.filter_sustain = percent(number_at(t, "FilterSustain").value_or(0));
        pad.filter_release = percent(number_at(t, "FilterRelease").value_or(0));
    }
    if (layers.size() != 1) return;
    // One layer: the pad plays its file, from where the layer says.
    const auto& l = *layers[0];
    auto start = int_at(l, "SliceStart");
    if (!start) start = int_at(l, "SampleStart");
    auto end = int_at(l, "SliceEnd");
    if (!end) end = int_at(l, "SampleEnd");
    pad.start = start && *start > 0 ? frame(*start) : 0;
    pad.end = end && *end > 0 ? frame(*end) : 0;
    pad.reverse = int_at(l, "Direction").value_or(0) == 1;
    if (int_at(l, "SliceLoop").value_or(0) > 0) {
        pad.loop = true;
        pad.loop_lock = false;
        pad.loop_start = frame(std::max(int_at(l, "SliceLoopStart").value_or(0), 0));
    }
    if (auto volume = number_at(l, "Volume"); volume && *volume > 0 && *volume != 1)
        pad.gain = int16_t(std::clamp(std::lround(keygroup_decibels(*volume) * 100), -9600l, 1200l));
}

bool keygroup_pad_layers(const KeygroupInstrument& instrument, const std::array<uint16_t, pad_layers>& slots,
                         const std::array<double, pad_layers>& to_model, PadSound& pad) {
    const auto& t = instrument.tags;
    const auto layers = keygroup_layers(instrument);
    if (layers.size() < 2 || layers.size() > pad_layers || int_at(t, "TriggerMode").value_or(-1) == 1) return false;
    const int play = int_at(t, "ZonePlay").value_or(1);
    struct Read { uint32_t start = 0, end = 0; double cents = 0, db = 0; int pan = 0; uint8_t low = 1, high = 127; };
    std::array<Read, pad_layers> read {};
    for (size_t n = 0; n < layers.size(); ++n) {
        const auto& l = *layers[n];
        const auto frame = [&](int at) { return uint32_t(std::max(std::lround(at * to_model[n]), 0l)); };
        auto start = int_at(l, "SliceStart");
        if (!start) start = int_at(l, "SampleStart");
        auto end = int_at(l, "SliceEnd");
        if (!end) end = int_at(l, "SampleEnd");
        auto& r = read[n];
        r.start = start && *start > 0 ? frame(*start) : 0;
        r.end = end && *end > 0 ? frame(*end) : 0;
        r.cents = layer_cents(l);
        r.db = keygroup_decibels(number_at(l, "Volume").value_or(1));
        r.pan = mpc_pan(number_at(l, "Pan").value_or(0.5));
        r.low = clamped<uint8_t>(int_at(l, "VelStart").value_or(1), 1, 127);
        r.high = clamped<uint8_t>(int_at(l, "VelEnd").value_or(127), 1, 127);
        if (r.low > r.high) std::swap(r.low, r.high);
    }
    auto split = [](double cents, int8_t& semis, int8_t& fine) {
        const int whole = int(std::lround(cents / 100));
        semis = int8_t(std::clamp(whole, -48, 48));
        fine = int8_t(std::clamp(int(std::lround(cents - 100.0 * whole)), -99, 99));
    };
    // Layer 1 is the pad: its tune, volume and pan over the pad's own.
    const auto& first = *layers[0];
    split(pad.pitch * 100.0 + pad.fine + read[0].cents, pad.pitch, pad.fine);
    pad.gain = int16_t(std::clamp(std::lround(read[0].db * 100), -9600l, 1200l));
    pad.pan = int8_t(std::clamp(pad.pan + read[0].pan, -50, 50));
    pad.sample = slots[0];
    pad.start = read[0].start; pad.end = read[0].end;
    pad.reverse = int_at(first, "Direction").value_or(0) == 1;
    if (int_at(first, "SliceLoop").value_or(0) > 0) {
        pad.loop = true;
        pad.loop_lock = false;
        pad.loop_start = uint32_t(std::max(std::lround(std::max(int_at(first, "SliceLoopStart").value_or(0), 0) * to_model[0]), 0l));
    }
    pad.layer_play = play == 0 ? LayerPlay::Cycle : play == 2 ? LayerPlay::Random : LayerPlay::Velocity;
    pad.low_velocity = read[0].low; pad.high_velocity = read[0].high;
    pad.layers = {};
    for (size_t n = 1; n < layers.size(); ++n) {
        auto& l = pad.layers[n - 1];
        l.sample = slots[n];
        l.start = read[n].start; l.end = read[n].end;
        split(read[n].cents - read[0].cents, l.pitch, l.fine);
        l.gain = int16_t(std::clamp(std::lround((read[n].db - read[0].db) * 100), -9600l, 2400l));
        l.pan = int8_t(std::clamp(read[n].pan - read[0].pan, -50, 50));
        l.low = read[n].low; l.high = read[n].high;
    }
    return true;
}

} // namespace melodee
