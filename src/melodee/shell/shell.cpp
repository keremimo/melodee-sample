// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shell.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/input_route.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/common.h"
#include "melodee/pages/page.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/fader.h"
#include <chrono>
#include "melodee/shell/takeover.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include <cstdio>
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/shift_pads.h"
#include "surface/controls.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace melodee {
namespace {
using Button = SurfaceButton;
} // namespace

Shell::Shell(EngineHost& host)
    : host(host), project_(std::make_unique<Project>()), patterns_(std::make_unique<PatternBank>()),
      context_(*project_, *patterns_, ui_, host) {
    starting_project(*project_, *patterns_);
    context_.card = &card_link;
}

void Shell::midi(const MidiPortInfo& port, const MidiPacket& packet, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    midi_io.receive(context_, port, packet);
}

void Shell::looper_ask(const LooperAsk& ask, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    looper_remote(context_, ask);
}

void Shell::midi_panic(uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    context_.midi_panic();
}

void Shell::midi_lost() {
    std::lock_guard<std::mutex> guard(lock);
    midi_io.lost(context_);
}

void Shell::midi_output(MidiOutput& out, MidiIo::Clock::time_point at, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    midi_io.pump(context_, out, at);
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
    // SD card access: the card is on a computer; only the main volume still turns (as the MPC, other modes wait).
    if (card_owns_controls(ui_) && SurfaceControl(input.index) != SurfaceControl::MainVolume) return;
    switch (SurfaceControl(input.index)) {
    case SurfaceControl::MainVolume: main_volume.store(main_volume_gain(input.value), std::memory_order_relaxed); break;
    case SurfaceControl::K1: pot(0, input.value, event.syncing); break;
    case SurfaceControl::K2: pot(1, input.value, event.syncing); break;
    case SurfaceControl::K3: pot(2, input.value, event.syncing); break;
    case SurfaceControl::Fader: fader_move(context_, input.value, event.syncing); break; // shell/fader.cpp: mod wheel, or the page's own job
    case SurfaceControl::Wheel: if (input.delta) wheel(input.delta); break;
    default: break;
    }
}

// Each press on a page is one undo step of what the page edits, unless it kept
// an edit of its own (parity phase 10); Shift and undo / redo themselves are not.
void Shell::button(Button b, bool pressed) {
    if (card_owns_controls(ui_)) { // SD card access (shell/card_access.h): B1, the question's B3, Play and Stop
        if (!pressed) { if (b == Button::Shift) ui_.shift = false; if (b == Button::Play || b == Button::Stop) press(b, pressed); return; }
        auto r = card_press(ui_, b);
        if (r.request != CardRequest::None) card_link.request.store(uint8_t(r.request));
        if (r.leave) context_.open(PageId::Project);
        if (!r.taken) press(b, pressed);
        return;
    }
    if (!pressed || b == Button::Shift || ((b == Button::Minus || b == Button::Plus) && ui_.shift)) { press(b, pressed); return; }
    PageEdit step(context_, b, false);
    press(b, pressed);
    step.finish();
}

