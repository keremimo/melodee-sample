// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Input Config, Sample Record and Audio Recall (parity phase 6), on the
// MIDI thread, after Akai's sample_record_mode and input_configuration
// pages:
//
// - Input Config (Shift + Sample) and Record share K1 source (Mic, Rear,
//   Rear L / R, Resample, USB, USB L / R), K2 rec length (Free, Seq,
//   Melodee's 1-16 bars), K3 threshold (-96 dB: at once) and B1 monitor
//   (Off, Auto: while Record is in view, On); Shift + K3 is Melodee's gain.
//   Input Config adds B2 Rec Input FX. They are kept with Melodee's
//   settings, a second after the last change.
// - Record: a pad records onto itself (after the threshold, or with Seq
//   while playing where the pattern comes round); during the take the other
//   pads tapped cut it there, each getting the part from its cut on; the
//   pad again, Stop or Push ends it and stays; Sample Record ends it and
//   goes back to the pad's Sample page.
// - A Track source (or Resample) on Record: the tapped pad is the take's;
//   from then until it ends the pads play the instrument (record_key), and
//   a Free take ends on its own after the Settings page's quiet stretch.
// - Audio Recall (Shift + Sample Record): the ring's last 25 s onto the next
//   empty pad, cut as the Settings page says (recall_bounds), written by the
//   service thread.
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include "melodee/shell/context.h"
#include "melodee/shell/input_choices.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cstdio>

