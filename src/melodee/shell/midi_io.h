// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/midi_out.h"
#include "melodee/shell/midi_route.h"
#include "melodee/shell/mtc_in.h"
#include "runtime/app.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// MIDI in and out on the MIDI thread (parity phase 9), as MIDI Config says.
//
// In: messages from the chosen port (juce::MidiMessage reads them) play the
// tracks their channel names, Song Position, Start, Continue and Stop run the
// transport and the clock's pulses lead the sequencer while Sync In is MIDI
// Clock (each with the kernel's receive stamp, so the tempo comes out steady);
// Program Change launches patterns; Thru sends it all on as it came.
// While Sync In is MTC (phase 9b), time code starts, places and stops the
// song or the patterns (shell/mtc_in.h reads it; the engine host keeps the
// sequencer on its time).
//
// Out: the engine host decides on the audio thread what goes out at which of
// its frames (pads, patterns, the clocks); here each becomes a
// juce::MidiMessage and is sent when its frame is heard, the runtime waking
// this thread for it (MidiOutput::wake_at). The CV clock is the sync line's
// level, set the same way.
namespace melodee {

class Context;
class EngineHost;

class MidiIo {
public:
    using Clock = std::chrono::steady_clock;
    void receive(Context& c, const MidiPortInfo& port, const MidiPacket& packet); // MIDI thread
    void lost(Context& c);                                                       // an input went away
    void pump(Context& c, MidiOutput& out, Clock::time_point now);               // every pass of the MIDI loop
    std::string summary(const EngineHost& host) const;                           // `status` fields, any thread
    // MIDI clock or MTC in leads the transport.
    bool following() const { return ext_running.load(std::memory_order_relaxed) || mtc_leads.load(std::memory_order_relaxed); }
    bool following_mtc() const { return mtc_leads.load(std::memory_order_relaxed); }
    unsigned tempo_in() const { return tempo_seen.load(std::memory_order_relaxed); }

private:
    void configure(Context& c, bool force = false);    // MIDI Config and the tracks' channels to the engine, when they change
    void deliver(Context& c, MidiOutput& out, const MidiOutEvent& e);
    void follow_mtc(Context& c, const MtcReader::Event& e); // what time code did: start, place, keep on time, stop
    void send_mtc(Context& c, uint16_t what, const MtcReader::Event& e);
    // To every output of `dest` (MIDI Config's port, or a MIDI track's own).
    // A MIDI track's USB device (`track`, roadmap P4 M4) by its name; while it is not there, every USB output.
    bool send_all(Context& c, MidiOutput& out, const MidiPacket& packet, MidiDest dest = MidiDest::Config, int track = -1); // true: sent somewhere
    std::atomic<uint32_t> offline_tracks {0}; // a bit a MIDI track whose USB device is missing (sent to every USB output)
    std::atomic<unsigned> echoed {0}, echo_guarded {0}; // roadmap P4 (M3): messages echoed on MIDI tracks, and held back (loop guard)
    // A CC, bend or pressure (`bytes`) echoed on `tracks` (MIDI tracks); a CC a slot sends turns that slot (heard, and
    // recorded under Seq Record on the selected track), as the bend does BEND.
    void echo_raw(Context& c, const uint8_t* bytes, unsigned size, uint16_t tracks, uint64_t ns);
    void do_remote(Context& c, const RemoteJob& job); // MIDI Config's Remote rows: a track, a pattern or a scene
    // Roadmap P4 (M4): the SysEx going out now (Context::sysex_queue's first): its track, bytes, how far, the next piece.
    struct SysexSending { bool active = false; uint8_t track = 0; std::vector<uint8_t> bytes; size_t at = 0; int64_t next_ns = 0; } sysex;
    std::atomic<unsigned> sysex_sent {0}, sysex_bytes {0};
    void panic(Context& c, MidiOutput& out); // All Sound Off, All Notes Off, pitch bend centre: every channel, every port
    struct Pending { int64_t due = 0; MidiOutEvent event; };
    std::array<Pending, 512> pending {}; // sorted by when it is heard
    unsigned pending_count = 0;
    std::array<MidiPacket, 32> thru {};  // Thru: what came in, sent on in the next pump
    unsigned thru_count = 0;
    ClockFollower follower;
    unsigned tempo_sent = 0;
    uint32_t song_position = 0; // ticks, from Song Position: where Continue carries on
    bool configured = false, cv_blocked = false, cv_taken = false;
    unsigned panics_done = 0; // Context::midi_panics answered
    MidiConfig config_sent;
    std::array<uint8_t, track_count> channel_sent {}, mode_sent {};
    std::atomic<bool> ext_running {false}; // Start or Continue came, Stop not yet
    std::atomic<unsigned> tempo_seen {0}, ins {0}, outs {0}, pulses_in {0}, last_in {0}, cv_pulses {0}, mtc_in {0};
    std::atomic<bool> cv_ok {true};
    int64_t late_sum = 0, late_max = 0;      // ns behind the due time, over the messages since the last report
    unsigned late_count = 0;
    std::atomic<unsigned> late_avg_us {0}, late_max_us {0}, jitter_in {0}, bunched {0}; // bunched: clock pulses in under 2 ms
    // MIDI tracks (docs/MIDI_TRACKS.md): messages sent on a track's own port, the last one (its bytes and MidiDest), panics.
    std::atomic<unsigned> track_outs {0}, last_out {0}, last_dest {0}, panics {0};
    uint64_t last_pulse_ns = 0;
    // MTC in: the reader; it started what plays (and the engine has taken it up); held: stopped here while the master
    // ran on, so it waits for the master to stop before following again.
    MtcReader mtc;
    bool mtc_playing = false, mtc_taken = false, mtc_held = false;
    std::atomic<bool> mtc_leads {false};
    std::atomic<unsigned> mtc_state {0}, mtc_rate {25}, mtc_code {0}, mtc_jumps {0};
};

} // namespace melodee
