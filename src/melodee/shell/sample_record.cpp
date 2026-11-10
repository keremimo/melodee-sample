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
// - Audio Recall (Shift + Sample Record): the ring's last 25 s onto the next
//   empty pad, written by the service thread.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/context.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"

namespace melodee {
using Type = Command::Type;
using State = TakeRecorder::State;

namespace {
// RecordArm's rec length: 0 Free, 1..16 bars, 255 Seq.
uint8_t length_code(const InputConfig& in) {
    return in.length == rec_free ? 0 : in.length == rec_seq ? 255 : uint8_t(rec_bars(in.length));
}

bool arm_take(Context& c, unsigned kit) {
    auto& t = c.track();
    if (t.engine != EngineKind::Sampler) { c.notice("Sample Record: kit tracks"); return false; }
    kit %= sampler_pads;
    int slot = c.hold_file("TAKE", c.ui.track, int(kit), 2);
    if (slot < 0) { c.notice("Record: samples full (64)"); return false; }
    c.ui.rec_slot = int16_t(slot); c.ui.rec_pad = int16_t(kit); c.ui.rec_track = c.ui.track; c.ui.rec_chops = 0;
    c.rec_armed_at = c.now;
    t.last_pad = uint8_t(kit);
    const auto& in = c.ui.settings.input;
    Command arm {Type::RecordArm, uint8_t(in.source), length_code(in), uint16_t(slot), int16_t(in.threshold)};
    arm.position = c.ui.track;
    c.send(arm);
    bool cued = in.length == rec_seq && c.host.playing();
    c.notice(pad_text(cued ? "Next loop, to" : in.at_once() ? "Recording to" : "Waiting, to", kit));
    return true;
}
} // namespace

void Context::set_input_source(InputSource source) {
    if (host_recording()) { notice("Source stays while recording"); return; }
    ui.settings.input.source = source;
    input_changed();
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
    send({Type::RecordSource, uint8_t(in.source)});
    send({Type::RecordGain, 0, 0, 0, int16_t(in.gain)});
    gain_sent = in.gain;
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
    if (host.recall().filled() < 441) { notice("Audio Recall: nothing yet"); return; }
    int kit = next_empty_pad(t.last_pad);
    if (kit < 0) { notice("No empty pad"); return; }
    int slot = hold_file("RECALL", ui.track, kit, 1);
    if (slot < 0) { notice("Audio Recall: samples full"); return; }
    if (!library->recall(unsigned(slot))) { drop_file(slot); notice("Audio Recall: busy"); return; }
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
    if (input) input->want(in.source, in.mutes_speaker(page));
    // The engine hears the settings as loaded from the card (and after a take, a source changed meanwhile).
    if (host.recorder().state() == State::Idle && host.record_source() != unsigned(in.source)) send({Type::RecordSource, uint8_t(in.source)});
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
