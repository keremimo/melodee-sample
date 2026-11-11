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
std::string last_text(unsigned bytes) {
    char text[16];
    std::snprintf(text, sizeof(text), "%06x", bytes);
    return text;
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
    // An instrument under 16 Levels has the keyboard: the note is that pad, at the key
    // played. Kept from the press, so its end finds it whatever has changed meanwhile.
    if (down) {
        midi_played[t][size_t(k)] = k;
        if (midi_every_key(tr)) {
            const auto& pad = tr.pads[tr.levels_pad % sampler_pads];
            midi_played[t][size_t(k)] = pad_note(int(sampler_first_note + tr.levels_pad % sampler_pads), k - 60 - pad.pitch);
        }
    }
    const int played = midi_played[t][size_t(k)];
    from_midi = true;
    if (t == ui.track) { // as the pads: the arp, Seq Record, Capture's history
        if (down && played == k && tr.engine == EngineKind::Sampler) tr.last_pad = uint8_t((unsigned(k) - sampler_first_note) % sampler_pads);
        key(played, velocity, down);
    } else if (tr.arp) {
        if (down || !tr.arp_latch) send({Type::ArpKey, uint8_t(t), 0, uint16_t(played), int16_t(down ? velocity : 0)});
    } else {
        note(t, played, down ? velocity : 0);
    }
    from_midi = false;
}

bool Context::midi_every_key(const Track& tr) const {
    if (tr.engine != EngineKind::Sampler || !tr.levels || tr.arp) return false;
    const auto& pad = tr.pads[tr.levels_pad % sampler_pads];
    return pad.used() && project.samples[pad.sample % sample_slots].instrument();
}

void Context::midi_release_all() {
    for (unsigned t = 0; t < track_count; ++t)
        for (int k = 0; k < 128; ++k)
            if (midi_held[t].test(size_t(k))) midi_key(t, k, 0, false);
}

void Context::change_midi(unsigned row, int way) {
    if (row >= PanicRow) return;
    midi_row_step(ui.settings, track(), row, way);
    if (row == TrackChannelRow) dirty = true; // the project keeps it
    if (store) store->save_settings(ui.settings.text());
}

void Context::midi_panic() {
    midi_release_all();
    send({Type::AllNotesOff, 255}); // the engines, and the notes the engine host still owes an off
    ++midi_panics;
    notice("MIDI panic");
}

