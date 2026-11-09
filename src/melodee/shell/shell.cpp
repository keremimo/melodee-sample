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
bool play_group(PageId page) { return page == PageId::Stage || page == PageId::Keys || page == PageId::Mix; }
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
    if (b == Button::PadBank) { ui.pad_bank = pressed; return; }
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
    if (!pressed) return;
    if (ui.shift) ui.shift_used = true;

    if (ui.exit_open) {
        // Only the soft keys answer: B1 stays, B3 leaves.
        if (b == Button::TopLeft) ui.exit_open = false;
        if (b == Button::TopRight) {
            context_.send({Command::Type::Stop});
            context_.send({Command::Type::AllNotesOff, 255});
            leave.store(true);
        }
        return;
    }
    const auto& page = page_spec(ui.page);
    switch (b) {
    case Button::Sample:
        if (ui.shift) break; // Project and Settings come with their own boards
        if (play_group(ui.page)) context_.open(ui.page == PageId::Stage ? PageId::Keys : ui.page == PageId::Keys ? PageId::Mix : PageId::Stage);
        else context_.open(PageId::Stage);
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
    case Button::SampleSelect: if (!ui.shift) context_.open(PageId::Sounds); break;
    case Button::SixteenLevels: context_.open(PageId::Keys); break;
    case Button::Play: context_.play(); break;
    case Button::Stop:
        if (ui.shift) context_.send({Command::Type::AllNotesOff, 255});
        else context_.stop();
        break;
    case Button::SeqRecord: ui.recording = !ui.recording; break;
    case Button::TapTempo: context_.tap_tempo(); break;
    case Button::Minus: page.nudge(context_, -1); break;
    case Button::Plus: page.nudge(context_, 1); break;
    case Button::TopLeft: page.soft(context_, 0); break;
    case Button::TopMiddle: page.soft(context_, 1); break;
    case Button::TopRight: page.soft(context_, 2); break;
    case Button::WheelPush:
        if (ui.shift && ui.page == PageId::Song && !ui.sheet_open) song_marker(context_);
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
        if (ui_.pad_bank) { context_.select_track(pad); return; } // Pad Bank + pad: pick that track
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

void starting_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "New project");
    project.tempo = 1200;
    load_sound(project.tracks[0], EngineKind::Prophet, 1);  // FUNK BASS II
    load_sound(project.tracks[1], EngineKind::Prophet, 5);  // SOFT PAD
    load_sound(project.tracks[2], EngineKind::Prophet, 6);  // GLASS PLUCK
    load_sound(project.tracks[3], EngineKind::Prophet, 3);  // SYNC LEAD
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
}

} // namespace melodee
