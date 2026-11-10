// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/midi_io.h"
#include "melodee/engine/engine_host.h"
#include "melodee/model/sampler.h"
#include "melodee/shell/context.h"
#include "melodee/shell/midi_config_rows.h"
#include "melodee/shell/project_store.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cstdio>

namespace melodee {
using Type = Command::Type;

namespace {
int64_t ns_of(MidiIo::Clock::time_point t) {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}
MidiPacket packet_of(const juce::MidiMessage& m) {
    MidiPacket p;
    p.size = unsigned(std::min<int>(m.getRawDataSize(), int(p.bytes.size())));
    std::copy(m.getRawData(), m.getRawData() + p.size, p.bytes.begin());
    return p;
}
} // namespace

// --- Context: what MIDI in plays ---

void Context::midi_key(unsigned t, int k, int velocity, bool down) {
    t %= track_count;
    if (k < 0 || k > 127) return;
    auto& tr = project.tracks[t];
    if (down) midi_held[t].set(size_t(k));
    else if (midi_held[t].test(size_t(k))) midi_held[t].reset(size_t(k));
    else return; // never started here (or already let go)
    from_midi = true;
    if (t == ui.track) { // as the pads: the arp, Seq Record, Capture's history
        if (down && tr.engine == EngineKind::Sampler) tr.last_pad = uint8_t((unsigned(k) - sampler_first_note) % sampler_pads);
        key(k, velocity, down);
    } else if (tr.arp) {
        if (down || !tr.arp_latch) send({Type::ArpKey, uint8_t(t), 0, uint16_t(k), int16_t(down ? velocity : 0)});
    } else {
        note(t, k, down ? velocity : 0);
    }
    from_midi = false;
}

void Context::midi_release_all() {
    for (unsigned t = 0; t < track_count; ++t)
        for (int k = 0; k < 128; ++k)
            if (midi_held[t].test(size_t(k))) midi_key(t, k, 0, false);
}

void Context::change_midi(unsigned row, int way) {
    if (row >= ResetRow) return;
    midi_row_step(ui.settings, track(), row, way);
    if (row == TrackChannelRow) dirty = true; // the project keeps it
    if (store) store->save_settings(ui.settings.text());
}

void Context::reset_settings() {
    ui.settings.reset_factory();
    if (store) store->save_settings(ui.settings.text());
    input_changed(); // Input Config's source and monitor as they come
    notice("Factory settings");
}

// --- In ---

void MidiIo::receive(Context& c, const MidiPortInfo& port, const MidiPacket& packet) {
    const auto& config = c.ui.settings.midi;
    if (!packet.size || !port_is(config.port, port.driver, port.surface)) return;
    ins.fetch_add(1, std::memory_order_relaxed);
    c.ui.midi_in_at = c.now;
    if (config.thru && thru_count < thru.size()) thru[thru_count++] = packet;
    juce::MidiMessage m(packet.bytes.data(), int(packet.size));
    last_in.store((unsigned(packet.bytes[0]) << 16) | (packet.size > 1 ? unsigned(packet.bytes[1]) << 8 : 0) |
                  (packet.size > 2 ? packet.bytes[2] : 0), std::memory_order_relaxed);

    // Sync In: MIDI clock leads the transport and the tempo.
    bool clock_in = config.sync_in == MidiSync::Clock;
    if (m.isMidiClock()) {
        if (!clock_in) return;
        pulses_in.fetch_add(1, std::memory_order_relaxed);
        if (last_pulse_ns && packet.timestamp_ns - last_pulse_ns < 2000000) bunched.fetch_add(1, std::memory_order_relaxed);
        last_pulse_ns = packet.timestamp_ns;
        follower.pulse(packet.timestamp_ns);
        unsigned tempo = follower.tempo_tenths();
        if (tempo && tempo != tempo_sent) { c.send({Type::ExtSync, 0, 0, 2, int16_t(tempo)}); tempo_sent = tempo; tempo_seen.store(tempo); }
        jitter_in.store(follower.jitter_us(), std::memory_order_relaxed);
        if (ext_running.load(std::memory_order_relaxed)) {
            // Let the sequencer through a block after the pulse arrived, at that very frame: steady, not block by block.
            const auto& clock = c.host.audio_clock();
            Command pulse {Type::ExtSync, 0, 0, 1};
            pulse.position = clock.known() ? uint32_t(clock.frame_at(int64_t(packet.timestamp_ns)) + 128) : 0;
            c.send(pulse);
        }
        return;
    }
    if (m.isSongPositionPointer()) {
        if (!clock_in) return;
        song_position = uint32_t(m.getSongPositionPointerMidiBeat()) * sixteenth_ticks; // a MIDI beat is a sixteenth
        if (!c.host.playing()) c.ui.locate = song_position;
        return;
    }
    if (m.isMidiStart() || m.isMidiContinue()) {
        if (!clock_in) return;
        bool from_start = m.isMidiStart();
        if (c.host.playing()) c.stop();
        ext_running.store(true, std::memory_order_relaxed);
        if (from_start) { c.ui.locate = 0; c.play(true); }
        else { c.ui.locate = song_position; c.play_continue(true); }
        return;
    }
    if (m.isMidiStop()) {
        if (!clock_in) return;
        ext_running.store(false, std::memory_order_relaxed);
        c.send({Type::ExtSync, 0, 0, 4});
        if (c.host.playing()) c.stop();
        return;
    }
    if (m.isQuarterFrame() || m.isFullFrame()) { // MTC in: phase 9b
        mtc_in.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (packet.size > 3 || packet.bytes[0] >= 0xf0) return;

    // Channel messages.
    int channel = m.getChannel();
    if (m.isProgramChange()) {
        if (!config.program_change || (config.in_channel && config.in_channel != channel)) return;
        int pattern = m.getProgramChangeNumber();
        if (pattern >= int(pattern_count)) return;
        for (unsigned t = 0; t < track_count; ++t) if (c.project.tracks[t].used()) c.launch(t, unsigned(pattern));
        c.notice("Program change: P" + std::to_string(pattern + 1));
        return;
    }
    std::array<uint8_t, track_count> own {};
    for (unsigned t = 0; t < track_count; ++t) own[t] = c.project.tracks[t].midi_channel;
    uint16_t tracks = note_tracks(config, own, c.ui.track, unsigned(channel));
    if (m.isNoteOff(true)) { // a note off (or on at velocity 0) ends the note wherever it is held, even after a track change
        for (unsigned t = 0; t < track_count; ++t) {
            int key = note_key(c.project.tracks[t], m.getNoteNumber());
            if (key >= 0 && c.midi_held[t].test(size_t(key))) c.midi_key(t, key, 0, false);
        }
        return;
    }
    if (m.isNoteOn()) {
        if (!config.pad_in) return;
        for (unsigned t = 0; t < track_count; ++t) {
            if (!((tracks >> t) & 1)) continue;
            int key = note_key(c.project.tracks[t], m.getNoteNumber());
            if (key >= 0) c.midi_key(t, key, m.getVelocity(), true);
        }
        return;
    }
    if (m.isAftertouch() || m.isChannelPressure()) {
        if (!config.pad_in || !c.ui.settings.pressure) return;
        int value = m.isAftertouch() ? m.getAfterTouchValue() : m.getChannelPressureValue();
        for (unsigned t = 0; t < track_count; ++t) {
            if (!((tracks >> t) & 1)) continue;
            if (m.isAftertouch()) {
                int key = note_key(c.project.tracks[t], m.getNoteNumber());
                if (key >= 0) c.send({Type::Pressure, uint8_t(t), 0, uint16_t(key), int16_t(value)});
            } else {
                for (int k = 0; k < 128; ++k)
                    if (c.midi_held[t].test(size_t(k))) c.send({Type::Pressure, uint8_t(t), 0, uint16_t(k), int16_t(value)});
            }
        }
        return;
    }
    if (m.isController()) {
        for (unsigned t = 0; t < track_count; ++t) {
            if (!((tracks >> t) & 1)) continue;
            if (m.getControllerNumber() == 1) { // the mod wheel, as the fader's
                c.project.tracks[t].wheel = uint8_t(m.getControllerValue());
                c.send({Type::Wheel, uint8_t(t), 0, 0, int16_t(m.getControllerValue() * 1000 / 127)});
            } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
                for (int k = 0; k < 128; ++k) if (c.midi_held[t].test(size_t(k))) c.midi_key(t, k, 0, false);
            }
        }
    }
}

void MidiIo::lost(Context& c) {
    c.midi_release_all();
    if (ext_running.exchange(false)) c.send({Type::ExtSync, 0, 0, 4});
    follower.reset();
}

// --- Out ---

void MidiIo::configure(Context& c, bool force) {
    const auto& config = c.ui.settings.midi;
    bool changed = force || !configured || config != config_sent;
    if (changed) {
        if (config.cv_out && !config_sent.cv_out) cv_blocked = false; // turned on again: try the line again
        bool cv = config.cv_out && !cv_blocked;
        c.send({Type::MidiSync, 0, 0, 0, int16_t(config.sync_out == MidiSync::Clock)});
        c.send({Type::MidiSync, 0, 0, 1, int16_t(config.sync_out == MidiSync::Mtc)});
        c.send({Type::MidiSync, 0, 0, 3, int16_t(config.cv_ppqn())});
        c.send({Type::MidiSync, 0, 0, 2, int16_t(cv)});
        bool follow = config.sync_in == MidiSync::Clock;
        if (!configured || follow != (config_sent.sync_in == MidiSync::Clock)) {
            c.send({Type::ExtSync, 0, 0, 0, int16_t(follow)});
            if (!follow) { ext_running.store(false); tempo_sent = 0; tempo_seen.store(0); follower.reset(); }
        }
        config_sent = config;
    }
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = c.project.tracks[t];
        auto channel = uint8_t(out_channel_of(config, track));
        auto mode = uint8_t(config.pad_out);
        if (configured && !force && channel == channel_sent[t] && mode == mode_sent[t]) continue;
        c.send({Type::MidiTrack, uint8_t(t), 0, channel, int16_t(mode)});
        channel_sent[t] = channel; mode_sent[t] = mode;
    }
    configured = true;
}