namespace melodee {
using Type = Command::Type;
using State = TakeRecorder::State;

namespace {
// RecordArm's rec length: 0 Free, 1..16 bars, 255 Seq.
uint8_t length_code(const InputConfig& in) {
    return in.length == rec_free ? 0 : in.length == rec_seq ? 255 : uint8_t(rec_bars(in.length));
}

// RecordSource for the input as set: with a track, which one and its tap.
Command source_command(const InputConfig& in) {
    return {Type::RecordSource, uint8_t(in.source), 0, uint16_t(in.track % track_count), int16_t(in.post)};
}

bool arm_take(Context& c, unsigned kit) {
    auto& t = c.track();
    if (t.engine != EngineKind::Sampler) { c.notice("Sample Record: kit tracks"); return false; }
    const auto& in = c.ui.settings.input;
    unsigned source = in.track % track_count;
    if (in.source == InputSource::Track) { // a track records onto a kit pad: another track, with a sound
        char text[40];
        if (source == c.ui.track) { c.notice("Source: pick another track"); return false; }
        if (!sounding_track(c.project.tracks[source])) {
            std::snprintf(text, sizeof(text), "Source: track %u has no sound", source + 1);
            c.notice(text);
            return false;
        }
    }
    kit %= sampler_pads;
    int slot = c.hold_file("TAKE", c.ui.track, int(kit), 2);
    if (slot < 0) { c.notice("Record: samples full (64)"); return false; }
    c.ui.rec_slot = int16_t(slot); c.ui.rec_pad = int16_t(kit); c.ui.rec_track = c.ui.track; c.ui.rec_chops = 0;
    c.rec_armed_at = c.now;
    t.last_pad = uint8_t(kit);
    // Free: the take ends on its own after a quiet stretch, where the Settings page says so for this source.
    const auto& sp = c.ui.settings.sampling;
    Command quiet {Type::RecordAutoEnd, 0, 0, 0, int16_t(sp.silence_db())};
    if (in.length == rec_free && sp.auto_ends(in.source))
        quiet.position = uint32_t(SamplingPrefs::end_seconds[sp.end_after % SamplingPrefs::end_count] * float(dsp::model_rate));
    c.send(quiet);
    c.send(source_command(in));
    Command arm {Type::RecordArm, uint8_t(in.source), length_code(in), uint16_t(slot), int16_t(in.threshold)};
    arm.position = in.source == InputSource::Track ? source : c.ui.track; // Seq: the source track's pattern
    c.send(arm);
    bool cued = in.length == rec_seq && c.host.playing();
    c.notice(pad_text(cued ? "Next loop, to" : in.at_once() ? "Recording to" : "Waiting, to", kit));
    return true;
}
} // namespace

void Context::set_input_source(InputSource source, unsigned track) {
    if (host_recording()) { notice("Source stays while recording"); return; }
    ui.settings.input.source = source;
    ui.settings.input.track = uint8_t(track % track_count);
    input_changed();
}

void Context::set_input_tap(bool post) {
    if (host_recording()) { notice("Tap stays while recording"); return; }
    ui.settings.input.post = post;
    input_changed();
}

bool Context::record_keys() const {
    const auto& in = ui.settings.input;
    if (ui.page != PageId::Record || !internal_source(in.source)) return false;
    if (ui.rec_slot < 0 || ui.rec_track != ui.track) return false;
    auto state = host.recorder().state();
    return state == State::Armed || state == State::Cued || state == State::Recording;
}

// A pad while a take of Melodee's own sound runs: the source track plays it as
// if it were selected (its layout, scale, chord, arp, levels; never into its
// pattern), or with Resample the kit in view.
void Context::record_key(unsigned pad, uint8_t velocity, bool down) {
    pad %= 16;
    const auto& in = ui.settings.input;
    uint8_t keep_track = ui.track;
    bool keep_recording = ui.recording;
    if (in.source == InputSource::Track) ui.track = uint8_t(in.track % track_count);
    ui.recording = false;
    play_pad(pad, velocity, down);
    ui.track = keep_track;
    ui.recording = keep_recording;
    if (down) record_key_pads = uint16_t(record_key_pads | 1u << pad);
    else record_key_pads = uint16_t(record_key_pads & ~(1u << pad));
}

void Context::record_octave(int way) {
    auto& t = project.tracks[ui.settings.input.track % track_count];
    t.octave = int8_t(std::clamp(t.octave + way, 0, 7));
    notice(std::string("Pads ") + note_name(pad_note(project, t, 0).key) + "\xe2\x80\x93" + note_name(pad_note(project, t, 15).key));
}

void Context::cycle_monitor() {
    auto& in = ui.settings.input;
    in.monitor = Monitor((unsigned(in.monitor) + 1) % unsigned(Monitor::Count));
    input_changed();
    notice(std::string("Monitor ") + monitor_name(in.monitor));
}

void Context::toggle_rec_fx() {
    auto& in = ui.settings.input;
    in.fx = !in.fx;
    input_changed();
    notice(in.fx ? "Rec Input FX on" : "Rec Input FX off");
}

void Context::input_changed() {
    auto& in = ui.settings.input;
    in.clamp();
    send(source_command(in));
    send({Type::RecordGain, 0, 0, 0, int16_t(in.gain)});
    gain_sent = in.gain;
    send_knob_state(); // Rec Input FX: Knob FX on the input during a take
    settings_save_at = std::max<uint32_t>(now, 1) + 1000; // pots move in many steps: the card gets the last
}

void Context::record_pad(unsigned kit) {
    kit %= sampler_pads;
    auto state = host.recorder().state();
    if (state == State::Idle && ui.rec_slot < 0) { arm_take(*this, kit); return; }
    if (ui.rec_pad < 0) return;
    if (state == State::Armed || state == State::Cued) { // the pad again: at once
        if (kit == unsigned(ui.rec_pad)) send({Type::RecordNow});
        else notice(pad_text("Recording to", unsigned(ui.rec_pad)));
        return;
    }
    if (state != State::Recording) return; // saving
    if (kit == unsigned(ui.rec_pad)) { send({Type::RecordStop}); return; } // the pad again: done, still here
    if (ui.track != ui.rec_track) { notice("Cut: the take's track"); return; }
    for (unsigned n = 0; n < ui.rec_chops; ++n) if (ui.rec_chop_pad[n] == kit) return; // cut there already
    if (ui.rec_chops >= UiState::rec_chop_capacity) { notice("Cut: 16 pads"); return; }
    ui.rec_chop_pad[ui.rec_chops] = uint8_t(kit);
    ui.rec_chop_at[ui.rec_chops] = host.recorder().frames();
    ++ui.rec_chops;
    notice(pad_text("Cut to", kit));
}

// Push on Record (Melodee's): onto the first empty pad; then at once; then done.
void Context::record_press() {
    auto state = host.recorder().state();
    if (state == State::Armed || state == State::Cued) { send({Type::RecordNow}); return; }
    if (state == State::Recording) { send({Type::RecordStop}); return; }
    if (state != State::Idle || ui.rec_slot >= 0) return;
    int kit = next_empty_pad(sampler_pads - 1);
    if (kit < 0) { notice("No empty pad"); return; }
    arm_take(*this, unsigned(kit));
}

// Sample Record on Record: the take ends (if one runs) and the page goes
// back to Sample Mode with its pad in hand, as Akai's guide has it.
void Context::record_button() {
    auto state = host.recorder().state();
    int kit = ui.rec_pad >= 0 && ui.rec_track == ui.track ? ui.rec_pad : track().last_pad;
    if (state == State::Armed || state == State::Cued || state == State::Recording) send({Type::RecordStop});
    if (track().engine != EngineKind::Sampler) return;
    track().last_pad = uint8_t(kit % int(sampler_pads));
    open(PageId::Trim);
}

void Context::audio_recall() {
    auto& t = track();
    if (t.engine != EngineKind::Sampler) { notice("Audio Recall: kit tracks"); return; }
    if (!library) return;
    if (host.recall().filled() < host.recall().rate() / 100) { notice("Audio Recall: nothing yet"); return; }
    int kit = next_empty_pad(t.last_pad);
    if (kit < 0) { notice("No empty pad"); return; }
    int slot = hold_file("RECALL", ui.track, kit, 1);
    if (slot < 0) { notice("Audio Recall: samples full"); return; }
    const auto& sp = ui.settings.sampling;
    RecallOptions options;
    options.grab = uint8_t(sp.grab);
    options.gap = SamplingPrefs::gap_seconds[sp.gap % SamplingPrefs::gap_count];
    options.trim_end = sp.trim_end;
    options.silence = sp.silence_level();
    if (!library->recall(unsigned(slot), options)) { drop_file(slot); notice("Audio Recall: busy"); return; }
    notice(pad_text("Audio Recall to", unsigned(kit)));
}

void Context::record_tick(uint32_t at) {
    const auto& in = ui.settings.input;
    bool page = ui.page == PageId::Record;
    // Monitor: Auto hears the source only while Record is in view.
    bool monitoring = in.monitoring(page);
    if (!monitor_known || monitoring != monitor_sent) {
        send({Type::Monitor, 0, 0, 0, int16_t(monitoring)});
        monitor_sent = monitoring; monitor_known = true;
    }
    // Loops on a hardware input of their own: routed too, heard as Monitor says (the mic quiets the speaker then).
    if (loop_monitor_sent != int(in.monitor)) { send({Type::LoopMonitor, 0, 0, 0, int16_t(in.monitor)}); loop_monitor_sent = int(in.monitor); }
    unsigned loops = loop_input_needs(project.looper);
    bool loop_mic = (loops & InputRoute::need_mic) && in.monitor != Monitor::Off;
    if (input) input->want(in.source, in.mutes_speaker(page) || loop_mic, loops);
    // The engine hears the settings as loaded from the card (and after a take, a source changed meanwhile).
    if (host.recorder().state() == State::Idle
        && (host.record_source() != unsigned(in.source)
            || (in.source == InputSource::Track && (host.record_track() != in.track % track_count || host.record_post() != in.post))))
        send(source_command(in));
    // The take ended (or Record was left): what the pads played on the source lets go.
    if (record_key_pads && !record_keys())
        for (unsigned pad = 0; pad < 16; ++pad)
            if (record_key_held(pad)) record_key(pad, 0, false);
    if (gain_sent != in.gain) { send({Type::RecordGain, 0, 0, 0, int16_t(in.gain)}); gain_sent = in.gain; }
    if (settings_save_at && at >= settings_save_at) {
        settings_save_at = 0;
        if (store) store->save_settings(ui.settings.text());
    }
    // A take armed and let go before it recorded: its slot is free again.
    if (ui.rec_slot >= 0 && host.recorder().state() == State::Idle && host.recorder().frames() == 0 && at - rec_armed_at > 100) {
        drop_file(ui.rec_slot);
        ui.rec_slot = ui.rec_pad = -1;
        ui.rec_chops = 0;
    }
    render_tick(at);
}

} // namespace melodee
