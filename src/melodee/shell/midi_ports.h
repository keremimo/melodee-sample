// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/midi_engine.h"
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <cstring>
#include <string>

// Roadmap P4 (docs/MIDI_TRACKS.md M4): a MIDI track's DEVICE K1 and SYSEX K1 as choices, from what the MIDI thread
// last saw (UiState::usb_devices, sysex_names). Pure: the cards and the shell share them.
namespace melodee {

// External, USB-C, each USB device connected (by name), a device chosen that is not there now (OFFLINE), All USB.
inline bool midi_device_listed(const UiState& ui, const char* name) {
    for (unsigned n = 0; n < ui.usb_device_count; ++n) if (std::strcmp(ui.usb_devices[n].data(), name) == 0) return true;
    return false;
}
inline bool midi_device_missing(const UiState& ui, const Track& track) {
    return track.params[midi_track::Port] == midi_track::UsbDevice && track.midi.device[0] && !midi_device_listed(ui, track.midi.device);
}
inline unsigned midi_port_count(const UiState& ui, const Track& track) { return 3u + ui.usb_device_count + (midi_device_missing(ui, track) ? 1u : 0u); }
inline int midi_port_index(const UiState& ui, const Track& track) {
    using namespace midi_track;
    switch (track.params[Port]) {
    case External: return 0;
    case UsbC: return 1;
    case UsbDevice:
        for (unsigned n = 0; n < ui.usb_device_count; ++n) if (std::strcmp(ui.usb_devices[n].data(), track.midi.device) == 0) return int(2 + n);
        return int(2 + ui.usb_device_count);
    default: return int(midi_port_count(ui, track)) - 1;
    }
}
inline std::string midi_port_text(const UiState& ui, const Track& track, int choice, bool* offline = nullptr) {
    if (offline) *offline = false;
    int last = int(midi_port_count(ui, track)) - 1;
    if (choice <= 0) return "External";
    if (choice == 1) return "USB-C";
    if (choice >= last) return "All USB";
    unsigned n = unsigned(choice - 2);
    if (n < ui.usb_device_count) return ui.usb_devices[n].data();
    if (offline) *offline = true;
    return track.midi.device;
}
// The SysEx file picked: 0 none (or not in the folder's list), else its place in it + 1.
inline int midi_sysex_choice(const UiState& ui, const Track& track) {
    for (unsigned n = 0; n < ui.sysex_count; ++n) if (std::strcmp(ui.sysex_names[n].data(), track.midi.sysex) == 0) return int(n + 1);
    return 0;
}

} // namespace melodee
