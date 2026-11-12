// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine.h"
#include "melodee/model/midi_config.h"
#include <string>

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
// Roadmap P4 (docs/MIDI_TRACKS.md M2-M5): 24 knob slots, their values from Cc1 on (Edit's SLOTS pages, three a page;
// Stage's CC page the first three) and each slot's address (what it sends: a CC, a 14-bit CC, NRPN, pitch bend or
// channel pressure; the first three in CcNumber1-3 as before); how pad pressure goes out, the pitch bend, the track's
// delay, Keys or Drum pads, an input as its sound (with monitor and level), its SysEx sent as the project loads, and a
// program of each pattern's own (0: the track's), sent at its launch.
enum Param : int {
    Port, Channel, BankMsb, BankLsb, Program, CcNumber1, CcNumber2, CcNumber3, Cc1, Cc2, Cc3,
    SlotAddress4 = Cc1 + 24, // slots 4-24's addresses
    PressureOut = SlotAddress4 + 21, Bend, Delay, AudioIn, AudioMonitor, AudioLevel, SysexOnLoad, PatternProgram0,
    Count = PatternProgram0 + 16
};
constexpr int slots = 24, slot_pages = slots / 3;
constexpr int slot_value(int k) { return Cc1 + k; }
constexpr int slot_address(int k) { return k < 3 ? CcNumber1 + k : SlotAddress4 + (k - 3); }
constexpr int routing_params = 2; // Port and Channel: what decides where notes go (no Motion lane)
constexpr bool is_cc(int id) { return id >= Cc1 && id < Cc1 + slots; } // a slot's value: a Motion lane
constexpr bool is_program(int id) { return id >= BankMsb && id <= Program; }
constexpr int slot_of_address(int id) { return id >= CcNumber1 && id <= CcNumber3 ? id - CcNumber1 : id >= SlotAddress4 && id < SlotAddress4 + 21 ? id - SlotAddress4 + 3 : -1; }
// A slot's address: what its knob sends. -1 none; CC 0-119; a 14-bit CC 0-31 (its LSB at +32); pitch bend; channel
// pressure; NRPN 0-1023.
enum class SlotKind : uint8_t { Off, Cc, Cc14, PitchBend, ChannelPressure, Nrpn };
constexpr int address_cc14 = 120, address_bend = 152, address_pressure = 153, address_nrpn = 154, address_last = address_nrpn + 1023;
constexpr SlotKind slot_kind(int a) {
    return a < 0 ? SlotKind::Off : a < address_cc14 ? SlotKind::Cc : a < address_bend ? SlotKind::Cc14 : a == address_bend ? SlotKind::PitchBend
         : a == address_pressure ? SlotKind::ChannelPressure : a <= address_last ? SlotKind::Nrpn : SlotKind::Off;
}
constexpr int slot_number(int a) {
    return a < address_cc14 ? a : a < address_bend ? a - address_cc14 : a >= address_nrpn ? a - address_nrpn : 0;
}
// How pad pressure goes out: as each note's (poly), the channel's, or not at all.
enum PressureChoice : int { PressurePoly, PressureChannel, PressureOff };
// A slot's knob as its card names it: its own CC's name or "CC 74", "CC14 1", "BEND", "PRESS", "NRPN 12"; "SLOT" off.
inline std::string slot_label(int address) {
    switch (slot_kind(address)) {
    case SlotKind::Cc: return "CC " + std::to_string(slot_number(address));
    case SlotKind::Cc14: return "CC14 " + std::to_string(slot_number(address));
    case SlotKind::PitchBend: return "BEND";
    case SlotKind::ChannelPressure: return "PRESS";
    case SlotKind::Nrpn: return "NRPN " + std::to_string(slot_number(address));
    default: return "SLOT";
    }
}
// An input as the track's sound (M5): none, or one of these (InputSource's order).
enum AudioChoice : int { AudioNone, AudioRearL, AudioRearR, AudioRear, AudioMic, AudioUsb, AudioCount };
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
// UsbDevice (M4): one USB device by its name (Track::midi.device), every USB output while it is not there.
enum PortChoice : int { External, UsbC, AllUsb, UsbDevice, PortCount };
inline MidiDest dest_of(int port) {
    switch (port) {
    case UsbC: return MidiDest::UsbC;
    case AllUsb: return MidiDest::AllUsb;
    case UsbDevice: return MidiDest::UsbDevice;
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
    bool sounding() const override { return audio != midi_track::AudioNone; }
    // Roadmap P4 (docs/MIDI_TRACKS.md M5): an input as the track's sound (the external instrument back in the mix):
    // which (midi_track::AudioChoice; none while its route is taken), and this block of it, filled by the engine host.
    bool stereo() const override { return audio != midi_track::AudioNone; }
    void render_stereo(float* left, float* right, unsigned frames) override {
        for (unsigned f = 0; f < frames && f < 128; ++f) { left[f] += in_l[f]; right[f] += in_r[f]; }
    }
    int audio = midi_track::AudioNone;
    float in_l[128] {}, in_r[128] {};
};

} // namespace melodee
