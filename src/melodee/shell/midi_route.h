// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/midi_config.h"
#include "melodee/model/project.h"
#include <array>
#include <cstdint>
#include <string>

// Where MIDI goes in Melodee (parity phase 9), as plain decisions the MIDI
// thread acts on (shell/midi_io.cpp) and the host test checks.
namespace melodee {

// Which port a device belongs to: External is the 1/8" jacks (the UART's
// SerialMIDI card), USB every other MIDI device (USB-C's gadget, USB host).
// The control surface is never one.
bool port_is(MidiPort port, const std::string& driver, bool surface);
// Whether a message for `dest` goes out on a device: Config is MIDI Config's
// port; a MIDI track's External the jacks, USB-C the gadget (f_midi, driver
// "MIDI Gadget") alone, All USB every USB device. Never the surface.
bool dest_is(MidiDest dest, MidiPort config, const std::string& driver, bool surface);

// The tracks a note on `channel` (1..16) plays: every track that has it as
// its own channel; when none does, the selected track if MIDI Config's In
// channel is All or this one. A bit a track.
uint16_t note_tracks(const MidiConfig& config, const std::array<uint8_t, track_count>& own, unsigned selected, unsigned channel);
// The same for a message from one port: a track's own channel counts only from a port it listens on (`heard`, a bit a
// track: track_heard()); the selected track only from MIDI Config's port (`config_port`).
uint16_t note_tracks(const MidiConfig& config, const std::array<uint8_t, track_count>& own, uint16_t heard, unsigned selected,
                     unsigned channel, bool config_port);
// Whether a track with its own channel listens on a device (Track::midi_in). Never the surface.
bool track_hears(TrackMidiIn in, const std::string& driver, bool surface);

// Remote (MIDI Config's Remote rows): what a CC does. Track: select `index`; Pattern: launch `index` on the selected
// track; Scene: launch scene `index`. None: not a remote CC (it goes on to the tracks), or a value past the end
// (Ignored: taken, nothing done).
struct RemoteJob {
    enum class Kind : uint8_t { None, Ignored, Track, Pattern, Scene } kind = Kind::None;
    unsigned index = 0;
};
RemoteJob remote_cc(const MidiConfig& config, unsigned channel, unsigned cc, unsigned value);

// The key a MIDI note plays on a track: a synth the note itself; a kit the
// pad it names (36 = A01, as Melodee's own pad keys and its MIDI out), -1
// below the first pad; a 909 its drums from 36 as its pads.
int note_key(const Track& track, int note, bool every_key = false); // every_key: a kit pad on the keys under 16 Levels

// 16 Levels on a kit (no arp): the whole keyboard plays the 16 Levels pad, key 60 its root. An instrument plays at its
// real pitch (the pad's Semi undone); a single sample is a C, key 60 the pad as its pads play it. The pad note
// (pad_note()) for `key`, or -1 when the keyboard plays the kit's pads.
int levels_key(const Track& track, int key, bool instrument);

// The channel a track's notes go out on (its own, else MIDI Config's Out) and
// when (PadMidiOut), for Command::MidiTrack.
inline unsigned out_channel_of(const MidiConfig& config, const Track& track) { return track.midi_channel ? track.midi_channel : config.out_channel; }

// MIDI clock in: the tempo from the last pulses' kernel stamps. A pause of a
// quarter second or more starts it over.
class ClockFollower {
public:
    void pulse(uint64_t ns);
    void reset() { count = 0; }
    unsigned tempo_tenths() const; // 0 until thirteen pulses (half a beat)
    unsigned jitter_us() const;    // how unevenly they came: the intervals' standard deviation over the last two beats
private:
    static constexpr unsigned span = 48; // two quarters of pulses
    std::array<uint64_t, span + 1> stamps {};
    unsigned count = 0;
};

} // namespace melodee
