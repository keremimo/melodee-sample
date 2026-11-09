// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shell.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/page.h"
#include "melodee/pages/pages.h"
#include "surface/controls.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace melodee {
namespace {
using Button = SurfaceButton;
bool play_group(PageId page) { return page == PageId::Stage || page == PageId::Keys || page == PageId::Pads || page == PageId::Mix; }
} // namespace

Shell::Shell(EngineHost& host)
    : host(host), project_(std::make_unique<Project>()), patterns_(std::make_unique<PatternBank>()),
      context_(*project_, *patterns_, ui_, host) {
    starting_project(*project_, *patterns_);
}

void Shell::surface(const SurfaceEvent& event, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    const auto& input = event.input;
    if (event.pad >= 0) {
        unsigned kind = event.midi.status & 0xf0;
        if (kind == 0x90 && event.midi.value) pad(unsigned(event.pad), event.midi.value, true);
        else if (kind == 0x80 || kind == 0x90) pad(unsigned(event.pad), 0, false);
        else if (kind == 0xa0) context_.pressure(unsigned(event.pad), event.midi.value); // to the mod matrix
        return;
    }
    if (input.kind == SurfaceInput::Kind::Button) { button(Button(input.index), input.pressed); return; }
    if (input.kind != SurfaceInput::Kind::Control) return;
    switch (SurfaceControl(input.index)) {
    case SurfaceControl::MainVolume: main_volume.store(main_volume_gain(input.value), std::memory_order_relaxed); break;
    case SurfaceControl::K1: pot(0, input.value, event.syncing); break;
    case SurfaceControl::K2: pot(1, input.value, event.syncing); break;
    case SurfaceControl::K3: pot(2, input.value, event.syncing); break;
    case SurfaceControl::Fader: // the mod wheel of the selected track
        context_.send({Command::Type::Wheel, ui_.track, 0, 0, int16_t(input.value * 1000 / 127)});
        break;
    case SurfaceControl::Wheel: if (input.delta) wheel(input.delta); break;
    default: break;
    }
}

