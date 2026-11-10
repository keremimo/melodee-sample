// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine.h"
#include "melodee/model/midi_config.h"

// The MIDI engine (docs/MIDI_TRACKS.md, phase M1): a track that plays an
// external instrument. It makes no sound; the engine host sends what reaches
// it (pads, patterns, the arp) as MIDI on the track's own port and channel,
// its first two parameters. Being an Engine all the same, the arp, note
// lengths, motion and pressure reach it the way they reach a synth.
namespace melodee {

namespace midi_track {
// Port and channel; bank and program (sent on a change and when the track
// loads); three controllers on Stage's CC page, their numbers and values
// (sent on a change, and as Motion plays them).
enum Param : int { Port, Channel, BankMsb, BankLsb, Program, CcNumber1, CcNumber2, CcNumber3, Cc1, Cc2, Cc3, Count };
constexpr int routing_params = 2; // Port and Channel: what decides where notes go (no Motion lane)
constexpr bool is_cc(int id) { return id >= Cc1 && id <= Cc3; }
constexpr bool is_program(int id) { return id >= BankMsb && id <= Program; }
// The General MIDI name of a controller, for the CC page ("Brightness"), or nullptr.
inline const char* cc_name(unsigned cc) {
    switch (cc) {
    case 1: return "Mod wheel"; case 2: return "Breath"; case 4: return "Foot"; case 5: return "Glide time";
    case 7: return "Volume"; case 8: return "Balance"; case 10: return "Pan"; case 11: return "Expression";
    case 64: return "Sustain"; case 65: return "Glide"; case 66: return "Sostenuto"; case 67: return "Soft";
    case 71: return "Resonance"; case 72: return "Release"; case 73: return "Attack"; case 74: return "Brightness";
    case 75: return "Decay"; case 76: return "Vibrato rate"; case 77: return "Vibrato depth"; case 78: return "Vibrato delay";
    case 91: return "Reverb"; case 93: return "Chorus"; case 94: return "Detune"; case 95: return "Phaser";
    default: return nullptr;
    }
}
// Port's values, in the order Edit shows them (the file keeps the number).
enum PortChoice : int { External, UsbC, AllUsb, PortCount };
inline MidiDest dest_of(int port) {
    switch (port) {
    case UsbC: return MidiDest::UsbC;
    case AllUsb: return MidiDest::AllUsb;
    default: return MidiDest::External;
    }
}
} // namespace midi_track

class MidiEngine final : public Engine {
public:
    void note_on(int, int) override {}
    void note_off(int) override {}
    void all_notes_off() override {}
    void silence() override {}
    void set_param(int, int) override {}
    void render(float* out, unsigned frames) override { for (unsigned f = 0; f < frames; ++f) out[f] = 0; }
    bool sounding() const override { return false; }
};

} // namespace melodee