void Shell::press(Button b, bool pressed) {
    auto& ui = ui_;
    if (b == Button::Shift) { ui.shift = pressed; if (pressed) ui.shift_used = false; return; }
    if (b == Button::Sample && !pressed) { sample_button(context_, false); return; } // always heard, so Sample is never left held
    if (pressed && ask_button(context_, b)) return; // a question open (shell/ask.cpp): B1 Cancel, B3 Do it
    if (ui.name.open()) { // the Name sheet takes the controls it uses; Play and Stop still work
        if (!pressed) return;
        switch (b) {
        case Button::TopLeft: context_.cancel_name(); return;
        case Button::TopMiddle: if (!ui.name.caps_only) ui.name.lower = !ui.name.lower; return;
        case Button::TopRight: case Button::WheelPush: context_.finish_name(); return;
        case Button::Erase: if (ui.shift) { ui.shift_used = true; ui.name.clear(); } else ui.name.erase(); return;
        case Button::Minus: ui.name.move(-1); return;
        case Button::Plus: ui.name.move(1); return;
        case Button::Play: case Button::Stop: break;
        default: return;
        }
    }
    if (b == Button::Seq && (!ui.exit_open || !pressed)) { seq_press(context_, pressed); return; } // tap, hold or Shift (shell/seq_layer.cpp)
    if (pressed && !ui.exit_open && seq_button(context_, b)) return; // Seq held: B1-B3, Minus / Plus, Seq Record, Push
    if (b == Button::PadBank) { pad_bank_button(context_, pressed); return; } // shell/pad_bank.cpp
    if (b == Button::Mute) { mute_button(context_, pressed); return; }         // shell/mute_mode.cpp
    if (b == Button::NoteRepeat && !ui.exit_open) { // shell/note_repeat.cpp; on Song the pads play nothing: it repeats the clip
        if (pressed && song_buttons()) song_repeat(context_);
        else repeat_button(context_, pressed);
        return;
    }
    if (b == Button::Sample) { if (!ui.exit_open) sample_button(context_, true); return; } // shell/sample_mode.cpp: held, a pad picks a kit pad
    if (b == Button::Loop && (!ui.exit_open || !pressed)) { loop_press(context_, pressed); return; } // twice: the Looper; held: the loop grid (shell/looper_jobs.cpp)
    if (b == Button::Erase) {
        ui.erase = pressed;
        if (!pressed) { // tapped on its own: on Roll the note in focus goes (one undo)
            if (!ui.erase_used && ui.page == PageId::Roll && !ui.sheet_open && ui.ask == UiState::Ask::None) roll_erase(context_);
            return;
        }
        ui.erase_used = ui.shift || ui.copying != UiState::Copying::Off; // Copy, or leaving it, is the press's job
        if (ui.shift) { // Shift + Erase: Copy, as printed: tracks with Pad Bank held, patterns with Seq held, kit pads on a kit (again: off)
            ui.shift_used = true;
            if (ui.pad_bank) ui.pad_bank_used = true;
            if (ui.seq_held) ui.seq_used = true;
            auto kind = ui.pad_bank ? UiState::Copying::Tracks : ui.seq_held ? UiState::Copying::Patterns
                      : kit_pads() ? UiState::Copying::Pads : UiState::Copying::Off;
            if (ui.copying != UiState::Copying::Off) { ui.stop_copying(); context_.notice("Copy off"); }
            else if (kind == UiState::Copying::Off) context_.notice("Copy: hold Seq or Pad Bank");
            else { ui.copying = kind; ui.copy_from = -1; }
        } else if (ui.copying != UiState::Copying::Off) { // Erase alone leaves copying
            ui.stop_copying();
            context_.notice("Copy off");
        } else if (song_buttons()) {
            song_erase(context_); // the clip under the cursor, one undo
        }
        return;
    }
    if (!pressed) {
        if (b == Button::TapTempo) tap_held = false;
        return;
    }
    if (ui.shift) ui.shift_used = true;

    if (ui.exit_open) {
        // Only the soft keys answer: B1 stays (even while a save to leave after is on its way), B2 turns
        // the MPC off, B3 leaves for the stock app.
        if (b == Button::TopLeft) { ui.exit_open = ui.exit_off = ui.exit_saved = false; leave_after_save = -1; off_after_save = false; }
        if ((b == Button::TopMiddle || b == Button::TopRight) && leave_after_save < 0) {
            off_after_save = ui.exit_off = b == Button::TopMiddle;
            context_.send({Command::Type::Stop});
            context_.send({Command::Type::AllNotesOff, 255});
            // The project goes to the card first (a new file if it has none); then away.
            ProjectStore::View v;
            if (store_) store_->view(v);
            if (!v.root.empty()) { leave_after_save = int(store_->saves()) + 1; context_.save_project(); }
            else if (off_after_save) { ui.exit_saved = true; off_saved_at = context_.now; } // nothing to save: the second, then off
            else leave.store(true);
        }
        return;
    }
    const auto& page = page_spec(ui.page);
    switch (b) {
    case Button::PadFx: // Pad FX: the punches, or (again, on a sampler track) each pad's own; Shift: Flex Beat (as printed)
        if (ui.shift) { context_.open(PageId::FlexBeat); break; }
        context_.open(ui.page == PageId::Punch && context_.track().engine == EngineKind::Sampler ? PageId::PadPunch : PageId::Punch);
        break;
    case Button::KnobFx:
        if (!ui.shift) { context_.knob_fx_button(); break; } // Knob FX, as on the MPC: the mode on or off (shell/knob_fx.cpp)
        // Shift + Knob FX (FX SELECT): Melodee's effects along the signal, track, sends, bus, master;
        // on the Looper the selected loop track's chain
        if (ui.page == PageId::Looper && context_.project.looper.on) { int8_t l = int8_t(ui.loop % loop_tracks); context_.open(PageId::Rack); ui.fx_loop = l; break; }
        context_.open(ui.page == PageId::Rack && ui.fx_loop < 0 ? PageId::Sends : ui.page == PageId::Sends ? PageId::Bus
                      : ui.page == PageId::Bus ? PageId::Master : PageId::Rack);
        break;
    case Button::SampleSelect: // on a sampler track it lists samples
        if (ui.shift) { // by engine: on Sounds a star; on Samples Save Bank; a kit: Save Sample; a synth: Save sound
            if (ui.page == PageId::Sounds) context_.toggle_favourite();
            else if (ui.page == PageId::Samples && context_.track().engine == EngineKind::Sampler) context_.save_bank();
            else if (context_.track().engine == EngineKind::Sampler) context_.save_sample();
            else context_.save_sound();
            break;
        }
        if (!ui.shift) {
            bool kit = context_.track().engine == EngineKind::Sampler;
            context_.open(kit ? PageId::Samples : PageId::Sounds);
            if (kit && library_) library_->rescan();
        }
        break;
    case Button::SampleRecord: // a sampler track: the Record page; there, a take ends and Sample Mode has its pad
        if (ui.shift) { context_.audio_recall(); break; } // Audio Recall: the input ring's last 25 s
        if (context_.track().engine != EngineKind::Sampler) { context_.notice("Sample Record: kit tracks"); break; }
        if (ui.page != PageId::Record) context_.open(PageId::Record);
        else context_.record_button();
        break;
    case Button::SixteenLevels: levels_button(context_); break; // the last pad (or note) on all sixteen; Shift: the type
    case Button::Chop: if (song_buttons()) song_chop(context_); else chop_button(context_); break; // Chop on the last pad's sample; Shift: Note On; on Song the clip in two
    case Button::Play: if (ui.shift) context_.play_continue(); else context_.play(); break;
    case Button::Stop:
        if (ui.page == PageId::Record) context_.send({Command::Type::RecordStop}); // the take ends; Record stays
        ui.resample_pick = false;
        if (ui.shift) context_.send({Command::Type::AllNotesOff, 255});
        else context_.stop();
        break;
    case Button::SeqRecord:
        if (ui.shift) { // Recall: the last bars played, into a pattern (Capture)
            ui.capture_into = context_.track().pattern;
            for (unsigned n = 0; n < pattern_count; ++n)
                if (patterns_->at[ui.track][n].empty()) { ui.capture_into = uint8_t(n); break; }
            context_.open(PageId::Capture);
            break;
        }
        context_.seq_record(); // arm, record now, or stop recording; a take is undone like any edit (Shift + Minus)
        break;
    case Button::TapTempo:
        if (ui.shift) { // Shift: the metronome, Off -> On -> Rec (only while Seq Record)
            context_.set_click(context_.ui.settings.click + 1u);
            static const char* const modes[] {"Metronome off", "Metronome on", "Metronome: rec"};
            context_.notice(modes[context_.ui.settings.click % 3]);
            break;
        }
        context_.tap_tempo();
        tap_down = context_.now; tap_held = true; // held, it opens the Tempo page
        break;
    // Shift + Minus / Plus: undo and redo, as printed, on every page.
    // Pad Bank held: Minus / Plus jump banks.
    case Button::Minus:
        if (ui.shift) context_.undo();
        else if (record_track_keys()) context_.record_octave(-1); // the source track's pads, while they play
        else if (!pad_bank_move(context_, -1)) page.nudge(context_, -1);
        break;
    case Button::Plus:
        if (ui.shift) context_.redo();
        else if (record_track_keys()) context_.record_octave(1);
        else if (!pad_bank_move(context_, 1)) page.nudge(context_, 1);
        break;
    // Note Repeat on: B1-B3 are its rate (Shift: triplets), except on Arp and Chord.
    case Button::TopLeft: if (!repeat_soft(context_, 0)) page.soft(context_, 0); break;
    case Button::TopMiddle: if (!repeat_soft(context_, 1)) page.soft(context_, 1); break;
    case Button::TopRight: if (!repeat_soft(context_, 2)) page.soft(context_, 2); break;
    case Button::WheelPush:
        if (ui.shift && ui.page == PageId::Song) song_marker(context_);
        else if (ui.shift && ui.page == PageId::Project && !ui.sheet_open && ui.project_demos) { ui.project_demos = false; ui.project_cursor = 0; } // out of DEMOS
        else if (ui.shift && ui.page == PageId::Project && !ui.sheet_open && !ui.move_open && !context_.project_folder.empty()) {
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
    end_copy_away(); // left the kit or Patterns: copying ends
}

// A pot drives its value once it meets it (takeover: shell/takeover.h); until
// then the card shows the value dim with a tick where the pot sits. Knob FX's
// cards follow their own mode (Settings), the rest the knobs'.
Takeover Shell::pot_takeover(const Binding& binding) const {
    return binding.kind == Binding::Kind::KnobParam ? ui_.settings.knob_fx_takeover : ui_.settings.knob_takeover;
}

void Shell::check_pickup(unsigned n, const Binding& binding) {
    auto& pot = ui_.pots[n];
    uint32_t key = binding.key(ui_.track);
    if (pot.bound == key) return;
    pot.bound = key;
    pot.scaled = -1;
    pot.engaged = pot_takeover(binding) == Takeover::Instant || takeover_meets(pot.position, binding);
}

void Shell::pot(unsigned n, int position, bool syncing) {
    auto bindings = page_bindings(*project_, ui_, context_.pattern());
    const auto& b = bindings[n];
    auto& pot = ui_.pots[n];
    int before = pot.position;
    pot.position = int16_t(position);
    if (syncing) { pot.bound = 0; check_pickup(n, b); return; }
    if (ui_.seq_held) ui_.seq_used = true; // Seq + pot: the pattern's length, Q or swing; its release goes nowhere
    if (ui_.erase) ui_.erase_used = true; // Erase + pot: its release deletes nothing
    if (ui_.sheet_open && ui_.page == PageId::Roll) ui_.sheet_open = false; // a pot leaves Roll's sheet and does its job
    if (!b.bound()) return;
    check_pickup(n, b);
    ui_.turning = int8_t(n); ui_.turned_at = context_.now; ui_.last_pot = int8_t(n); ui_.last_pot_shift = ui_.shift;
    if (ui_.shift) ui_.shift_used = true;
    if (ui_.ask != UiState::Ask::None) return; // a question open: the pots wait
    if (ui_.erase && b.kind == Binding::Kind::KnobParam) { // Erase + pot on Knob FX's cards: back to the effect's initial value
        context_.apply(b, effect_info(EffectKind(project_->knob_fx.kind)).params[size_t(b.id) % effect_params].initial);
        pot.engaged = false; pot.bound = 0;
        return;
    }
    if (ui_.erase && ui_.page == PageId::Motion) { // Erase + pot on Motion: clear the lane, once B3 says so (as on the MPC)
        int lane = motion_lanes(*project_, ui_)[n];
        uint8_t param = uint8_t(lane_param(lane)), kit = uint8_t(lane_pad(lane) % sampler_pads);
        if (lane >= 0 && project_->motion.has(ui_.track, context_.track().pattern, param, kit)) ask(context_, UiState::Ask::EraseLane, lane);
        else context_.notice("No motion on " + lane_name(context_.track(), lane));
        return;
    }
    if (ui_.erase && b.kind == Binding::Kind::Param) { // Erase + pot: back to the initial value
        context_.apply(b, engine_info(context_.track().engine).params[b.id].initial);
        pot.engaged = false; pot.bound = 0;
        return;
    }
    int value = 0;
    if (takeover_move(pot_takeover(b), pot, before, position, b, value)) context_.apply(b, value);
}

void Shell::wheel(int delta) {
    if (ui_.exit_open || ui_.ask != UiState::Ask::None) return;
    if (ui_.name.open()) { // Shift: the character under the cursor; else the cursor
        if (ui_.shift) { ui_.shift_used = true; ui_.name.step_char(delta); } else ui_.name.move(delta);
        return;
    }
    if (seq_wheel(context_, delta)) return;     // Seq held: the tempo
    if (pad_bank_move(context_, delta)) return; // Pad Bank held: the wheel jumps banks
    if (ui_.shift && ui_.last_pot >= 0 && ui_.page != PageId::Patterns && ui_.page != PageId::Roll) { // Patterns: locates by Q; Roll: note to note
        // Shift + wheel: fine edit of the last pot touched, one step a detent.
        ui_.shift_used = true;
        auto bindings = page_bindings(*project_, ui_, context_.pattern(), ui_.last_pot_shift);
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
    if (card_owns_controls(ui_)) { // the pads only play the selected track's sound
        if (down) ui_.held_pads |= 1u << pad; else ui_.held_pads &= ~(1u << pad);
        context_.play_pad(pad, down ? ui_.settings.velocity(velocity) : 0, down);
        return;
    }
    if (!down) { hit(pad, velocity, down); context_.close_hold(pad); return; }
    context_.open_hold(pad); // Roll, Steps, Step Edit: press to release is one step (then PageEdit stands aside)
    PageEdit step(context_, Button::Count, true);
    hit(pad, velocity, down);
    step.finish();
}

void Shell::hit(unsigned pad, uint8_t velocity, bool down) {
    if (down) velocity = ui_.settings.velocity(velocity); // the pad response
    if (down) ui_.held_pads |= 1u << pad; else ui_.held_pads &= ~(1u << pad);
    if (down && ui_.erase) ui_.erase_used = true; // Erase + pad: its release deletes nothing
    if (down && (ui_.exit_open || ui_.ask != UiState::Ask::None)) return; // a sheet or a question over the page
    if (down && track_pad(context_, pad)) { end_copy_away(); return; } // Pad Bank + pad: that track (or a track copy's)
    if (ui_.name.open()) { if (down) ui_.name.pad(pad, context_.now); return; } // the pads type
    if (loop_held_pad(context_, pad, down)) return; // Loop held: the loop grid on any page
    if (down && pattern_copy_pad(context_, pad)) return; // a pattern copy (Shift + Erase with Seq held): from, then to
    if (seq_pad(context_, pad, down)) return;            // Seq held: the selected track's patterns (shell/seq_layer.cpp)
    // A Record take of Melodee's own sound: the pads play the instrument until it ends (Shift + pad jobs stay).
    if (!(down && ui_.shift) && (context_.record_keys() || (!down && context_.record_key_held(pad)))) {
        context_.record_key(pad, velocity, down);
        return;
    }
    if (down && ui_.mute_mode()) { mute_pad(context_, pad, kit_pads()); return; } // mute mode: mute; with Shift, solo
    if (down && sample_pad(context_, pad)) return; // Sample held: the pad becomes the last pad, unheard
    if (down && ui_.shift && ui_.page == PageId::Looper && pad >= 8 && pad < 12) { // the loop grid: play or stop at once
        ui_.shift_used = true;
        loop_grid_pad(context_, pad, true);
        return;
    }
    if (down && ui_.shift) { // the printed Shift + pad jobs, on every page but the browser (its next empty pad)
        ui_.shift_used = true;
        if (!samples_shift_pad(context_)) shift_pad(context_, pad);
        return;
    }
    if (down && ui_.resample_pick) { // Resample (Shift + Pad 11): this pad gets the render
        context_.resample_pad(context_.track().bank % sampler_banks * 16 + pad);
        return;
    }
    bool copying_pads = ui_.copying == UiState::Copying::Pads;
    if (down && kit_pads() && (copying_pads || ui_.erase)) {
        unsigned kit = context_.track().bank % sampler_banks * 16 + pad;
        if (!copying_pads) { // Erase + pad: stopped, the pad empty; playing or recording, its notes out of the pattern
            if (host.playing() || ui_.recording) context_.erase_pad_notes(kit);
            else context_.delete_pad(kit);
            return;
        }
        if (ui_.copy_from < 0) {                                    // copying: first the pad to copy
            if (context_.track().pads[kit].used()) { ui_.copy_from = int8_t(kit); context_.track().last_pad = uint8_t(kit); }
            else context_.notice(pad_text("Empty", kit));
            return;
        }
        if (unsigned(ui_.copy_from) == kit) return;                // then where it goes
        context_.copy_pad(unsigned(ui_.copy_from), kit);
        context_.track().last_pad = uint8_t(kit);
        ui_.stop_copying();
        return;
    }
    pad_job(ui_)(context_, pad, velocity, down);
}

// A Record take from a synth track: its pads play, so Minus / Plus move their octave.
bool Shell::record_track_keys() const {
    if (!context_.record_keys() || ui_.settings.input.source != InputSource::Track) return false;
    auto engine = project_->tracks[ui_.settings.input.track % track_count].engine;
    return engine != EngineKind::Sampler && engine != EngineKind::Kit909;
}

// Whether the pads play the selected track's kit on this page, one pad each:
// there Erase + pad deletes a pad and Shift + Erase copies one.
bool Shell::kit_pads() const {
    const auto& t = project_->tracks[ui_.track % track_count];
    return t.engine == EngineKind::Sampler && !t.levels && pad_job(ui_) == play_pad;
}

// Copying ends once its pads are gone: pads away from a kit, patterns away from Patterns.
void Shell::end_copy_away() {
    bool here = ui_.copying == UiState::Copying::Pads ? kit_pads()
              : ui_.copying == UiState::Copying::Tracks || ui_.copying == UiState::Copying::Patterns; // tracks, patterns: on any page
    if (ui_.copying != UiState::Copying::Off && !here) ui_.stop_copying();
}

void Shell::snapshot(Snapshot& out, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    auto bindings = page_bindings(*project_, ui_, context_.pattern());
    for (unsigned n = 0; n < 3; ++n) if (bindings[n].bound()) check_pickup(n, bindings[n]);
    fader_check(context_);
    out.ui = ui_;
    out.project = *project_;
    out.pattern = context_.pattern();
    for (unsigned t = 0; t < track_count; ++t)
        out.has_notes[t] = !patterns_->at[t][project_->tracks[t].pattern].empty();
    out.playing = host.playing();
    for (unsigned t = 0; t < track_count; ++t) {
        out.positions[t] = host.position_of(t);
        out.playing_patterns[t] = uint8_t(host.pattern_of(t));
    }
    out.position = unsigned(std::max(out.positions[ui_.track], 0));
    out.ticks = host.ticks();
    out.count_in = host.count_in();
    out.tempo = context_.tempo_now();
    // MIDI clock in leads the tempo while Sync In is on and a clock comes.
    out.following = midi_io.following();
    out.following_mtc = midi_io.following_mtc();
    if (ui_.settings.midi.sync_in == MidiSync::Clock && midi_io.tempo_in()) out.tempo = host.tempo_now();
    {
        const auto& r = host.recorder();
        out.take.state = uint8_t(r.state()); out.take.frames = r.frames(); out.take.length = r.length(); out.take.level = r.level();
        for (unsigned c = 0; c < out.take.peaks.size(); ++c) out.take.peaks[c] = r.peak(c);
        out.render_state = uint8_t(host.bounce().state()); out.render_frames = host.bounce().frames();
        out.recall_frames = host.recall().filled();
        out.monitoring = host.monitoring();
        out.speaker_quieted = input_ && input_->speaker_quieted();
        out.usb_state = input_ ? uint8_t(input_->usb_state()) : 0;
    }
    if (ui_.page == PageId::Capture) { out.history = context_.history; out.history_at = context_.history_at; }
    for (unsigned n = 0; n < 16; ++n) out.held_keys[n] = context_.pad_keys[n][0];
    out.project_path = context_.project_path; out.project_folder = context_.project_folder; out.dirty = context_.dirty;
    if (store_ && (ui_.page == PageId::Project || ui_.exit_open)) store_->view(out.projects);
    if (context_.plugins && ui_.page == PageId::Plugins) out.plugins = context_.plugins->view();
    if (library_) {
        const auto& t = project_->tracks[ui_.track];
        unsigned slot = ui_.page == PageId::Chop ? ui_.chop_sample : t.pads[t.last_pad % sampler_pads].sample;
        out.sample = slot < sample_slots ? library_->slot(slot) : LoadedSample {};
        if (ui_.page == PageId::Samples) {
            out.files = library_->files(); out.sample_root = library_->root(); out.files_folder = library_->folder(); out.files_top = library_->top();
        }
    }
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned n = 0; n < pattern_count; ++n) {
            const auto& pattern = patterns_->at[t][n];
            auto& sketch = out.sketches[t][n];
            sketch.length = pattern.length; sketch.keys.fill(0);
            sketch.notes = !pattern.empty();
            for (const auto& e : pattern.events) { // the first note of each of the first 64 sixteenths
                unsigned k = e.tick / sixteenth_ticks;
                if (k >= sketch.keys.size() || e.tick >= pattern.length) break;
                if (!sketch.keys[k]) sketch.keys[k] = e.key;
            }
        }
    out.song_playing = host.song_playing();
    out.song_bar = host.song_bar();
    {
        const auto& lp = host.looper();
        out.looper_on = lp.enabled_now();
        out.loop_tick = lp.beat_tick();
        for (unsigned n = 0; n < loop_tracks; ++n) {
            auto& l = out.loops[n];
            l.view = lp.view(n);
            l.meter = host.loop_meter(n);
            if (ui_.page == PageId::Looper) for (unsigned c = 0; c < looper::peak_columns; ++c) l.peaks[c] = lp.peak(n, c);
        }
    }
    for (unsigned t = 0; t < track_count; ++t) out.meters[t] = host.meter(t);
    // The voices of the track in view, placed where they are heard: the device frame heard now (the audio clock,
    // at the device's own rate, less what the PCM holds queued) against the frame the taps stand at.
    host.watch_taps(int(ui_.track % track_count));
    if (!host.taps(out.taps) || out.taps.track != ui_.track % track_count) out.taps = TrackTaps {};
    out.tap_frames = 0;
    if (const auto& clock = host.audio_clock(); clock.known() && out.taps.track != 255) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        out.tap_frames = double(clock.frame_at(int64_t(ns))) - double(clock.latency_frames()) - double(out.taps.stamp);
    }
    trail.update(out.taps, out.tap_frames, now);
    out.heads = trail.heads();
    out.grains = trail.grains();
    out.compression = host.compression();
    {
        float level = -90, reduce = 0;
        host.compressor_activity(level, reduce);
        comp_trail.update(level, reduce, now);
        out.comp_trail = comp_trail.trail();
    }
    out.flex_playing = host.flex_playing();
    out.now = now;
}

std::string Shell::summary() const {
    uint32_t pt = page_track.load(std::memory_order_relaxed);
    uint32_t st = stage_bits.load(std::memory_order_relaxed);
    std::string key;
    { std::lock_guard<std::mutex> guard(text_lock); key = stage_key_text; }
    for (auto& ch : key) if (ch == ' ') ch = '_';
    return "melodee_page=" + std::to_string(pt & 0xff) + " melodee_track=" + std::to_string((pt >> 8) + 1) +
           " melodee_stage=" + std::to_string((st & 0xf) + 1) + "." + std::to_string(((st >> 4) & 0xf) + 1) + " melodee_stage_key=" + key +
           " melodee_stage_card=" + std::to_string(((st >> 8) & 0xf) + 1) + " melodee_preset_due=" + std::to_string((st >> 12) & 1) +
           " melodee_tracks_half=" + std::to_string((st >> 13) & 1) + " melodee_preset=" + std::to_string(((st >> 16) & 0x3ff) + 1) +
           " melodee_bank=" + std::string(1, char('A' + ((st >> 26) & 7))) +
           " melodee_pad_sample=" + std::to_string(int(pad_sample.load()) == 0xffff ? -1 : int(pad_sample.load())) +
           " melodee_pad_frames=" + std::to_string(pad_frames.load()) + " melodee_tempo=" + std::to_string(tempo_now.load()) +
           " melodee_punch=" + std::to_string(int(host.punching())) + " melodee_buttons=" + button_text() + " melodee_pads=" + lit_pads_text() +
           " melodee_rec=" + std::to_string(rec_state.load()) + " melodee_count_in=" + std::to_string(host.count_in()) +
           " melodee_position=" + std::to_string(host.position_of(pt >> 8)) +
           " melodee_pattern_ticks=" + std::to_string(pattern_ticks.load()) + " melodee_events=" + std::to_string(pattern_events.load()) +
           " melodee_q=" + std::to_string(pattern_q.load()) + " melodee_first_tick=" + std::to_string(first_tick.load()) +
           " melodee_signature=" + std::to_string(signature_now.load() / 100) + "/" + std::to_string(signature_now.load() % 100) +
           " melodee_step=" + std::to_string(step_now.load() + 1) + " melodee_event=" + std::to_string(event_now.load()) + "/" +
           std::to_string(step_events_now.load()) + " melodee_event_tick=" + std::to_string(event_tick.load()) +
           " melodee_ask=" + std::to_string(ask_now.load()) + " melodee_motion=" + std::to_string(motion_now.load()) +
           " melodee_roll_key=" + std::to_string(roll_key_now.load()) + " melodee_roll_focus=" + std::to_string(roll_focus_now.load()) +
           " melodee_roll_view=" + std::to_string(roll_view_now.load() + 1) + " melodee_roll_top=" + std::to_string(roll_top_now.load()) +
           " melodee_sheet=" + std::to_string(sheet_now.load()) + pad_summary() + sample_summary()
           + input_summary() + fx_summary() + midi_io.summary(host) + midi_config_summary() + phase10_summary() +
           // Phase 10b: SD card access (0 home, 1 lending, 2 lent, 3 returning; the note shell/card_access.h's CardNote).
           " melodee_card=" + std::to_string(card_link.state.load()) + " melodee_card_note=" + std::to_string(card_link.note.load()) +
           " melodee_card_ejected=" + std::to_string(int(card_link.ejected.load())) + " melodee_card_locked=" + std::to_string(int(card_link.locked.load())) +
           " " + looper_status(host.looper()) +
           " melodee_plugins=" + (context_.plugins ? context_.plugins->summary() : std::string("none"));
}

// Phase 10: undo (depth, redo waiting, the last step's name), Seq held, the sampler map, DEMOS, a pot gesture open.
std::string Shell::phase10_summary() const {
    uint32_t b = phase10_bits.load(std::memory_order_relaxed);
    std::string what;
    { std::lock_guard<std::mutex> guard(text_lock); what = undo_text; }
    for (auto& ch : what) if (ch == ' ') ch = '_';
    return " melodee_undo=" + std::to_string(b & 0xff) + " melodee_redo=" + std::to_string((b >> 8) & 1) +
           " melodee_undo_last=" + (what.empty() ? std::string("none") : what) + " melodee_seq_held=" + std::to_string((b >> 9) & 1) +
           " melodee_map=" + std::to_string((b >> 10) & 1) + " melodee_demos=" + std::to_string((b >> 11) & 1) +
           " melodee_gesture=" + std::to_string((b >> 12) & 1) + " melodee_song_clips=" + std::to_string((b >> 16) & 0xfff) +
           " melodee_engine=" + std::to_string(b >> 28);
}

// Phase 9: MIDI Config as set (the settings are the MIDI thread's; these are copies taken in lights()).
std::string Shell::midi_config_summary() const {
    uint32_t m = midi_bits.load(std::memory_order_relaxed);
    return " melodee_midi_port=" + std::to_string(m & 1) + " melodee_midi_in_ch=" + std::to_string((m >> 1) & 31)
         + " melodee_midi_out_ch=" + std::to_string((m >> 6) & 31) + " melodee_pad_in=" + std::to_string((m >> 11) & 1)
         + " melodee_pad_out=" + std::to_string((m >> 12) & 3) + " melodee_sync_in=" + std::to_string((m >> 14) & 3)
         + " melodee_sync_out=" + std::to_string((m >> 16) & 3) + " melodee_thru=" + std::to_string((m >> 18) & 1)
         + " melodee_program_change=" + std::to_string((m >> 19) & 1) + " melodee_cv_out=" + std::to_string((m >> 20) & 1)
         + " melodee_cv_ppqn=" + std::to_string((m >> 21) & 255) + " melodee_track_ch=" + std::to_string(track_channel.load(std::memory_order_relaxed))
         + " melodee_midi_row=" + std::to_string(midi_row.load(std::memory_order_relaxed));
}

// Phase 7: Knob FX and the master compressor.
std::string Shell::fx_summary() const {
    uint32_t k = knob_bits.load(std::memory_order_relaxed), targets = knob_targets.load(std::memory_order_relaxed);
    uint32_t c = comp_bits.load(std::memory_order_relaxed), side = comp_side.load(std::memory_order_relaxed);
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f", double(host.compression()));
    return " melodee_knob=" + std::to_string(k & 1) + " melodee_knob_bypass=" + std::to_string((k >> 1) & 1)
         + " melodee_rec_fx=" + std::to_string((k >> 2) & 1) + " melodee_knob_entry=" + std::to_string((k >> 8) & 0xff)
         + " melodee_knob_kind=" + std::to_string((k >> 16) & 0xff) + " melodee_knob_cursor=" + std::to_string(k >> 24)
         + " melodee_knob_tracks=" + std::to_string(targets & 0xffff) + " melodee_knob_pads=" + std::to_string(targets >> 16)
         + " melodee_comp=" + std::to_string(c & 1) + " melodee_comp_color=" + std::to_string((c >> 1) & 1)
         + " melodee_comp_boost=" + std::to_string((c >> 2) & 0x3f) + " melodee_comp_attack=" + std::to_string((c >> 8) & 0xff)
         + " melodee_comp_release=" + std::to_string((c >> 16) & 0xff) + " melodee_comp_amount=" + std::to_string(c >> 24)
         + " melodee_comp_sidechain=" + std::to_string(side & 1) + " melodee_comp_key=" + std::to_string(((side >> 1) & 0x7f) + 1)
         + " melodee_comp_key_pad=" + std::to_string(((side >> 8) & 0xff) == MasterComp::whole_kit ? 0u : ((side >> 8) & 0xff) + 1)
         + " melodee_comp_reduction=" + text + padfx_summary() + fader_summary();
}

// Phase 8: the fader. melodee_fader_job: 0 off, 1 the track's target, 2 the page's own job, 3 Step Edit's nudge;
// melodee_fader_kind: the card it drives (Binding::Kind), melodee_fader_value its value; takeovers 0 Pickup, 1 Scaled, 2 Instant.
std::string Shell::fader_summary() const {
    uint32_t f = fader_bits.load(std::memory_order_relaxed), v = fader_value.load(std::memory_order_relaxed);
    return " melodee_fader_target=" + std::to_string(f & 0xf) + " melodee_fader_on=" + std::to_string((f >> 4) & 1)
         + " melodee_fader_job=" + std::to_string((f >> 5) & 3) + " melodee_fader_engaged=" + std::to_string((f >> 7) & 1)
         + " melodee_fader_kind=" + std::to_string(v >> 16) + " melodee_fader_value=" + std::to_string(int(int16_t(v & 0xffff)))
         + " melodee_fader_light=" + std::to_string((f >> 16) & 0x7f) + " melodee_wheel=" + std::to_string((f >> 24) & 0x7f)
         + " melodee_takeover=" + std::to_string((f >> 8) & 3) + "," + std::to_string((f >> 10) & 3) + "," + std::to_string((f >> 12) & 3)
         + " melodee_fader_at=" + std::to_string(fader_at.load(std::memory_order_relaxed));
}

// Phase 7b: Pad FX and Flex Beat.
std::string Shell::padfx_summary() const {
    uint32_t b = padfx_bits.load(std::memory_order_relaxed), slots = punch_slots_bits.load(std::memory_order_relaxed);
    std::string kinds;
    for (unsigned n = 0; n < 4; ++n) kinds += (n ? "," : "") + std::to_string(int((slots >> (8 * n)) & 0xff) - 1);
    return " melodee_punch_bank=" + std::to_string(b & 3) + " melodee_engaged=" + std::to_string((b >> 2) & 0x1f)
         + " melodee_latched=" + std::to_string((b >> 7) & 0x1f) + " melodee_punch_selected=" + std::to_string(int((b >> 12) & 0xff) - 1)
         + " melodee_punch_slots=" + kinds + " melodee_live=" + std::to_string(host.live_punches())
         + " melodee_flex=" + std::to_string(host.flex_playing()) + " melodee_flex_mode=" + std::to_string((b >> 20) & 1)
         + " melodee_flex_quantize=" + std::to_string((b >> 21) & 1) + " melodee_flex_mix=" + std::to_string((b >> 22) & 0x7f);
}

// Phase 6: Input Config, Sample Record, Audio Recall and renders.
std::string Shell::input_summary() const {
    uint32_t r = record_bits.load(std::memory_order_relaxed);
    static const char* const renders[] {"idle", "cued", "running", "tail", "done", "cancelled", "failed"};
    return " melodee_take=" + std::to_string(unsigned(host.recorder().state())) + " melodee_take_frames=" + std::to_string(host.recorder().frames())
         + " melodee_rec_pad=" + std::to_string((r >> 8) & 0xff) + " melodee_rec_cuts=" + std::to_string((r >> 16) & 0x1f)
         + " melodee_input=" + std::to_string(r & 0xf) + " melodee_input_engine=" + std::to_string(host.record_source())
         + " melodee_monitor=" + std::to_string((r >> 4) & 3) + " melodee_monitoring=" + std::to_string(int(host.monitoring()))
         + " melodee_threshold=" + std::to_string(int8_t(r >> 24)) + " melodee_speaker=" + std::to_string(input_ ? int(input_->speaker_on()) : -1)
         + " melodee_recall_frames=" + std::to_string(host.recall().filled())
         + " melodee_render=" + renders[unsigned(host.bounce().state()) % 7] + " melodee_render_frames=" + std::to_string(host.bounce().frames())
         + " melodee_rendering=" + std::to_string((r >> 21) & 3) + " melodee_resample_pick=" + std::to_string((r >> 23) & 1);
}

// Phase 4: the last pad's Sample Mode values, for remote checks.
std::string Shell::pad_summary() const {
    uint32_t b = pad_bits.load(std::memory_order_relaxed);
    static const char* const kinds[] {"velocity", "scale", "tune", "filter"};
    return " melodee_last_pad=" + std::to_string(((b >> 8) & 0xff) + 1) + " melodee_pad_loop=" + std::to_string(b & 1) +
           " melodee_pad_lock=" + std::to_string((b >> 1) & 1) + " melodee_pad_note_on=" + std::to_string((b >> 2) & 1) +
           " melodee_pad_reverse=" + std::to_string((b >> 3) & 1) + " melodee_pad_mono=" + std::to_string((b >> 4) & 1) +
           " melodee_pad_filter=" + pad_filter_name(PadFilter((b >> 16) & 0xf)) + " melodee_pad_link=" + std::to_string((b >> 20) & 0x1f) +
           " melodee_pad_decay_from=" + ((b >> 25) & 1 ? "start" : "end") +
           " melodee_pad_gain=" + std::to_string(pad_gain.load(std::memory_order_relaxed)) +
           " melodee_levels=" + std::to_string((b >> 5) & 1) + " melodee_levels_kind=" + kinds[(b >> 6) & 3];
}

// Phase 5: Chop, the last pad's region and file, the browser.
std::string Shell::sample_summary() const {
    uint32_t c = chop_bits.load(std::memory_order_relaxed), f = files_bits.load(std::memory_order_relaxed);
    std::string name, folder;
    { std::lock_guard<std::mutex> guard(text_lock); name = pad_name_text; folder = folder_text; }
    for (auto& ch : name) if (ch == ' ') ch = '_';
    for (auto& ch : folder) if (ch == ' ') ch = '_';
    return " melodee_chop_type=" + std::string(chop_mode_name(ChopMode(c & 3))) + " melodee_chop_slices=" + std::to_string((c >> 8) & 0xff) +
           " melodee_chop_slice=" + std::to_string(((c >> 16) & 0xff) + 1) + " melodee_chop_armed=" + std::to_string((c >> 2) & 1) +
           " melodee_chop_taking=" + std::to_string((c >> 3) & 1) + " melodee_pad_slots=" + std::to_string(c >> 24) +
           " melodee_pad_start=" + std::to_string(pad_start_now.load()) + " melodee_pad_end=" + std::to_string(pad_end_now.load()) +
           " melodee_pad_name=" + (name.empty() ? "-" : name) + " melodee_files=" + std::to_string(f & 0xffff) +
           " melodee_file_cursor=" + std::to_string(((f >> 16) & 0x3fff) + 1) + " melodee_drive=" + ((f >> 30) & 1 ? "internal" : "card") +
           " melodee_preview=" + std::to_string(f >> 31) + " melodee_folder=" + (folder.empty() ? "-" : folder);
}

// The lit pads by number, 1-16.
std::string Shell::lit_pads_text() const {
    uint32_t lit = pads_lit.load(std::memory_order_relaxed);
    std::string out;
    for (unsigned n = 0; n < 16; ++n)
        if (lit & (1u << n)) { if (!out.empty()) out += ','; out += std::to_string(n + 1); }
    return out.empty() ? "none" : out;
}

// Each lit button as name:level (1 faint, 2 bright), in the surface's order.
std::string Shell::button_text() const {
    static const char* const names[] {"sample", "seq", "pad-fx", "knob-fx", "shift", "pad-bank", "erase", "note-repeat",
                                      "b1", "b2", "b3", "chop", "mute", "loop", "16-levels", "sample-select", "tap-tempo",
                                      "minus", "plus", "sample-record", "seq-record", "stop", "play", "wheel-push"};
    uint64_t levels = button_levels.load(std::memory_order_relaxed);
    std::string out;
    for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        unsigned level = unsigned(levels >> (2 * n)) & 3;
        if (!level) continue;
        if (!out.empty()) out += ',';
        out += names[n]; out += ':'; out += char('0' + level);
    }
    return out.empty() ? "none" : out;
}

} // namespace melodee
