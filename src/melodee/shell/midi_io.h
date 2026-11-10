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
    void send_all(Context& c, MidiOutput& out, const MidiPacket& packet); // to every output of the chosen port
    struct Pending { int64_t due = 0; MidiOutEvent event; };
    std::array<Pending, 512> pending {}; // sorted by when it is heard
    unsigned pending_count = 0;
    std::array<MidiPacket, 32> thru {};  // Thru: what came in, sent on in the next pump
    unsigned thru_count = 0;
    ClockFollower follower;
    unsigned tempo_sent = 0;
    uint32_t song_position = 0; // ticks, from Song Position: where Continue carries on
    bool configured = false, cv_blocked = false, cv_taken = false;
    MidiConfig config_sent;
    std::array<uint8_t, track_count> channel_sent {}, mode_sent {};
    std::atomic<bool> ext_running {false}; // Start or Continue came, Stop not yet
    std::atomic<unsigned> tempo_seen {0}, ins {0}, outs {0}, pulses_in {0}, last_in {0}, cv_pulses {0}, mtc_in {0};
    std::atomic<bool> cv_ok {true};
    int64_t late_sum = 0, late_max = 0;      // ns behind the due time, over the messages since the last report
    unsigned late_count = 0;
    std::atomic<unsigned> late_avg_us {0}, late_max_us {0}, jitter_in {0}, bunched {0}; // bunched: clock pulses in under 2 ms
    uint64_t last_pulse_ns = 0;
    // MTC in: the reader; it started what plays (and the engine has taken it up); held: stopped here while the master
    // ran on, so it waits for the master to stop before following again.
    MtcReader mtc;
    bool mtc_playing = false, mtc_taken = false, mtc_held = false;
    std::atomic<bool> mtc_leads {false};
    std::atomic<unsigned> mtc_state {0}, mtc_rate {25}, mtc_code {0}, mtc_jumps {0};
};

} // namespace melodee