void Shell::button(Button b, bool pressed) {
    auto& ui = ui_;
    if (b == Button::Shift) { ui.shift = pressed; if (pressed) ui.shift_used = false; return; }
    if (b == Button::PadBank) {
        // Held with a pad it picks a track; a tap alone moves a sampler track to its next bank.
        if (pressed) pad_bank_used = false;
        else if (!pad_bank_used && ui.page == PageId::Punch) project_->punch_bank ^= 1; // on Punch: the other bank of punches
        else if (!pad_bank_used && context_.track().engine == EngineKind::Sampler)
            context_.track().bank = uint8_t((context_.track().bank + 1) % sampler_banks);
        ui.pad_bank = pressed;
        return;
    }
    if (b == Button::Mute) { ui.mute = pressed; if (pressed) ui.shift_used |= ui.shift; return; }
    if (b == Button::Erase) {
        ui.erase = pressed;
        if (pressed && ui.shift) { // Shift + Erase: clear the pattern in view
            ui.shift_used = true;
            auto& pattern = context_.pattern();
            for (unsigned s = 0; s < pattern.length; ++s) if (!pattern.steps[s].empty()) context_.set_step(ui.track, s, Step {});
        }
        return;
    }
    if (!pressed) {
        if (b == Button::TapTempo) tap_held = false;
        if (b == Button::NoteRepeat && repeat_turned_on && context_.now - repeat_down > 400) context_.set_arp(false); // it was held
        return;
    }
    if (ui.shift) ui.shift_used = true;

    if (ui.exit_open) {
        // Only the soft keys answer: B1 stays, B3 leaves.
        if (b == Button::TopLeft) ui.exit_open = false;
        if (b == Button::TopRight && leave_after_save < 0) {
            context_.send({Command::Type::Stop});
            context_.send({Command::Type::AllNotesOff, 255});
            // The project goes to the card first (a new file if it has none); then away.
            ProjectStore::View v;
            if (store_) store_->view(v);
            if (!v.root.empty()) { leave_after_save = int(store_->saves()) + 1; context_.save_project(); }
            else leave.store(true);
        }
        return;
    }
    const auto& page = page_spec(ui.page);
    switch (b) {
    case Button::Sample:
        if (ui.shift) { // the projects on the card
            context_.open(PageId::Project);
            if (store_) store_->list(context_.project_folder);
            ui.project_rows = 64; // until the list arrives
            break;
        }
        if (play_group(ui.page)) {
            PageId keys = context_.track().engine == EngineKind::Sampler ? PageId::Pads : PageId::Keys;
            context_.open(ui.page == PageId::Stage ? keys : (ui.page == PageId::Keys || ui.page == PageId::Pads) ? PageId::Mix : PageId::Stage);
        }
        else context_.open(PageId::Stage);
        break;
    case Button::PadFx: // the punches: on the master or the track, or (again, on a sampler track) each pad's own
        context_.open(ui.page == PageId::Punch && context_.track().engine == EngineKind::Sampler ? PageId::PadPunch : PageId::Punch);
        break;
    case Button::KnobFx: // the effects along the signal: track, sends, bus, master
        context_.open(ui.page == PageId::Rack ? PageId::Sends : ui.page == PageId::Sends ? PageId::Bus
                      : ui.page == PageId::Bus ? PageId::Master : PageId::Rack);
        break;
    case Button::Loop: // Patterns, then Song, then Length
        context_.open(ui.page == PageId::Patterns ? PageId::Song : ui.page == PageId::Song ? PageId::Length : PageId::Patterns);
        break;
    case Button::Seq: // Steps, then Roll, then Motion
        context_.open(ui.page == PageId::Steps ? PageId::Roll : ui.page == PageId::Roll ? PageId::Motion : PageId::Steps);
        break;
    case Button::SampleSelect: // on a sampler track it lists samples
        if (!ui.shift) {
            bool kit = context_.track().engine == EngineKind::Sampler;
            context_.open(kit ? PageId::Samples : PageId::Sounds);
            if (kit && library_) library_->rescan();
        }
        break;
    case Button::SampleRecord: // a sampler track: the Record page; there, arm, start now, stop
        if (ui.shift) { // Capture: the last bars played, into a pattern
            ui.capture_into = context_.track().pattern;
            for (unsigned n = 0; n < pattern_count; ++n)
                if (patterns_->at[ui.track][n].empty()) { ui.capture_into = uint8_t(n); break; }
            context_.open(PageId::Capture);
            break;
        }
        if (context_.track().engine != EngineKind::Sampler) break;
        if (ui.page != PageId::Record) context_.open(PageId::Record);
        else context_.record_press();
        break;
    case Button::SixteenLevels: { // a sampler track: the last pad on all sixteen, and again back to the kit
        auto& t = context_.track();
        if (t.engine != EngineKind::Sampler) { context_.open(PageId::Keys); break; }
        if (ui.page == PageId::Levels) { t.levels = false; context_.open(PageId::Pads); break; }
        t.levels = true; t.levels_pad = t.last_pad; ui.levels_hit = -1;
        context_.open(PageId::Levels);
        break;
    }
    case Button::Chop: // the MPC's own job, on the sample of the last pad hit
        if (context_.track().engine != EngineKind::Sampler || !context_.pad().used()) break;
        if (ui.chop_sample != context_.pad().sample) { ui.chop_sample = context_.pad().sample; ui.chop_slice = 0; context_.rechop(); }
        context_.open(PageId::Chop);
        break;
    case Button::Play: context_.play(); break;
    case Button::Stop:
        if (ui.page == PageId::Record) context_.send({Command::Type::RecordStop});
        if (ui.shift) context_.send({Command::Type::AllNotesOff, 255});
        else context_.stop();
        break;
    case Button::SeqRecord: ui.recording = !ui.recording; break;
    case Button::TapTempo:
        if (ui.shift) { context_.set_click(!context_.project.click); break; } // Shift: the metronome
        context_.tap_tempo();
        tap_down = context_.now; tap_held = true; // held, it opens the Tempo page
        break;
    case Button::NoteRepeat: { // the arp (note repeat on a kit): held, for as long; tapped, it stays on
        bool was = context_.track().arp;
        if (!was) context_.set_arp(true);
        repeat_down = context_.now; repeat_turned_on = !was;
        if (ui.page != PageId::Arp && ui.page != PageId::Chord) context_.open(PageId::Arp);
        else if (was) context_.set_arp(false); // a tap on the page turns it off
        break;
    }
    case Button::Minus: page.nudge(context_, -1); break;
    case Button::Plus: page.nudge(context_, 1); break;
    case Button::TopLeft: page.soft(context_, 0); break;
    case Button::TopMiddle: page.soft(context_, 1); break;
    case Button::TopRight: page.soft(context_, 2); break;
    case Button::WheelPush:
        if (ui.shift && ui.page == PageId::Song && !ui.sheet_open) song_marker(context_);
        else if (ui.shift && ui.page == PageId::Project && !ui.sheet_open && !context_.project_folder.empty()) {
            // up a folder
            auto& f = context_.project_folder;
            auto cut = f.find_last_of('/');
            f = cut == std::string::npos ? std::string() : f.substr(0, cut);
            ui.project_cursor = 0;
            if (store_) store_->list(f);
        }
        else if (ui.shift) context_.back();
        else page.push(context_);
        break;
    default: break;
    }
}

