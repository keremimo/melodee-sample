// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/shell/context.h"

// Roadmap P4, MIDI tracks' devices, SysEx and inputs (docs/MIDI_TRACKS.md M4-M5): what the shell does for them.
namespace melodee {

// DEVICE's K1 (shell/midi_ports.h): the track to choice `choice`.
void set_midi_port(Context& c, unsigned track, int choice);
// SYSEX: the .syx files in the card's SysEx folder (listed when first wanted), the one picked, and sending it (bounded,
// 64 KB: MidiIo sends it in pieces, paced on the jacks).
void list_sysex(Context& c);
void set_sysex(Context& c, unsigned track, int choice);
void send_sysex(Context& c, unsigned track);
void sysex_on_load(Context& c); // a project just loaded: every MIDI track's SysEx with ON LOAD set
// The ADC route and USB-C a MIDI track's input needs (InputRoute::need_*), all tracks together; and each track told
// what it plays: its input when the route gives it, else none (IN USE).
unsigned midi_input_needs(const Project& project);
void midi_audio_tick(Context& c, unsigned needs, bool source_mic);
bool midi_binding(Context& c, const Binding& b, int value); // Context::apply's: MidiPort, MidiSysexFile

} // namespace melodee