void Context::reset_settings() {
    ui.settings.reset_factory();
    if (store) store->save_settings(ui.settings.text());
    input_changed(); // Input Config's source and monitor as they come
    send_click();
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
    if (m.isQuarterFrame() || m.isFullFrame()) { // MTC in (phase 9b)
        mtc_in.fetch_add(1, std::memory_order_relaxed);
        if (config.sync_in != MidiSync::Mtc) return;
        if (m.isQuarterFrame()) {
            if (packet.size >= 2) follow_mtc(c, mtc.quarter(packet.bytes[1], packet.timestamp_ns));
        } else {
            int h = 0, mi = 0, s = 0, f = 0;
            juce::MidiMessage::SmpteTimecodeType type {};
            m.getFullFrameParameters(h, mi, s, f, type);
            follow_mtc(c, mtc.full(unsigned(h), unsigned(mi), unsigned(s), unsigned(f), MtcRate(int(type) & 3), packet.timestamp_ns));
        }
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
            int key = m.getNoteNumber();
            if (key >= 0 && key < 128 && c.midi_held[t].test(size_t(key))) c.midi_key(t, key, 0, false);
        }
        return;
    }
    if (m.isNoteOn()) {
        if (!config.pad_in) return;
        for (unsigned t = 0; t < track_count; ++t) {
            if (!((tracks >> t) & 1)) continue;
            int key = note_key(c.project.tracks[t], m.getNoteNumber(), c.midi_every_key(c.project.tracks[t]));
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

// --- MTC in ---

void MidiIo::send_mtc(Context& c, uint16_t what, const MtcReader::Event& e) {
    Command cmd {Type::MtcIn, 0, 0, what};
    cmd.position = uint32_t(c.host.audio_clock().heard_frame(int64_t(e.ns))); // the master was at e.seconds when this was heard
    cmd.micros = uint32_t(std::clamp(e.seconds, 0.0, 4000.0) * 1e6);
    c.send(cmd);
}

void MidiIo::follow_mtc(Context& c, const MtcReader::Event& e) {
    using K = MtcReader::Event::Kind;
    mtc_state.store(mtc.running() ? 2 : mtc.settling() ? 1 : 0, std::memory_order_relaxed);
    switch (e.kind) {
    case K::None: break;
    case K::Cue: { // the master located while stopped: the playhead there, as Play would start it
        if (c.host.playing()) break;
        double ticks = e.seconds * c.project.tempo_of(c.ui.lead_pattern) / 10.0 / 60.0 * ticks_per_quarter;
        if (c.ui.page == PageId::Song) {
            const auto& song = c.project.song;
            auto bar = unsigned(ticks / c.project.bar_ticks());
            if (song.looping() && bar >= song.loop_end) bar = song.loop_start + (bar - song.loop_start) % (song.loop_end - song.loop_start);
            c.ui.song_cursor = uint16_t(std::min(bar, 65535u));
        } else {
            c.ui.locate = uint32_t(ticks) % std::max<uint32_t>(c.pattern().length, 1);
        }
        break;
    }
    case K::Start:
    case K::Jump:
        if (mtc_held) break; // stopped here: not until the master stops and starts again
        if (!c.host.playing() && !mtc_playing) c.play(true); // no count-in; the song on Song, else the patterns
        if (!mtc_playing) mtc_taken = false;
        mtc_playing = true;
        send_mtc(c, 1, e);
        break;
    case K::Time:
        if (mtc_playing) send_mtc(c, 2, e);
        break;
    case K::Stop:
        if (mtc_playing && c.host.playing()) c.stop();
        mtc_playing = mtc_taken = mtc_held = false;
        break;
    }
    mtc_leads.store(mtc_playing && !mtc_held, std::memory_order_relaxed);
    mtc_rate.store(mtc.rate() == MtcRate::Fps24 ? 24 : mtc.rate() == MtcRate::Fps25 ? 25 : mtc.rate() == MtcRate::Fps30 ? 30 : 2997,
                   std::memory_order_relaxed);
    mtc_code.store(mtc.frame(), std::memory_order_relaxed);
    mtc_jumps.store(mtc.jumps(), std::memory_order_relaxed);
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
        if (config.sync_in != MidiSync::Mtc && configured && config_sent.sync_in == MidiSync::Mtc) { // MTC no longer leads
            mtc.reset();
            if (mtc_playing) c.send({Type::MtcIn, 0, 0, 0}); // what plays carries on by itself
            mtc_playing = mtc_taken = mtc_held = false;
            mtc_leads.store(false); mtc_state.store(0);
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

bool MidiIo::send_all(Context& c, MidiOutput& out, const MidiPacket& packet, MidiDest dest) {
    const auto& config = c.ui.settings.midi;
    bool sent = false;
    for (const auto& port : out.ports())
        if (port.output && dest_is(dest, config.port, port.driver, port.surface)) sent |= out.send(port.address, packet);
    if (!sent) return false;
    outs.fetch_add(1, std::memory_order_relaxed); c.ui.midi_out_at = c.now;
    if (dest == MidiDest::Config) return true;
    track_outs.fetch_add(1, std::memory_order_relaxed);
    last_out.store((unsigned(packet.bytes[0]) << 16) | (packet.size > 1 ? unsigned(packet.bytes[1]) << 8 : 0) |
                   (packet.size > 2 ? packet.bytes[2] : 0), std::memory_order_relaxed);
    last_dest.store(unsigned(dest), std::memory_order_relaxed);
    return true;
}

void MidiIo::panic(Context& c, MidiOutput& out) {
    // Notes waiting for their frame would start again after it: they go; their offs, the clocks and the rest stay.
    unsigned kept = 0;
    for (unsigned n = 0; n < pending_count; ++n)
        if (pending[n].event.kind != MidiOutEvent::Kind::NoteOn) pending[kept++] = pending[n];
    pending_count = kept;
    for (const auto& port : out.ports()) {
        if (!port.output || port.surface) continue;
        for (int ch = 1; ch <= 16; ++ch) {
            out.send(port.address, packet_of(juce::MidiMessage::allSoundOff(ch)));
            out.send(port.address, packet_of(juce::MidiMessage::allNotesOff(ch)));
            out.send(port.address, packet_of(juce::MidiMessage::pitchWheel(ch, 8192)));
        }
    }
    c.ui.midi_out_at = c.now;
    panics.fetch_add(1, std::memory_order_relaxed);
}

void MidiIo::deliver(Context& c, MidiOutput& out, const MidiOutEvent& e) {
    using K = MidiOutEvent::Kind;
    switch (e.kind) {
    case K::NoteOn:
        if (send_all(c, out, packet_of(juce::MidiMessage::noteOn(e.channel, e.a, juce::uint8(e.b))), e.dest) && e.track < track_count)
            c.ui.track_out_at[e.track] = c.now;
        break;
    case K::NoteOff: send_all(c, out, packet_of(juce::MidiMessage::noteOff(e.channel, e.a)), e.dest); break;
    case K::Control: // a MIDI track's CC or bank select (Stage)
        if (send_all(c, out, packet_of(juce::MidiMessage::controllerEvent(e.channel, e.a, e.b)), e.dest) && e.track < track_count)
            c.ui.track_out_at[e.track] = c.now;
        break;
    case K::Program:
        if (send_all(c, out, packet_of(juce::MidiMessage::programChange(e.channel, e.a)), e.dest) && e.track < track_count)
            c.ui.track_out_at[e.track] = c.now;
        break;
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
    if (c.ui.settings.midi.sync_in == MidiSync::Mtc) {
        follow_mtc(c, mtc.idle(uint64_t(ns_of(now)))); // the quarter frames stopped: Stop
        // Stopped here (Stop, the song's end, Play elsewhere) while the master runs on: wait for it to stop.
        if (mtc_playing && c.host.mtc_following()) mtc_taken = true;
        else if (mtc_playing && mtc_taken && !c.host.mtc_following()) {
            mtc_playing = mtc_taken = false; mtc_held = mtc.running() || mtc.settling();
            mtc_leads.store(false, std::memory_order_relaxed);
        }
    }
    for (unsigned n = 0; n < thru_count; ++n) send_all(c, out, thru[n]);
    thru_count = 0;
    if (panics_done != c.midi_panics) { panics_done = c.midi_panics; panic(c, out); }
    { // the outputs there now, for Edit's DEVICE picture (a handful of ports: cheap every pass)
        uint32_t bits = 0, devices = 0;
        for (const auto& port : out.ports()) {
            if (!port.output || port.surface) continue;
            if (port.driver == "SerialMIDI") bits |= 1;
            else if (port.driver == "MIDI Gadget") bits |= 2;
            else ++devices;
        }
        c.ui.midi_outputs = bits | std::min(devices, 255u) << 8;
    }
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

std::string MidiIo::summary(const EngineHost& host) const {
    char last[16];
    std::snprintf(last, sizeof(last), "%06x", last_in.load());
    // MTC in: 0 idle, 1 settling, 2 running; the rate (2997 drop-frame); the last code read whole; jumps; leading;
    // how far the song is behind the master (filtered) and the pace trim; places (Start, jumps, catch-ups); where the
    // sequencer is (ticks since Play, the song's bar).
    unsigned minutes = 0, seconds = 0, frames = 0, rate = mtc_rate.load();
    mtc_code_of(mtc_code.load(), rate == 24 ? MtcRate::Fps24 : rate == 25 ? MtcRate::Fps25 : rate == 30 ? MtcRate::Fps30 : MtcRate::Fps2997Drop,
                minutes, seconds, frames);
    char code[16];
    std::snprintf(code, sizeof(code), "%02u:%02u:%02u", minutes, seconds, frames);
    std::string mtc_summary = " melodee_mtc=" + std::to_string(mtc_state.load()) + " melodee_mtc_rate=" + std::to_string(rate) +
                              " melodee_mtc_code=" + code + " melodee_mtc_jumps=" + std::to_string(mtc_jumps.load()) +
                              " melodee_mtc_leads=" + std::to_string(int(mtc_leads.load())) + " melodee_mtc_error_us=" +
                              std::to_string(host.mtc_error_us()) + " melodee_mtc_trim_ppm=" + std::to_string(host.mtc_trim_ppm()) +
                              " melodee_mtc_places=" + std::to_string(host.mtc_places()) + " melodee_ticks=" +
                              std::to_string(host.ticks()) + " melodee_song_bar=" + std::to_string(host.song_bar());
    return " melodee_midi_in=" + std::to_string(ins.load()) + " melodee_midi_out=" + std::to_string(outs.load()) +
           " melodee_midi_last=" + last + " melodee_clock_in=" + std::to_string(pulses_in.load()) +
           " melodee_clock_jitter_us=" + std::to_string(jitter_in.load()) + " melodee_clock_bunched=" + std::to_string(bunched.load()) + " melodee_ext=" + std::to_string(int(ext_running.load())) + " melodee_ext_tempo=" + std::to_string(tempo_seen.load()) +
           " melodee_cv_pulses=" + std::to_string(cv_pulses.load()) + " melodee_cv_ok=" + std::to_string(int(cv_ok.load())) +
           " melodee_mtc_in=" + std::to_string(mtc_in.load()) + mtc_summary + " melodee_midi_track_out=" + std::to_string(track_outs.load()) +
           " melodee_midi_last_out=" + last_text(last_out.load()) + " melodee_midi_last_dest=" + std::to_string(last_dest.load()) +
           " melodee_midi_panics=" + std::to_string(panics.load()) + " melodee_midi_late_us=" + std::to_string(late_avg_us.load()) + "/" +
           std::to_string(late_max_us.load());
}

} // namespace melodee
