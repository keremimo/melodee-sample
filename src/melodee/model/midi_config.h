// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

// The MPC's MIDI Config (Shift + Pad 8, parity phase 9), kept with Melodee's
// settings: which port MIDI goes in and out of, the channels, whether MIDI
// plays the pads and the pads (and patterns) send MIDI, clock sync both
// ways, MIDI Thru, Program Change to patterns and the CV clock on SYNC OUT.
// A track may also have a channel of its own (Track::midi_channel, kept with
// the project): notes on it play that track whatever is selected, from its own
// input (Track::midi_in: any port, the jacks or USB), so each track can have a
// controller of its own. Remote: CCs on one channel select a track, launch a
// pattern on the selected track or launch a scene, the CC's value saying which.
namespace melodee {

enum class MidiPort : uint8_t { External, Usb, Count };     // External: the 1/8" jacks (UART5); USB: USB-C (the gadget) and USB host
enum class PadMidiOut : uint8_t { Never, Always, Empty, Count }; // Empty: only notes with no sound of their own (an empty pad or track)
enum class MidiSync : uint8_t { Off, Clock, Mtc, Count };
// Where one message goes out: MIDI Config's port (pads on audio tracks, the
// clocks, Thru), or a MIDI track's own (docs/MIDI_TRACKS.md): the 1/8" jacks,
// USB-C's gadget alone, or every USB output (the gadget and USB host devices).
enum class MidiDest : uint8_t { Config, External, UsbC, AllUsb, UsbDevice, Count }; // UsbDevice: a MIDI track's named device (roadmap P4)
// Where a track with a channel of its own listens: every port, the 1/8" jacks or USB (the gadget and USB host).
enum class TrackMidiIn : uint8_t { Any, External, Usb, Count };
inline const char* track_midi_in_name(TrackMidiIn in) {
    static const char* const names[] {"Any", "External", "USB"};
    return names[unsigned(in) % unsigned(TrackMidiIn::Count)];
}

struct MidiConfig {
    MidiPort port = MidiPort::External;
    uint8_t in_channel = 0;        // 0 All, 1..16: what plays the selected track
    uint8_t out_channel = 1;       // 1..16: what the pads and patterns send on (a track's own channel wins)
    bool pad_in = true;            // MIDI notes play the pads
    PadMidiOut pad_out = PadMidiOut::Never;
    MidiSync sync_in = MidiSync::Off, sync_out = MidiSync::Off;
    bool thru = false;             // what comes in goes out again, as it came
    bool program_change = false;   // Program Change 1..16 launches that pattern on every track
    bool cv_out = false;           // a clock on SYNC OUT (CV_CLK_OUT)
    uint8_t cv_base = 2, cv_division = 1; // pulses a quarter: base x division (2: Volca and Pocket Operator sync)
    // Remote (from any port): 0 Off, 1..16 the channel; each CC 0 Off, else its number. The value picks the track
    // (0 = T1), the selected track's pattern (0 = A01) or the scene (0 = the first).
    uint8_t remote_channel = 0;
    uint8_t track_cc = 102, pattern_cc = 103, scene_cc = 104;
    unsigned cv_ppqn() const { return unsigned(cv_base) * cv_division; }
    bool operator==(const MidiConfig& o) const {
        return port == o.port && in_channel == o.in_channel && out_channel == o.out_channel && pad_in == o.pad_in &&
               pad_out == o.pad_out && sync_in == o.sync_in && sync_out == o.sync_out && thru == o.thru &&
               program_change == o.program_change && cv_out == o.cv_out && cv_base == o.cv_base && cv_division == o.cv_division &&
               remote_channel == o.remote_channel && track_cc == o.track_cc && pattern_cc == o.pattern_cc && scene_cc == o.scene_cc;
    }
    bool operator!=(const MidiConfig& o) const { return !(*this == o); }
};

inline const char* midi_port_name(MidiPort p) { return p == MidiPort::Usb ? "USB" : "External"; }
inline const char* pad_midi_out_name(PadMidiOut p) {
    static const char* const names[] {"Never", "Always", "Empty"};
    return names[unsigned(p) % unsigned(PadMidiOut::Count)];
}
inline const char* midi_sync_name(MidiSync s) {
    static const char* const names[] {"Off", "MIDI Clock", "MTC"};
    return names[unsigned(s) % unsigned(MidiSync::Count)];
}
inline std::string midi_channel_text(unsigned channel, const char* zero) { return channel ? std::to_string(channel) : std::string(zero); }

// The settings file's keys (new keys only: older files load the defaults).
template <class Out> void write_midi_config(Out& out, const MidiConfig& m) {
    out << "midi_port=" << int(m.port) << "\nmidi_in_channel=" << int(m.in_channel) << "\nmidi_out_channel=" << int(m.out_channel)
        << "\npad_midi_in=" << int(m.pad_in) << "\npad_midi_out=" << int(m.pad_out) << "\nmidi_sync_in=" << int(m.sync_in)
        << "\nmidi_sync_out=" << int(m.sync_out) << "\nmidi_thru=" << int(m.thru) << "\nprogram_change=" << int(m.program_change)
        << "\ncv_out=" << int(m.cv_out) << "\ncv_base=" << int(m.cv_base) << "\ncv_division=" << int(m.cv_division)
        << "\nremote_channel=" << int(m.remote_channel) << "\nremote_track_cc=" << int(m.track_cc)
        << "\nremote_pattern_cc=" << int(m.pattern_cc) << "\nremote_scene_cc=" << int(m.scene_cc) << '\n';
}
// One key=value; false when the key is not MIDI Config's.
inline bool read_midi_config(MidiConfig& m, const std::string& key, int value) {
    auto clamp = [](int v, int lo, int hi) { return std::clamp(v, lo, hi); };
    if (key == "midi_port") m.port = MidiPort(clamp(value, 0, int(MidiPort::Count) - 1));
    else if (key == "midi_in_channel") m.in_channel = uint8_t(clamp(value, 0, 16));
    else if (key == "midi_out_channel") m.out_channel = uint8_t(clamp(value, 1, 16));
    else if (key == "pad_midi_in") m.pad_in = value != 0;
    else if (key == "pad_midi_out") m.pad_out = PadMidiOut(clamp(value, 0, int(PadMidiOut::Count) - 1));
    else if (key == "midi_sync_in") m.sync_in = MidiSync(clamp(value, 0, int(MidiSync::Count) - 1));
    else if (key == "midi_sync_out") m.sync_out = MidiSync(clamp(value, 0, int(MidiSync::Count) - 1));
    else if (key == "midi_thru") m.thru = value != 0;
    else if (key == "program_change") m.program_change = value != 0;
    else if (key == "cv_out") m.cv_out = value != 0;
    else if (key == "cv_base") m.cv_base = uint8_t(clamp(value, 1, 8));
    else if (key == "cv_division") m.cv_division = uint8_t(clamp(value, 1, 24));
    else if (key == "remote_channel") m.remote_channel = uint8_t(clamp(value, 0, 16));
    else if (key == "remote_track_cc") m.track_cc = uint8_t(clamp(value, 0, 127));
    else if (key == "remote_pattern_cc") m.pattern_cc = uint8_t(clamp(value, 0, 127));
    else if (key == "remote_scene_cc") m.scene_cc = uint8_t(clamp(value, 0, 127));
    else return false;
    return true;
}

} // namespace melodee
