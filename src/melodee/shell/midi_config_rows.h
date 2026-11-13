// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/midi_config.h"
#include "melodee/model/project.h"
#include "melodee/shell/settings.h"
#include <string>

// MIDI Config's rows (Shift + Pad 8, parity phase 9) in the MPC's order, with
// Melodee's track channel and its input after Out channel, the Remote rows after Receive program change and the three takeover rows the
// MPC keeps here (they are Settings' rows too), then Melodee's MIDI Panic
// (docs/MIDI_TRACKS.md) and Reset Factory Settings.
namespace melodee {

enum MidiRow : uint8_t {
    MidiPortRow, InChannelRow, OutChannelRow, TrackChannelRow, TrackInRow, PadInRow, PadOutRow, SyncInRow, SyncOutRow, ThruRow,
    ProgramChangeRow, RemoteChannelRow, TrackCcRow, PatternCcRow, SceneCcRow, CvOutRow, CvBaseRow, CvDivisionRow, KnobTakeoverRow, KnobFxTakeoverRow, FaderTakeoverRow, PanicRow, ResetRow,
    MidiRows
};

inline const char* midi_row_name(unsigned row) {
    static const char* const names[] {"MIDI port", "MIDI in channel", "MIDI out channel", "Track channel", "Track MIDI in",
                                      "Pad MIDI in", "Pad MIDI out", "MIDI sync in", "MIDI sync out", "MIDI thru",
                                      "Receive program change", "Remote channel", "Remote track CC", "Remote pattern CC",
                                      "Remote scene CC", "CV/sync out", "CV/sync base", "CV/sync division", "Parameter takeover",
                                      "Knob FX takeover", "Fader takeover", "MIDI panic", "Reset factory settings"};
    return names[row % MidiRows];
}

inline std::string midi_row_value(const Settings& s, const Track& track, unsigned row) {
    const auto& m = s.midi;
    switch (row) {
    case MidiPortRow: return midi_port_name(m.port);
    case InChannelRow: return midi_channel_text(m.in_channel, "All");
    case OutChannelRow: return std::to_string(m.out_channel);
    case TrackChannelRow: return midi_channel_text(track.midi_channel, "Off");
    case TrackInRow: return track_midi_in_name(track.midi_in);
    case PadInRow: return m.pad_in ? "On" : "Off";
    case PadOutRow: return pad_midi_out_name(m.pad_out);
    case SyncInRow: return midi_sync_name(m.sync_in);
    case SyncOutRow: return midi_sync_name(m.sync_out);
    case ThruRow: return m.thru ? "On" : "Off";
    case ProgramChangeRow: return m.program_change ? "Sequence" : "Off";
    case RemoteChannelRow: return midi_channel_text(m.remote_channel, "Off");
    case TrackCcRow: return midi_channel_text(m.track_cc, "Off");
    case PatternCcRow: return midi_channel_text(m.pattern_cc, "Off");
    case SceneCcRow: return midi_channel_text(m.scene_cc, "Off");
    case CvOutRow: return m.cv_out ? "On" : "Off";
    case CvBaseRow: return std::to_string(m.cv_base);
    case CvDivisionRow: return std::to_string(m.cv_division);
    case KnobTakeoverRow: return takeover_name(s.knob_takeover);
    case KnobFxTakeoverRow: return takeover_name(s.knob_fx_takeover);
    case FaderTakeoverRow: return takeover_name(s.fader_takeover);
    default: return "";
    }
}

// One step along a row (lists go round, numbers stop at their ends). The
// takeover rows go through Settings::step, so both pages agree.
inline void midi_row_step(Settings& s, Track& track, unsigned row, int way) {
    auto& m = s.midi;
    int w = way < 0 ? -1 : 1;
    auto round = [w](int value, int count) { return (value + count + w) % count; };
    auto stop = [w](int value, int lo, int hi) { return std::clamp(value + w, lo, hi); };
    switch (row) {
    case MidiPortRow: m.port = MidiPort(round(int(m.port), int(MidiPort::Count))); break;
    case InChannelRow: m.in_channel = uint8_t(stop(m.in_channel, 0, 16)); break;
    case OutChannelRow: m.out_channel = uint8_t(stop(m.out_channel, 1, 16)); break;
    case TrackChannelRow: track.midi_channel = uint8_t(stop(track.midi_channel, 0, 16)); break;
    case TrackInRow: track.midi_in = TrackMidiIn(round(int(track.midi_in), int(TrackMidiIn::Count))); break;
    case PadInRow: m.pad_in = !m.pad_in; break;
    case PadOutRow: m.pad_out = PadMidiOut(round(int(m.pad_out), int(PadMidiOut::Count))); break;
    case SyncInRow: m.sync_in = MidiSync(round(int(m.sync_in), int(MidiSync::Count))); break;
    case SyncOutRow: m.sync_out = MidiSync(round(int(m.sync_out), int(MidiSync::Count))); break;
    case ThruRow: m.thru = !m.thru; break;
    case ProgramChangeRow: m.program_change = !m.program_change; break;
    case RemoteChannelRow: m.remote_channel = uint8_t(stop(m.remote_channel, 0, 16)); break;
    case TrackCcRow: m.track_cc = uint8_t(stop(m.track_cc, 0, 127)); break;
    case PatternCcRow: m.pattern_cc = uint8_t(stop(m.pattern_cc, 0, 127)); break;
    case SceneCcRow: m.scene_cc = uint8_t(stop(m.scene_cc, 0, 127)); break;
    case CvOutRow: m.cv_out = !m.cv_out; break;
    case CvBaseRow: m.cv_base = uint8_t(stop(m.cv_base, 1, 8)); break;
    case CvDivisionRow: m.cv_division = uint8_t(stop(m.cv_division, 1, 24)); break;
    case KnobTakeoverRow: s.step(Settings::KnobTakeover, way); break;
    case KnobFxTakeoverRow: s.step(Settings::KnobFxTakeover, way); break;
    case FaderTakeoverRow: s.step(Settings::FaderTakeover, way); break;
    default: break;
    }
}

} // namespace melodee
