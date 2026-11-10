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
enum Param : int { Port, Channel, Count };
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