// A pot changes its value once it reaches it (pickup); until then the card
// shows the value dim with a tick where the pot sits.
void Shell::check_pickup(unsigned n, const Binding& binding) {
    auto& pot = ui_.pots[n];
    uint32_t key = binding.key(ui_.track);
    if (pot.bound == key) return;
    pot.bound = key;
    pot.engaged = pot.position >= 0 && std::abs(pot.position - int(binding.fraction() * 127 + 0.5f)) <= 2;
}

void Shell::pot(unsigned n, int position, bool syncing) {
    auto bindings = page_bindings(*project_, ui_, context_.pattern());
    const auto& b = bindings[n];
    auto& pot = ui_.pots[n];
    int before = pot.position;
    pot.position = int16_t(position);
    if (syncing) { pot.bound = 0; check_pickup(n, b); return; }
    if (!b.bound()) return;
    check_pickup(n, b);
    ui_.turning = int8_t(n); ui_.turned_at = context_.now; ui_.last_pot = int8_t(n);
    if (ui_.shift) ui_.shift_used = true;
    if (ui_.erase && b.kind == Binding::Kind::Param && ui_.page == PageId::Motion) { // Erase + pot on Motion: clear the lane
        context_.clear_lane(b.id);
        return;
    }
    if (ui_.erase && b.kind == Binding::Kind::Param) { // Erase + pot: back to the initial value
        context_.apply(b, engine_info(context_.track().engine).params[b.id].initial);
        pot.engaged = false; pot.bound = 0;
        return;
    }
    if (!pot.engaged) {
        int target = int(b.fraction() * 127 + 0.5f);
        bool crossed = before >= 0 && ((before <= target && position >= target) || (before >= target && position <= target));
        if (before < 0 || crossed || std::abs(position - target) <= 2) pot.engaged = true;
    }
    if (!pot.engaged) return;
    int value = b.at(position);
    if (value != b.value) context_.apply(b, value);
}

void Shell::wheel(int delta) {
    if (ui_.exit_open) return;
    if (ui_.shift && ui_.last_pot >= 0) {
        // Shift + wheel: fine edit of the last pot touched, one step a detent.
        ui_.shift_used = true;
        auto bindings = page_bindings(*project_, ui_, context_.pattern());
        const auto& b = bindings[size_t(ui_.last_pot)];
        if (!b.bound()) return;
        context_.apply(b, b.value + delta);
        auto& pot = ui_.pots[size_t(ui_.last_pot)];
        pot.engaged = false; pot.bound = 0; // the value left the pot: pick it up again
        ui_.turning = ui_.last_pot; ui_.turned_at = context_.now;
        return;
    }
    page_spec(ui_.page).wheel(context_, delta);
}

void Shell::pad(unsigned pad, uint8_t velocity, bool down) {
    if (down) ui_.held_pads |= 1u << pad; else ui_.held_pads &= ~(1u << pad);
    if (down && !ui_.exit_open) {
        if (ui_.pad_bank) { pad_bank_used = true; context_.select_track(pad); return; } // Pad Bank + pad: pick that track
        if (ui_.mute) {                                           // Mute + pad: mute; with Shift, solo
            const auto& mix = project_->tracks[pad].mix;
            if (ui_.shift) context_.set_solo(pad, !mix.solo);
            else context_.set_mute(pad, !mix.mute);
            return;
        }
    }
    if (down && ui_.exit_open) return;
    page_spec(ui_.page).pad(context_, pad, velocity, down);
}