void MidiIo::send_all(Context& c, MidiOutput& out, const MidiPacket& packet) {
    const auto& config = c.ui.settings.midi;
    bool sent = false;
    for (const auto& port : out.ports())
        if (port.output && port_is(config.port, port.driver, port.surface)) sent |= out.send(port.address, packet);
    if (sent) { outs.fetch_add(1, std::memory_order_relaxed); c.ui.midi_out_at = c.now; }
}

void MidiIo::deliver(Context& c, MidiOutput& out, const MidiOutEvent& e) {
    using K = MidiOutEvent::Kind;
    switch (e.kind) {
    case K::NoteOn: send_all(c, out, packet_of(juce::MidiMessage::noteOn(e.channel, e.a, juce::uint8(e.b)))); break;
    case K::NoteOff: send_all(c, out, packet_of(juce::MidiMessage::noteOff(e.channel, e.a))); break;
    case K::Clock: send_all(c, out, packet_of(juce::MidiMessage::midiClock())); break;
    case K::Start: send_all(c, out, packet_of(juce::MidiMessage::midiStart())); break;
    case K::Stop: send_all(c, out, packet_of(juce::MidiMessage::midiStop())); break;
    case K::Continue:
        send_all(c, out, packet_of(juce::MidiMessage::songPositionPointer(int(e.value))));
        send_all(c, out, packet_of(juce::MidiMessage::midiContinue()));
        break;
    case K::MtcQuarter: send_all(c, out, packet_of(juce::MidiMessage::quarterFrame(e.a, e.b))); break;
    case K::MtcFull: {
        unsigned f = e.value % 25, s = e.value / 25 % 60, m = e.value / 1500 % 60, h = e.value / 90000 % 24;
        send_all(c, out, packet_of(juce::MidiMessage::fullFrame(int(h), int(m), int(s), int(f), juce::MidiMessage::fps25)));
        break;
    }
    case K::CvHigh:
    case K::CvLow:
        if (cv_blocked || (e.kind == K::CvLow && !cv_taken)) break; // the line is only ever taken by a pulse (opt-in)
        if (!out.sync(e.kind == K::CvHigh)) { // the kernel or another program holds SYNC OUT
            cv_blocked = true; cv_ok.store(false);
            c.notice("CV/Sync Out: the jack is busy");
            c.send({Type::MidiSync, 0, 0, 2, 0});
            break;
        }
        cv_ok.store(true); cv_taken = true;
        if (e.kind == K::CvHigh) cv_pulses.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void MidiIo::pump(Context& c, MidiOutput& out, Clock::time_point now) {
    configure(c);
    for (unsigned n = 0; n < thru_count; ++n) send_all(c, out, thru[n]);
    thru_count = 0;
    const auto& clock = c.host.audio_clock();
    int64_t now_ns = ns_of(now);
    MidiOutEvent e;
    while (c.host.take_midi(e)) {
        int64_t due = clock.known() ? clock.heard_at(e.frame) : now_ns;
        if (pending_count == pending.size()) { deliver(c, out, e); continue; } // no room to wait: now
        unsigned at = pending_count;
        while (at > 0 && pending[at - 1].due > due) { pending[at] = pending[at - 1]; --at; } // after those due at the same time
        pending[at] = {due, e};
        ++pending_count;
    }
    // Due within a fifth of a millisecond: now (the next wake-up would be later than that).
    unsigned sent = 0;
    while (sent < pending_count && pending[sent].due <= now_ns + 200000) {
        // How late this thread sends against when the frame is heard (USB or the UART add their own after).
        int64_t late = std::max<int64_t>(0, now_ns - pending[sent].due);
        late_sum += late; ++late_count; late_max = std::max(late_max, late);
        deliver(c, out, pending[sent++].event);
    }
    if (late_count >= 200) { // kept per 200 messages for `status`
        late_avg_us.store(unsigned(late_sum / late_count / 1000)); late_max_us.store(unsigned(late_max / 1000));
        late_sum = late_max = 0; late_count = 0;
    }
    if (sent) {
        std::copy(pending.begin() + sent, pending.begin() + pending_count, pending.begin());
        pending_count -= sent;
    }
    if (pending_count) out.wake_at(Clock::time_point(std::chrono::nanoseconds(pending[0].due)));
}

std::string MidiIo::summary() const {
    char last[16];
    std::snprintf(last, sizeof(last), "%06x", last_in.load());
    return " melodee_midi_in=" + std::to_string(ins.load()) + " melodee_midi_out=" + std::to_string(outs.load()) +
           " melodee_midi_last=" + last + " melodee_clock_in=" + std::to_string(pulses_in.load()) +
           " melodee_clock_jitter_us=" + std::to_string(jitter_in.load()) + " melodee_clock_bunched=" + std::to_string(bunched.load()) + " melodee_ext=" + std::to_string(int(ext_running.load())) + " melodee_ext_tempo=" + std::to_string(tempo_seen.load()) +
           " melodee_cv_pulses=" + std::to_string(cv_pulses.load()) + " melodee_cv_ok=" + std::to_string(int(cv_ok.load())) +
           " melodee_mtc_in=" + std::to_string(mtc_in.load()) + " melodee_midi_late_us=" + std::to_string(late_avg_us.load()) + "/" +
           std::to_string(late_max_us.load());
}

} // namespace melodee
