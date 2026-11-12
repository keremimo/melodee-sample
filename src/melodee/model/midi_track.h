// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/sampler.h"
#include <algorithm>
#include <array>
#include <cstdint>

// A MIDI track's own (docs/MIDI_TRACKS.md M3-M4) beside its parameters (engine/midi_engine.h midi_track::Param):
// what is no number. Kept in MIDT from its version 3.
namespace melodee {

struct MidiDevice {
    char device[32] {};                         // Port USB device: the ALSA port's name, matched as devices come and go
    std::array<uint8_t, sampler_pads> drum_note {}; // Drum pads: each pad's note (pad n of bank b: 16 b + n), General MIDI from 36
    char sysex[64] {};                          // a .syx in the card's SysEx folder, sent on load (SysexOnLoad) or by hand
    MidiDevice() { for (unsigned n = 0; n < sampler_pads; ++n) drum_note[n] = uint8_t(std::min(36u + n, 127u)); }
};

// General MIDI's drum names, short (a pad's cell), for Drum pads; nullptr outside 27-87.
inline const char* gm_drum_name(unsigned note) {
    static const char* names[] {"High Q", "Slap", "Scratch In", "Scratch Out", "Sticks", "Sq Click", "Metro Click",
                                "Metro Bell", "Kick 2", "Kick", "Side Stick", "Snare", "Clap", "E Snare",
                                "Floor Tom 1", "Closed Hat", "Floor Tom 2", "Pedal Hat", "Low Tom", "Open Hat",
                                "Mid Tom 1", "Mid Tom 2", "Crash", "High Tom", "Ride", "China", "Ride Bell",
                                "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2", "Hi Bongo",
                                "Lo Bongo", "Mute Conga", "Open Conga", "Lo Conga", "Hi Timbale", "Lo Timbale",
                                "Hi Agogo", "Lo Agogo", "Cabasa", "Maracas", "Whistle S", "Whistle L", "Guiro S",
                                "Guiro L", "Claves", "Hi Block", "Lo Block", "Mute Cuica", "Open Cuica", "Mute Tri",
                                "Open Tri", "Shaker", "Jingle Bell", "Bell Tree", "Castanets", "Mute Surdo",
                                "Open Surdo"};
    return note >= 27 && note < 27 + sizeof(names) / sizeof(names[0]) ? names[note - 27] : nullptr;
}

} // namespace melodee