void Shell::snapshot(Snapshot& out, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    auto bindings = page_bindings(*project_, ui_, context_.pattern());
    for (unsigned n = 0; n < 3; ++n) if (bindings[n].bound()) check_pickup(n, bindings[n]);
    out.ui = ui_;
    out.project = *project_;
    out.pattern = context_.pattern();
    for (unsigned t = 0; t < track_count; ++t)
        out.has_notes[t] = !patterns_->at[t][project_->tracks[t].pattern].empty();
    out.playing = host.playing();
    for (unsigned t = 0; t < track_count; ++t) {
        out.steps[t] = host.step_of(t);
        out.playing_patterns[t] = uint8_t(host.pattern_of(t));
    }
    out.position = unsigned(std::max(out.steps[ui_.track], 0));
    out.ticks = host.ticks();
    {
        const auto& r = host.recorder();
        out.take.state = uint8_t(r.state()); out.take.frames = r.frames(); out.take.length = r.length(); out.take.level = r.level();
        for (unsigned c = 0; c < out.take.peaks.size(); ++c) out.take.peaks[c] = r.peak(c);
    }
    if (ui_.page == PageId::Capture) { out.history = context_.history; out.history_at = context_.history_at; }
    for (unsigned n = 0; n < 16; ++n) out.held_keys[n] = context_.pad_keys[n][0];
    out.project_path = context_.project_path; out.project_folder = context_.project_folder; out.dirty = context_.dirty;
    if (store_ && (ui_.page == PageId::Project || ui_.exit_open)) store_->view(out.projects);
    if (library_) {
        const auto& t = project_->tracks[ui_.track];
        unsigned slot = ui_.page == PageId::Chop ? ui_.chop_sample : t.pads[t.last_pad % sampler_pads].sample;
        out.sample = slot < sample_slots ? library_->slot(slot) : LoadedSample {};
        if (ui_.page == PageId::Samples) { out.files = library_->files(); out.sample_root = library_->root(); }
    }
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned n = 0; n < pattern_count; ++n) {
            const auto& pattern = patterns_->at[t][n];
            auto& sketch = out.sketches[t][n];
            sketch.length = pattern.length; sketch.division = pattern.division; sketch.notes = false;
            for (unsigned k = 0; k < sketch.keys.size(); ++k) {
                const Note* note = k < pattern.length ? pattern.steps[k].first() : nullptr;
                sketch.keys[k] = note ? note->key : 0;
                sketch.notes |= note != nullptr;
            }
            if (!sketch.notes) sketch.notes = !pattern.empty(); // notes beyond step 64
        }
    out.song_playing = host.song_playing();
    out.song_bar = host.song_bar();
    for (unsigned t = 0; t < track_count; ++t) out.meters[t] = host.meter(t);
    out.now = now;
}

std::string Shell::summary() const {
    uint32_t pt = page_track.load(std::memory_order_relaxed);
    return "melodee_page=" + std::to_string(pt & 0xff) + " melodee_track=" + std::to_string((pt >> 8) + 1) +
           " melodee_pad_sample=" + std::to_string(int(pad_sample.load()) == 0xffff ? -1 : int(pad_sample.load())) +
           " melodee_pad_frames=" + std::to_string(pad_frames.load()) + " melodee_tempo=" + std::to_string(tempo_now.load()) +
           " melodee_punch=" + std::to_string(int(host.punching()));
}

void starting_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Untitled");
    project.tempo = 1200;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);  // FUNK BASS II
    load_sound(project.tracks[1], EngineKind::Prophet, 5);  // SOFT PAD
    load_sound(project.tracks[2], EngineKind::Prophet, 6);  // GLASS PLUCK
    load_sound(project.tracks[3], EngineKind::Prophet, 3);  // SYNC LEAD
    load_sound(project.tracks[4], EngineKind::Sampler, 0); // an empty kit: Sample Select fills its pads
    load_sound(project.tracks[5], EngineKind::Kit909, 0);  // drums, with a beat
    project.tracks[1].mix.level = 80; project.tracks[1].octave = 3;
    project.tracks[2].octave = 4; project.tracks[3].octave = 4;
    patterns = PatternBank {};
    // A bass line in D for track 1, so Play has something to play.
    struct { unsigned step; uint8_t key, length, velocity; } line[] {
        {0, 38, 2, 110}, {3, 38, 1, 70}, {4, 41, 1, 96}, {6, 36, 2, 96}, {8, 33, 3, 120},
        {11, 36, 1, 64}, {12, 38, 2, 104}, {14, 31, 2, 88},
    };
    for (auto& n : line) {
        auto& step = patterns.at[0][0].steps[n.step];
        step.notes[0] = {n.key, n.velocity};
        step.length = n.length;
    }
    // Track 6: kick on the beat, snare on two and four, closed hats on the off-beats.
    auto& beat = patterns.at[5][0];
    for (unsigned st = 0; st < 16; st += 4) beat.steps[st].notes[0] = {36, 120};
    for (unsigned st : {4u, 12u}) beat.steps[st].notes[1] = {37, 110};
    for (unsigned st = 2; st < 16; st += 4) beat.steps[st].notes[0] = {43, 90};
}

} // namespace melodee
