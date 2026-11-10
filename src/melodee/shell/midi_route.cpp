// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/midi_route.h"
#include "melodee/model/sampler.h"
#include <algorithm>
#include <cmath>

namespace melodee {

bool port_is(MidiPort port, const std::string& driver, bool surface) {
    if (surface) return false;
    bool external = driver == "SerialMIDI";
    return port == MidiPort::External ? external : !external;
}

bool dest_is(MidiDest dest, MidiPort config, const std::string& driver, bool surface) {
    if (surface) return false;
    switch (dest) {
    case MidiDest::External: return driver == "SerialMIDI";
    case MidiDest::UsbC: return driver == "MIDI Gadget";
    case MidiDest::AllUsb: return driver != "SerialMIDI";
    default: return port_is(config, driver, surface);
    }
}

uint16_t note_tracks(const MidiConfig& config, const std::array<uint8_t, track_count>& own, unsigned selected, unsigned channel) {
    uint16_t tracks = 0;
    for (unsigned t = 0; t < track_count; ++t)
        if (own[t] && own[t] == channel) tracks = uint16_t(tracks | (1u << t));
    if (tracks) return tracks; // a channel a track has taken is that track's
    if (config.in_channel == 0 || config.in_channel == channel) tracks = uint16_t(1u << (selected % track_count));
    return tracks;
}

int note_key(const Track& track, int note) {
    if (note < 0 || note > 127) return -1;
    if (track.engine == EngineKind::Sampler) return note >= int(sampler_first_note) ? note : -1;
    if (track.engine == EngineKind::Kit909) return note >= 36 && note < 47 ? note : -1;
    return note;
}

void ClockFollower::pulse(uint64_t ns) {
    if (count && ns - stamps[(count - 1) % stamps.size()] > 250000000ull) count = 0; // a pause: start over
    stamps[count % stamps.size()] = ns;
    ++count;
}

unsigned ClockFollower::tempo_tenths() const {
    if (count < 13) return 0; // a dozen intervals first: USB brings pulses in bunches
    unsigned n = std::min<unsigned>(count, unsigned(stamps.size()));
    uint64_t first = stamps[(count - n) % stamps.size()], last = stamps[(count - 1) % stamps.size()];
    if (last <= first) return 0;
    double interval = double(last - first) / double(n - 1); // ns a pulse
    double bpm = 60e9 / (interval * 24);
    return unsigned(std::clamp(bpm * 10 + 0.5, 300.0, 3000.0));
}

unsigned ClockFollower::jitter_us() const {
    if (count < 13) return 0;
    unsigned n = std::min<unsigned>(count, unsigned(stamps.size()));
    double sum = 0, sum2 = 0;
    for (unsigned k = 1; k < n; ++k) {
        double d = double(stamps[(count - k) % stamps.size()] - stamps[(count - k - 1) % stamps.size()]);
        sum += d; sum2 += d * d;
    }
    double mean = sum / (n - 1);
    return unsigned(std::sqrt(std::max(0.0, sum2 / (n - 1) - mean * mean)) / 1000 + 0.5);
}

} // namespace melodee
