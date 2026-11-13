// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/shell.h"
#include "melodee/crash_log.h"
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/key_pick.h"
#include "melodee/shell/p4_status.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/looper_chord.h"
#include "melodee/shell/input_route.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/common.h"
#include "melodee/pages/page.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/fader.h"
#include <chrono>
#include "melodee/shell/takeover.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_light.h"
#include <cstdio>
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/note_jobs.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/mix_hub.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/shell/sample_mode.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/shift_pads.h"
#include "surface/controls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace melodee {
namespace {
using Button = SurfaceButton;

// A button let go while the Name sheet or SD card access has the controls: what it held lets go, and its
// tap does nothing (it went down before they took them). Left held, Erase would go on deleting and Seq or
// Pad Bank would keep the pads once the sheet closes or the card comes back.
void let_go(Context& c, Button b) {
    auto& ui = c.ui;
    switch (b) {
    case Button::Seq: ui.seq_held = false; ui.seq_left = false; ui.seq_print = false; break;
    case Button::PadBank: ui.pad_bank = false; break;
    case Button::Mute: ui.mute = false; break;
    case Button::Erase: ui.erase = false; break;
    case Button::Sample: ui.sample_held = false; break;
    case Button::Loop: ui.loop_held = ui.loop_grip = ui.loop_shift_held = false; break;
    case Button::SixteenLevels: ui.levels_held = ui.levels_used = false; break;
    case Button::NoteRepeat: repeat_button(c, false); break; // held, the repeat ends with it
    default: break;
    }
}
} // namespace

Shell::Shell(EngineHost& host)
    : host(host), project_(std::make_unique<Project>()), patterns_(std::make_unique<PatternBank>()),
      context_(*project_, *patterns_, ui_, host) {
    starting_project(*project_, *patterns_);
    context_.card = &card_link;
    context_.updates = &update_link;
}

void Shell::start_settings(const std::string& text) {
    ui_.settings.read(text);
    want_audio_format();
}

namespace {
// The control or MIDI message being handled, for its notes' frame and Seq Record's place (Context::hit_ns).
struct HitStamp {
    Context& context;
    HitStamp(Context& c, uint64_t ns) : context(c) { context.hit_ns = ns; }
    ~HitStamp() { context.hit_ns = 0; }
    HitStamp(const HitStamp&) = delete;
    HitStamp& operator=(const HitStamp&) = delete;
};
} // namespace

void Shell::midi(const MidiPortInfo& port, const MidiPacket& packet, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    HitStamp stamp(context_, packet.timestamp_ns);
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
    fader_rest(context_); // a pitch bend the fader left, back to the middle once nothing holds it there
    context_.flush_motion(); // recorded Motion the audio thread has not had yet (roadmap P1)
    context_.take_arp_notes(); // and Note Repeat's notes under Seq Record
    midi_io.pump(context_, out, at);
}

void Shell::surface(const SurfaceEvent& event, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    context_.now = now;
    HitStamp stamp(context_, event.timestamp_ns);
    const auto& input = event.input;
    { // the crash report's last presses (melodee/crash_log.h), with the page and track they were made on
        char line[80] = {};
        uint32_t key = 0;
        const char* run = host.playing() ? "playing" : "stopped";
        const unsigned page = unsigned(ui_.page), track = ui_.track + 1u;
        if (event.pad >= 0) {
            unsigned kind = event.midi.status & 0xf0;
            if (kind == 0x90 || kind == 0x80)
                std::snprintf(line, sizeof line, "page %u track %u %s: pad %d %s %u", page, track, run, event.pad + 1,
                              kind == 0x90 && event.midi.value ? "down" : "up", unsigned(event.midi.value));
        } else if (input.kind == SurfaceInput::Kind::Button) {
            std::snprintf(line, sizeof line, "page %u track %u %s: %s %s", page, track, run, describe_last_input(pack_last_input(input)).c_str(),
                          input.pressed ? "down" : "up");
        } else if (input.kind == SurfaceInput::Kind::Control) {
            if (input.index != uint8_t(SurfaceControl::Wheel)) key = 0x100u | input.index; // a pot turning is one line, its last value
            std::snprintf(line, sizeof line, "page %u track %u %s: %s", page, track, run, describe_last_input(pack_last_input(input)).c_str());
        }
        if (line[0]) crash_log::note(line, key);
    }
    // While a project loads (Loading sheet) or an update runs (shell/update.h), presses and their releases are
    // ignored; pads and keys held from before still release. K1-K3 and the fader keep their positions; the main
    // volume still works. After a failed update, B1 closes the update sheet.
    const bool held_off = ui_.loading || ui_.update.phase != 0;
    if (event.pad >= 0) {
        unsigned kind = event.midi.status & 0xf0;
        bool& eaten = eaten_pads[unsigned(event.pad) % eaten_pads.size()];
        bool down = kind == 0x90 && event.midi.value, up = kind == 0x80 || (kind == 0x90 && !event.midi.value);
        if (held_off && down) { eaten = true; return; }
        if (eaten) { if (up) eaten = false; return; }
        if (held_off && !up) return;
        if (down) pad(unsigned(event.pad), event.midi.value, true);
        else if (up) pad(unsigned(event.pad), 0, false);
        else if (kind == 0xa0) context_.pressure(unsigned(event.pad), event.midi.value); // to the mod matrix
        return;
    }
    if (input.kind == SurfaceInput::Kind::Button) {
        bool& eaten = eaten_buttons[input.index];
        if (held_off && input.pressed) {
            if (ui_.update.phase == uint8_t(UpdatePhase::Failed) && Button(input.index) == Button::TopLeft) {
                ui_.update = {};
                update_link.progress(UpdatePhase::None, UpdateError::None);
            }
            eaten = true;
            return;
        }
        if (eaten && !input.pressed) { eaten = false; return; }
        button(Button(input.index), input.pressed);
        return;
    }
    if (input.kind != SurfaceInput::Kind::Control) return;
    if (held_off) {
        switch (SurfaceControl(input.index)) {
        case SurfaceControl::MainVolume: break;
        case SurfaceControl::K1: pot(0, input.value, true); return;
        case SurfaceControl::K2: pot(1, input.value, true); return;
        case SurfaceControl::K3: pot(2, input.value, true); return;
        case SurfaceControl::Fader: fader_move(context_, input.value, true); return;
        default: return;
        }
    }
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
        if (!pressed) { // a button held as the card went away lets go, doing nothing
            if (b == Button::Shift) ui_.shift = false;
            if (b == Button::TapTempo) tap_held = false;
            if (b == Button::Play || b == Button::Stop) press(b, pressed);
            else let_go(context_, b);
            return;
        }
        auto r = card_press(ui_, b);
        if (r.request != CardRequest::None) card_link.request.store(uint8_t(r.request));
        if (r.leave) context_.open(PageId::Project);
        if (!r.taken) press(b, pressed);
        return;
    }
    if (pressed && b != Button::Erase && b != Button::Shift && ui_.erase) ui_.erase_used = true; // Erase + a button: its release erases nothing
    // Erase's release is a press of its own (a tap erases the thing in focus): one undo step, as any page press.
    if ((!pressed && b != Button::Erase) || b == Button::Shift || ((b == Button::Minus || b == Button::Plus) && ui_.shift)) { press(b, pressed); return; }
    PageEdit step(context_, b, false);
    press(b, pressed);
    step.finish();
}

void Shell::press(Button b, bool pressed) {
    auto& ui = ui_;
    if (b == Button::Shift) {
        ui.shift = pressed;
        if (pressed) ui.shift_used = false;
        else if (ui.page == PageId::Roll) roll_shift_up(context_); // Roll: marking ends, or a tap clears the marks
        return;
    }
    if (b == Button::Play && !pressed && ui.fill) context_.fill(false); // Fill lasts while Play is held
    if (b == Button::Sample && !pressed) { sample_button(context_, false); return; } // always heard, so Sample is never left held
    if (pressed && ask_button(context_, b)) return; // a question open (shell/ask.cpp): B1 Cancel, B3 Do it
    if (new_track_lock(context_, b, pressed)) return; // an empty track in hand: made or left first (shell/new_track.h)
    if (ui.name.open()) { // the Name sheet takes the controls it uses; Play and Stop still work
        if (!pressed) { if (b == Button::TapTempo) tap_held = false; let_go(context_, b); return; }
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
    if (pressed && !ui.exit_open && (b == Button::TopLeft || b == Button::TopMiddle || b == Button::TopRight) &&
        sample_key(context_, b == Button::TopLeft ? 0 : b == Button::TopMiddle ? 1 : 2)) return; // Sample held: Stage's sections
    if (b == Button::PadBank) { pad_bank_button(context_, pressed); return; } // shell/pad_bank.cpp
    if (b == Button::Mute) { // shell/mute_mode.cpp; Roll's marked notes first (shell/roll.h)
        if (!pressed || !roll_selection_mute(context_)) mute_button(context_, pressed);
        return;
    }
    if (b == Button::NoteRepeat && (!ui.exit_open || !pressed)) { // shell/note_repeat.cpp; on Song the pads play nothing: it repeats the clip
        if (pressed && ui.shift && note_arp(context_)) return;   // the note editors: Shift, the arp written as notes
        if (pressed && roll_selection_repeat(context_)) return; // Roll's marked notes: their ratchet
        if (pressed && song_buttons()) song_repeat(context_);
        else repeat_button(context_, pressed);
        return;
    }
    if (b == Button::Sample) { if (!ui.exit_open) sample_button(context_, true); return; } // shell/sample_mode.cpp: held, a pad picks a kit pad
    if ((b == Button::PadFx || b == Button::KnobFx) && (!ui.exit_open || !pressed)) {
        // Pad FX and Knob FX together, in either order: the MIX hub on Tracks (shell/mix_hub.h); alone, their own job
        // when let go. Shift + either acts as it goes down (the cases below).
        switch (fx_chord(ui, b, pressed)) {
        case FxChord::Mix: context_.open(PageId::Tracks); end_copy_away(); return;
        case FxChord::Wait: return;
        case FxChord::Own:
            if (pressed) break; // Shift: below, as printed
            if (ui.exit_open || ui.ask != UiState::Ask::None) return; // let go under a sheet: nothing
            if (b == Button::KnobFx) context_.knob_fx_button();
            else context_.open(PageId::Punch);
            return;
        }
    }
    if ((b == Button::Loop || b == Button::SixteenLevels) && (!ui.exit_open || !pressed)) {
        // Loop and 16 Levels together, in either order: the Looper, and neither's own job (shell/looper_chord.h).
        // 16 Levels alone acts on its release (the last pad on all sixteen; Shift: the type, as it goes down).
        // Held, K1-K3 and − / + are its settings (sample_mode.h); let go after levels_hold_ms, nothing.
        if (b == Button::SixteenLevels && pressed) {
            ui.levels_down = context_.now;
            auto& t = context_.track();
            if (t.engine == EngineKind::Sampler && !t.levels) t.levels_pad = t.last_pad; // its cards are the last pad's
        }
        if (b == Button::SixteenLevels && !pressed && ui.levels_held && context_.now - ui.levels_down >= levels_hold_ms) ui.levels_used = true;
        switch (looper_chord(ui, b, pressed)) {
        case ChordAct::Looper: if (ui.shift) ui.shift_used = true; context_.open(PageId::Looper); end_copy_away(); return;
        case ChordAct::Levels:
            if (ui.shift) ui.shift_used = true;
            if (!pressed && (ui.exit_open || ui.ask != UiState::Ask::None)) return; // let go under a sheet: nothing
            levels_button(context_);
            end_copy_away();
            return;
        case ChordAct::Nothing: return;
        case ChordAct::Own: break;
        }
        if (b == Button::Loop) { loop_press(context_, pressed); return; } // held: the loop grid (shell/looper_jobs.cpp)
    }
    if (b == Button::Erase) {
        bool was_down = ui.erase;
        ui.erase = pressed;
        if (!pressed) { // tapped on its own: the thing in focus on the page goes (shell/erase_copy.h)
            if (was_down && !ui.erase_used) erase_tap(context_);
            return;
        }
        ui.erase_used = ui.shift || ui.copying != UiState::Copying::Off; // Copy, or leaving it, is the press's job
        if (ui.shift) { // Shift + Erase: Copy, as printed, of what the page holds (shell/erase_copy.h; again: off)
            ui.shift_used = true;
            if (ui.pad_bank) ui.pad_bank_used = true;
            if (ui.seq_held) ui.seq_used = true;
            if (ui.copying != UiState::Copying::Off) { context_.notice(ui.moving ? "Move off" : "Copy off"); ui.stop_copying(); }
            else copy_press(context_, kit_pads(), key_pads());
        } else if (ui.copying != UiState::Copying::Off) { // Erase alone leaves copying (or a track move)
            context_.notice(ui.moving ? "Move off" : "Copy off");
            ui.stop_copying();
        } else if (song_buttons()) {
            ui.erase_used = true; // the clip goes as Erase goes down; its release erases nothing more
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
        // the MPC off, B3 leaves for the stock app. Push presses the key the wheel highlights.
        if (b == Button::WheelPush && !ui.exit_off) b = ui.exit_pick == 2 ? Button::TopRight : ui.exit_pick == 1 ? Button::TopMiddle : Button::TopLeft;
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
    case Button::PadFx: // Pad FX: the punches; Shift: Flex Beat (as printed)
        if (ui.shift) { context_.open(PageId::FlexBeat); break; }
        context_.open(PageId::Punch);
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
        if (ui.page == PageId::Print) { context_.print_sheet_cancel(); break; } // again: the sheet closes (calling off its prints)
        if (context_.track().engine != EngineKind::Sampler) { context_.open_print_sheet(); break; } // freeze plan F2c: PRINT
        if (ui.page != PageId::Record) context_.open(PageId::Record);
        else context_.record_button();
        break;
    case Button::Chop: // Chop on the last pad's sample; Shift: Note On; on Song the clip in two; on the note editors split (Shift: legato)
        if (song_buttons()) song_chop(context_);
        else if (!(ui.shift ? note_legato(context_) : note_chop(context_))) chop_button(context_);
        break;
    case Button::Play: // Shift: Continue; held while playing: Fill (roadmap P1)
        if (ui.shift) context_.play_continue();
        else if (context_.host.playing()) context_.fill(true);
        else context_.play();
        break;
    case Button::Stop:
        if (ui.page == PageId::Record) context_.send({Command::Type::RecordStop}); // the take ends; Record stays
        ui.resample_pick = false;
        if (ui.shift) context_.send({Command::Type::AllNotesOff, 255});
        else context_.stop();
        break;
    case Button::SeqRecord:
        if (ui.erase) { ui.erase_used = true; erase_selected_pad(context_); break; } // the selected pad's notes, asked first
        if (ui.shift) { context_.capture(); break; } // Recall: the last bars played, into the next empty pattern (Capture)
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
        if (ui.sample_held && ui.light_show && !ui.shift) { light_style_step(-1); break; } // the light show's style
        if (ui.shift) context_.undo();
        else if (levels_held_cards(*project_, ui)) { ui.levels_used = true; levels_nudge(context_, -1); } // 16 Levels held
        else if (record_track_keys()) context_.record_octave(-1); // the source track's pads, while they play
        else if (!pad_bank_move(context_, -1)) page.nudge(context_, -1);
        break;
    case Button::Plus:
        if (ui.sample_held && ui.light_show && !ui.shift) { light_style_step(1); break; }
        if (ui.shift) context_.redo();
        else if (levels_held_cards(*project_, ui)) { ui.levels_used = true; levels_nudge(context_, 1); }
        else if (record_track_keys()) context_.record_octave(1);
        else if (!pad_bank_move(context_, 1)) page.nudge(context_, 1);
        break;
    // The pads the tracks (Pad Bank held, a track move or copy): B2 MOVE, B3 COPY, B1 back (shell/pad_bank.h).
    // Note Repeat on: B1-B3 are its rate (Shift: triplets), except on Arp and Chord.
    case Button::TopLeft: if (!track_soft(context_, 0) && !repeat_soft(context_, 0)) page.soft(context_, 0); break;
    case Button::TopMiddle: if (!track_soft(context_, 1) && !repeat_soft(context_, 1)) page.soft(context_, 1); break;
    case Button::TopRight: if (!track_soft(context_, 2) && !repeat_soft(context_, 2)) page.soft(context_, 2); break;
    case Button::WheelPush:
        if (ui.shift && ui.page == PageId::Song) song_marker(context_);
        else if (ui.shift && ui.page == PageId::Project && ui.project_list && !ui.sheet_open && !ui.move_open && project_folder_up(context_)) {
            // up a folder, done
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
    // A Trim or Chop handle's pot: that handle in focus, the view zoomed in round it (shell/zoom_view.h).
    if (!syncing && focus_handle(ui_, bindings[n])) bindings = page_bindings(*project_, ui_, context_.pattern());
    const auto& b = bindings[n];
    auto& pot = ui_.pots[n];
    int before = pot.position;
    pot.position = int16_t(position);
    if (syncing) { pot.bound = 0; check_pickup(n, b); return; }
    if (ui_.seq_held) ui_.seq_used = true; // Seq + pot: the pattern's length, Q or swing; its release goes nowhere
    if (ui_.erase) ui_.erase_used = true; // Erase + pot: its release deletes nothing
    if (ui_.levels_held) ui_.levels_used = true; // 16 Levels + pot: its setting; the release toggles nothing
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
        int lane = motion_lanes(*project_, ui_, context_.pattern())[n];
        if (lane >= 0 && context_.pattern().has_lane(lane_param(lane), lane_pad(lane) % sampler_pads)) ask(context_, UiState::Ask::EraseLane, lane);
        else context_.notice("No motion on " + lane_name(context_.track(), lane, project_.get()));
        return;
    }
    if (ui_.erase && b.kind == Binding::Kind::Param) { // Erase + pot: back to the initial value
        context_.apply(b, engine_info(context_.track().engine).params[b.id].initial);
        pot.engaged = false; pot.bound = 0;
        return;
    }
    if (ui_.erase && b.kind == Binding::Kind::PluginParam) { // a plug-in's: back to its default
        if (const auto* card = plugin_card(ui_.plugin_panel, b.id)) context_.apply(b, card->initial);
        pot.engaged = false; pot.bound = 0;
        return;
    }
    learn_fader(context_, b, n); // fader C: the fader follows the pot turned last, unless PIN holds it
    int value = 0;
    if (takeover_move(pot_takeover(b), pot, before, position, b, value)) context_.apply(b, value);
}

void Shell::wheel(int delta) {
    if (ui_.erase) ui_.erase_used = true; // Erase + the wheel: its release erases nothing
    if (ui_.exit_open) { if (!ui_.exit_off) ui_.exit_pick = key_pick_turn(ui_.exit_pick, delta); return; } // STAY, POWER OFF, MPC APP
    if (ui_.ask != UiState::Ask::None) { ui_.ask_pick = key_pick_turn(ui_.ask_pick, delta, 0b101); return; } // CANCEL or the verb
    if (ui_.name.open()) { // Shift: the character under the cursor; else the cursor
        if (ui_.shift) { ui_.shift_used = true; ui_.name.step_char(delta); } else ui_.name.move(delta);
        return;
    }
    if (seq_wheel(context_, delta)) return;     // Seq held: the tempo
    if (pad_bank_move(context_, delta)) return; // Pad Bank held: the wheel jumps banks
    if (ui_.shift && ui_.last_pot >= 0 && ui_.page != PageId::Patterns && ui_.page != PageId::Roll) { // Patterns: locates by Q; Roll: marks notes
        // Shift + wheel: fine edit of the last pot touched, one step a detent.
        ui_.shift_used = true;
        auto bindings = page_bindings(*project_, ui_, context_.pattern(), ui_.last_pot_shift);
        const auto& b = bindings[size_t(ui_.last_pot)];
        if (!b.bound()) return;
        context_.fine_edit(b, delta); // a Trim or Chop handle by the view on screen
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
    if (!down) {
        hit(pad, velocity, down);
        // Whatever took the release (another page's pads, the Loop grid, the Name sheet), the pad lets go of
        // all it started: its notes, the step it held on Steps, the pattern it held under Seq.
        context_.release_pad(pad);
        if (ui_.held_step >= 0 && unsigned(ui_.held_step) % bar_steps == pad % 16) ui_.held_step = -1;
        if (ui_.held_note == int(pad % 16)) ui_.held_note = -1; // Roll's note held for its locks
        if (ui_.pattern_held >= 0 && unsigned(ui_.pattern_held) % pattern_pads == pad % 16) ui_.pattern_held = -1; // a pattern of any bank
        context_.close_hold(pad);
        return;
    }
    context_.open_hold(pad); // Roll, Steps, Step Edit: press to release is one step (then PageEdit stands aside)
    PageEdit step(context_, Button::Count, true);
    hit(pad, velocity, down);
    step.finish();
}

void Shell::hit(unsigned pad, uint8_t velocity, bool down) {
    if (down) velocity = ui_.settings.velocity(velocity); // the pad response
    if (down) ui_.held_pads |= 1u << pad; else ui_.held_pads &= ~(1u << pad);
    if (down && ui_.erase) ui_.erase_used = true; // Erase + pad: its release deletes nothing
    if (!down) { // a release goes where its press went
        pad_let_go[pad % 16] = context_.now + 1; // its light fades from here
        if (ui_.name.open()) return;
        if (loop_held_pad(context_, pad, false)) return;
        if (seq_pad(context_, pad, false)) return;
        if (context_.record_keys() || context_.record_key_held(pad)) { context_.record_key(pad, velocity, false); return; }
        pad_job(ui_)(context_, pad, velocity, false);
        return;
    }
    if (ui_.shift && new_track_locked(context_)) return; // Shift + pad's pages wait while an empty track is in hand
    // A press: what the pads are now, the same choice their lights make (shell/pad_context.h, shell/pad_lights.cpp).
    unsigned kit = context_.track().bank % sampler_banks * 16 + pad % 16;
    switch (pad_context(ui_, context_.track(), pad_facts(), pad)) {
    case PadContext::Sheet: return; // a sheet or a question over the page
    case PadContext::Tracks: track_pad(context_, pad); end_copy_away(); return; // Pad Bank + pad: that track (or a track copy's)
    case PadContext::Name: ui_.name.pad(pad, context_.now); return; // the pads type
    case PadContext::LoopHeld: loop_held_pad(context_, pad, true); return; // Loop held: the loop grid on any page
    case PadContext::PatternCopy: pattern_copy_pad(context_, pad); return; // Shift + Erase with Seq held: from, then to
    case PadContext::SeqLayer: seq_pad(context_, pad, true); return; // Seq held: the selected track's patterns (shell/seq_layer.cpp)
    case PadContext::RecordKeys: context_.record_key(pad, velocity, true); return; // a take of Melodee's own sound: the instrument
    case PadContext::Mute: mute_pad(context_, pad, kit_pads()); return; // mute; with Shift, solo
    case PadContext::SamplePick: sample_pad(context_, pad); return; // Sample held: the pad becomes the last pad, unheard
    case PadContext::LoopNow: ui_.shift_used = true; loop_grid_pad(context_, pad, true); return; // the loop grid: at once
    case PadContext::Shift: // the printed Shift + pad jobs, on every page but the browser (its next empty pad)
        ui_.shift_used = true;
        if (!samples_shift_pad(context_)) shift_pad(context_, pad);
        return;
    case PadContext::ResamplePick: context_.resample_pad(kit); return; // Resample (Shift + Pad 11): this pad gets the render
    case PadContext::KeyCopy: key_copy_pad(context_, pad); return;   // a key's notes onto another key (shell/erase_copy.h)
    case PadContext::StepCopy: step_copy_pad(context_, pad); return; // a step onto another step
    case PadContext::KeyErase: key_erase_pad(context_, pad); return; // that key's notes, once B3 says so
    case PadContext::PadErase: // stopped, the pad empty; playing or recording, its notes out of the pattern
        if (host.playing() || ui_.recording) context_.erase_pad_notes(kit);
        else context_.delete_pad(kit);
        return;
    case PadContext::PadCopy:
        if (ui_.copy_from < 0) { // first the pad to copy
            if (context_.track().pads[kit].used()) { ui_.copy_from = int8_t(kit); context_.track().last_pad = uint8_t(kit); }
            else context_.notice(pad_text("Empty", kit));
            return;
        }
        if (unsigned(ui_.copy_from) == kit) return; // then where it goes
        context_.copy_pad(unsigned(ui_.copy_from), kit);
        context_.track().last_pad = uint8_t(kit);
        ui_.stop_copying();
        return;
    case PadContext::Page: break;
    }
    pad_job(ui_)(context_, pad, velocity, down);
}

PadFacts Shell::pad_facts() const {
    PadFacts f;
    f.card = card_owns_controls(ui_);
    f.record_keys = context_.record_keys();
    f.kit_pads = kit_pads();
    f.key_pads = key_pads();
    return f;
}

// A Record take from a synth track: its pads play, so Minus / Plus move their octave.
bool Shell::record_track_keys() const {
    if (!context_.record_keys() || ui_.settings.input.source != InputSource::Track) return false;
    auto engine = project_->tracks[ui_.settings.input.track % track_count].engine;
    return engine != EngineKind::Sampler;
}

// Whether the pads play the selected track's kit on this page, one pad each:
// there Erase + pad deletes a pad and Shift + Erase copies one.
bool Shell::kit_pads() const {
    const auto& t = project_->tracks[ui_.track % track_count];
    return t.engine == EngineKind::Sampler && !t.levels && pad_job(ui_) == play_pad;
}

// Whether the pads play the selected track's keys on this page, a synth's (or a MIDI or plug-in track's): there
// Erase + pad erases that key's notes and Shift + Erase copies them to another key. Not on 16 Levels.
bool Shell::key_pads() const {
    const auto& t = project_->tracks[ui_.track % track_count];
    return t.engine != EngineKind::Sampler && t.used() && !t.levels && t.layout != PadLayout::Levels && pad_job(ui_) == play_pad;
}

// Copying ends once its pads are gone: pads away from a kit, keys away from them, steps away from Steps.
void Shell::end_copy_away() {
    bool here = true; // tracks, patterns: on any page
    switch (ui_.copying) {
    case UiState::Copying::Pads: here = kit_pads(); break;
    case UiState::Copying::Keys: here = key_pads() || ui_.page == PageId::Roll || (ui_.page == PageId::StepEdit && !ui_.step_pads); break;
    case UiState::Copying::Steps: here = ui_.page == PageId::StepEdit && ui_.step_pads; break;
    case UiState::Copying::Notes: here = ui_.page == PageId::Roll; break;
    default: break;
    }
    if (ui_.copying != UiState::Copying::Off && !here) ui_.stop_copying();
}

void Shell::snapshot(Snapshot& out, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock);
    auto bindings = page_bindings(*project_, ui_, context_.pattern());
    for (unsigned n = 0; n < 3; ++n) if (bindings[n].bound()) check_pickup(n, bindings[n]);
    fader_check(context_);
    edit_loop_check(context_); // an editor's loop ends with the editor (shell/note_jobs.h)
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
    out.pass = host.pass_of(ui_.track);
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
        out.take.left = r.level_left(); out.take.right = r.level_right();
        for (unsigned c = 0; c < out.take.peaks.size(); ++c) out.take.peaks[c] = r.peak(c);
        out.render_state = uint8_t(host.bounce().state()); out.render_frames = host.bounce().frames();
        out.recall_frames = uint32_t(uint64_t(host.recall().filled()) * 44100 / std::max(1u, host.recall().rate())); // 44.1 kHz frames
        out.monitoring = host.monitoring();
        out.speaker_quieted = input_ && input_->speaker_quieted();
        out.usb_state = input_ ? uint8_t(input_->usb_state()) : 0;
    }
    for (unsigned n = 0; n < 16; ++n) out.held_keys[n] = context_.pad_keys[n][0];
    out.project_path = context_.project_path; out.project_folder = context_.project_folder; out.dirty = context_.dirty;
    if (store_ && (ui_.page == PageId::Project || ui_.exit_open)) store_->view(out.projects);
    if (context_.plugins && ui_.page == PageId::Plugins) out.plugins = context_.plugins->view();
    if (context_.plugins && ui_.page == PageId::PluginPresets) out.presets = context_.plugins->programs(ui_.presets_entry);
    if (context_.plugins && ui_.page == PageId::Stage) { // Stage PRESETS: the track's plug-in's
        const auto& t = project_->tracks[ui_.track % track_count];
        if (t.engine == EngineKind::Plugin && t.plugin < max_plugins && stage_page_now(ui_, t).kind == StageKind::PluginSound)
            out.presets = context_.plugins->programs(t.plugin);
    }
    if (library_) {
        const auto& t = project_->tracks[ui_.track];
        unsigned slot = ui_.page == PageId::Chop ? ui_.chop_sample : t.pads[t.last_pad % sampler_pads].sample;
        out.sample = slot < sample_slots ? library_->slot(slot) : LoadedSample {};
        out.zone_file = out.sample.instrument && ui_.page != PageId::Chop ? library_->zone_file(slot, ui_.zone) : std::string();
        // Trim and Chop zoomed in: the view's own peaks, from the audio (the overview's 512 are too few that close).
        FrameView view;
        if (ui_.page == PageId::Chop && ui_.chop_zoom) view = chop_view(*project_, ui_);
        if (ui_.page == PageId::Stage && ui_.trim_zoom && stage_page_now(ui_, t).kind == StageKind::Trim) view = trim_view(*project_, ui_);
        out.detail_from = out.detail_to = 0;
        uint8_t* const sides[2] = {out.detail_sides[0].data(), out.detail_sides[1].data()};
        if (view.width() && slot < sample_slots && library_->detail(slot, view.from, view.to, out.detail.data(), unsigned(out.detail.size()), sides)) {
            out.detail_from = view.from; out.detail_to = view.to;
        }
        out.pad_waves_shown = 0;
        if ((ui_.page == PageId::Pads || ui_.sample_held) && t.engine == EngineKind::Sampler) // pads B (and Sample held's): the bank's sixteen waveforms
            for (unsigned n = 0; n < 16; ++n) {
                const auto& pad = t.pads[t.bank % sampler_banks * 16 + n];
                if (pad.used() && library_->thumbnail(pad.sample, pad.start, pad.end, out.pad_waves[n].data(), 32))
                    out.pad_waves_shown = uint16_t(out.pad_waves_shown | 1u << n);
            }
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
            sketch.count = uint16_t(std::min<size_t>(pattern.events.size(), 0xffff));
            sketch.moves = uint16_t(std::min<size_t>(pattern.motion.size(), 0xffff));
            sketch.follows = pattern.follow.on(); // roadmap P5
            sketch.program = pattern.program;
            for (const auto& e : pattern.events) { // the first note of each of the first 64 sixteenths
                unsigned k = e.tick / sixteenth_ticks;
                if (k >= sketch.keys.size() || e.tick >= pattern.length) break;
                if (!sketch.keys[k]) sketch.keys[k] = e.key;
            }
        }
    out.freeze = context_.prints.freeze; // freeze plan F2c
    out.prints_kept = {};
    for (const auto& r : context_.prints.records) ++out.prints_kept[r.track % track_count];
    out.print = context_.printer ? context_.printer->progress() : PrintProgress {};
    out.live_load = context_.live_load;
    out.printing_tracks = host.printing_tracks();
    out.thawing = context_.thawing; // freeze plan F2d
    {
        unsigned t = ui_.track % track_count, live = (host.playing() ? unsigned(host.pattern_of(t)) : project_->tracks[t].pattern) % pattern_count;
        out.print_peaks_known = false;
        for (const auto& [n, peaks] : context_.print_peaks[t])
            if (n == live) { out.print_peaks = peaks; out.print_peaks_known = true; }
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
    { // roadmap P3, for remote checks: the voices heard now and their slots
        uint64_t slots = 0;
        unsigned heard = 0;
        for (unsigned n = 0; n < out.taps.count; ++n) {
            const auto& v = out.taps.voice[n];
            if (v.flags & (tap::Choked | tap::Waiting)) continue;
            ++heard;
            if (v.slot < 64) slots |= uint64_t(1) << v.slot;
        }
        voices_now.store(heard, std::memory_order_relaxed);
        voice_slots.store(slots, std::memory_order_relaxed);
    }
    out.heads = trail.heads();
    out.grains = trail.grains();
    out.compression = host.compression();
    {
        float level = -90, reduce = 0;
        host.compressor_activity(level, reduce);
        comp_trail.update(level, reduce, now);
        out.comp_trail = comp_trail.trail();
        comp_bar.update(level, reduce, out.ticks, project_->bar_ticks(), out.playing);
        out.comp_bar = comp_bar.bar(); out.comp_ghost = comp_bar.ghost(); out.comp_bar_at = comp_bar.at();
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
           " melodee_preset=" + std::to_string(((st >> 16) & 0x3ff) + 1) +
           " melodee_bank=" + std::string(1, char('A' + ((st >> 26) & 7))) +
           " melodee_pad_sample=" + std::to_string(int(pad_sample.load()) == 0xffff ? -1 : int(pad_sample.load())) +
           " melodee_pad_frames=" + std::to_string(pad_frames.load()) + " melodee_tempo=" + std::to_string(tempo_now.load()) +
           " melodee_punch=" + std::to_string(int(host.punching())) + " melodee_buttons=" + button_text() + " melodee_pads=" + lit_pads_text() + " melodee_meter=" + meter_text() + " melodee_meter_db=" + meter_db_text() +
           " melodee_rec=" + std::to_string(rec_state.load()) + " melodee_count_in=" + std::to_string(host.count_in()) +
           " melodee_position=" + std::to_string(host.position_of(pt >> 8)) +
           " melodee_timed=" + std::to_string(host.timed_kept()) + "/" + std::to_string(host.timed_missed()) + "/" +
           std::to_string(host.timed_worst_frames()) + // notes played by hand on their frame / after it / the latest by
           // Roadmap P1: ratchets' hits after their first; notes trig conditions let through / held back; Fill held.
           " melodee_hits=" + std::to_string(host.ratchet_hits()) + " melodee_trigs=" + std::to_string(host.trigs_kept()) + "/" +
           std::to_string(host.trigs_dropped()) + " melodee_fill=" + std::to_string(int(host.filling())) +
           " melodee_follows=" + std::to_string(follows_total()) + " melodee_stopped=" + std::to_string(host.stopped_tracks()) + // roadmap P5
           " melodee_capture=" + std::to_string(capture_kept.load()) +
           // A project load: the Loading sheet up (and its samples still to come), the audio thread's parks, the last build.
           " melodee_loading=" + std::to_string(loading_now.load() & 1) + "/" + std::to_string(loading_now.load() >> 1) +
           " melodee_parks=" + std::to_string(host.parks()) + " melodee_load_us=" + std::to_string(load_us_now.load()) +
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
           " melodee_plugins=" + (context_.plugins ? context_.plugins->summary() : std::string("none")) + p4_summary(host);
}

// Phase 10: undo (depth, redo waiting, the last step's name), Seq held, the sampler map, a pot gesture open.
std::string Shell::phase10_summary() const {
    uint32_t b = phase10_bits.load(std::memory_order_relaxed);
    std::string what;
    { std::lock_guard<std::mutex> guard(text_lock); what = undo_text; }
    for (auto& ch : what) if (ch == ' ') ch = '_';
    return " melodee_undo=" + std::to_string(b & 0xff) + " melodee_redo=" + std::to_string((b >> 8) & 1) +
           " melodee_undo_last=" + (what.empty() ? std::string("none") : what) + " melodee_seq_held=" + std::to_string((b >> 9) & 1) +
           " melodee_map=" + std::to_string((b >> 10) & 1) +
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
         + " melodee_rec_fx=" + std::to_string((k >> 2) & 1) + " melodee_knob_edit=" + std::to_string((k >> 3) & 1) // Knob FX C: 0 A, 1 B
         + " melodee_knob_entry=" + std::to_string((k >> 8) & 0xff)
         + " melodee_knob_kind=" + std::to_string((k >> 16) & 0xff) + " melodee_knob_cursor=" + std::to_string(k >> 24)
         + " melodee_knob_tracks=" + std::to_string(targets & 0xffff) + " melodee_knob_pads=" + std::to_string(targets >> 16)
         + " melodee_comp=" + std::to_string(c & 1) + " melodee_comp_color=" + std::to_string((c >> 1) & 1)
         + " melodee_comp_boost=" + std::to_string((c >> 2) & 0x3f) + " melodee_comp_attack=" + std::to_string((c >> 8) & 0xff)
         + " melodee_comp_release=" + std::to_string((c >> 16) & 0xff) + " melodee_comp_amount=" + std::to_string(c >> 24)
         + " melodee_comp_sidechain=" + std::to_string(side & 1) + " melodee_comp_key=" + std::to_string(((side >> 1) & 0x7f) + 1)
         + " melodee_comp_key_pad=" + std::to_string(((side >> 8) & 0xff) == MasterComp::whole_kit ? 0u : ((side >> 8) & 0xff) + 1)
         + " melodee_comp_reduction=" + text + padfx_summary() + fader_summary();
}

// Phase 8: the fader. melodee_fader_job: 0 off, 1 the track's target, 2 the page's own job;
// melodee_fader_kind: the card it drives (Binding::Kind), melodee_fader_value its value; takeovers 0 Pickup, 1 Scaled, 2 Instant.
std::string Shell::fader_summary() const {
    uint32_t f = fader_bits.load(std::memory_order_relaxed), v = fader_value.load(std::memory_order_relaxed);
    return " melodee_fader_target=" + std::to_string(f & 0xf) + " melodee_fader_on=" + std::to_string((f >> 4) & 1)
         + " melodee_fader_learned=" + std::to_string((f >> 14) & 1) + " melodee_fader_pin=" + std::to_string((f >> 15) & 1) // fader C
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
         + " melodee_punch_slots=" + kinds + " melodee_live=" + std::to_string(host.live_punches()) + " melodee_kept=" + std::to_string(host.kept_punches())
         + " melodee_flex=" + std::to_string(host.flex_playing()) + " melodee_flex_mode=" + std::to_string((b >> 20) & 1)
         + " melodee_flex_quantize=" + std::to_string((b >> 21) & 1) + " melodee_flex_mix=" + std::to_string((b >> 22) & 0x7f)
         + " melodee_punch_quantize=" + std::to_string((b >> 29) & 1);
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
         + " melodee_recall_frames=" + std::to_string(uint64_t(host.recall().filled()) * 44100 / std::max(1u, host.recall().rate()))
         + " melodee_render=" + renders[unsigned(host.bounce().state()) % 7] + " melodee_render_frames=" + std::to_string(host.bounce().frames())
         + " melodee_rendering=" + std::to_string((r >> 21) & 3) + " melodee_resample_pick=" + std::to_string((r >> 23) & 1);
}

// Phase 4: the last pad's Sample Mode values, for remote checks.
std::string Shell::pad_summary() const {
    uint32_t b = pad_bits.load(std::memory_order_relaxed);
    static const char* const kinds[] {"velocity", "scale", "tune", "filter", "loop", "zones", "-", "-"};
    return " melodee_last_pad=" + std::to_string(((b >> 8) & 0xff) + 1) + " melodee_pad_loop=" + std::to_string(b & 1) +
           " melodee_pad_lock=" + std::to_string((b >> 1) & 1) + " melodee_pad_note_on=" + std::to_string((b >> 2) & 1) +
           " melodee_pad_reverse=" + std::to_string((b >> 3) & 1) + " melodee_pad_mono=" + std::to_string((b >> 4) & 1) +
           " melodee_pad_filter=" + pad_filter_name(PadFilter((b >> 16) & 0xf)) + " melodee_pad_link=" + std::to_string((b >> 20) & 0x1f) +
           " melodee_pad_decay_from=" + ((b >> 25) & 1 ? "start" : "end") +
           " melodee_pad_gain=" + std::to_string(pad_gain.load(std::memory_order_relaxed)) +
           " melodee_levels=" + std::to_string((b >> 5) & 1) + " melodee_levels_kind=" + kinds[(b >> 26) & 7] + p3_summary();
}

// Roadmap P3: the last pad's layers and loop crossfade, the voices heard and their sample slots.
unsigned Shell::follows_total() const {
    unsigned n = 0;
    for (unsigned t = 0; t < track_count; ++t) n += host.follows_of(t);
    return n;
}

std::string Shell::p3_summary() const {
    uint32_t p = p3_bits.load(std::memory_order_relaxed);
    static const char* const plays[] {"velocity", "cycle", "random"};
    std::string slots;
    uint64_t mask = voice_slots.load(std::memory_order_relaxed);
    for (unsigned n = 0; n < 64; ++n) if ((mask >> n) & 1) slots += (slots.empty() ? "" : ",") + std::to_string(n);
    return " melodee_pad_layers=" + std::to_string(p & 15) + " melodee_layer_play=" + plays[((p >> 4) & 15) % 3] +
           " melodee_pad_xfade=" + std::to_string((p >> 8) & 0xff) + " melodee_voices=" + std::to_string(voices_now.load(std::memory_order_relaxed)) +
           " melodee_voice_slots=" + (slots.empty() ? "-" : slots);
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
    uint64_t rungs = pad_rungs.load(std::memory_order_relaxed), hues = pad_hues.load(std::memory_order_relaxed);
    std::string out;
    for (unsigned n = 0; n < 16; ++n) { // pad:hue rung, as 3:t4 (shell/pad_light.h: rung 1 faint .. 4 full; hue t, l, w, r, o)
        unsigned rung = unsigned(rungs >> (4 * n)) & 15;
        if (!rung) continue;
        if (!out.empty()) out += ',';
        out += std::to_string(n + 1) + ':' + pad_light::hue_letter(pad_light::Hue(unsigned(hues >> (4 * n)) & 15)) + std::to_string(rung);
    }
    return out.empty() ? "none" : out;
}

// The meter lights beside the screen: left and right as masks (bit 0 the bottom LED), and what they show.
std::string Shell::meter_text() const {
    uint32_t bits = meter_bits.load(std::memory_order_relaxed);
    return std::to_string(bits & 0xff) + ',' + std::to_string(bits >> 8 & 0xff) + ',' + (bits >> 16 & 1 ? "in" : "out");
}

// The levels the meters show now, left and right, in dBFS (-99 for silence).
std::string Shell::meter_db_text() const {
    bool input = meter_bits.load(std::memory_order_relaxed) >> 16 & 1;
    std::string out;
    for (unsigned side = 0; side < surface_meter_lights; ++side) {
        float level = input ? host.in_level(side) : host.out_level(side);
        char text[16];
        std::snprintf(text, sizeof text, "%.1f", level > 0 ? std::max(-99.0f, 20 * std::log10(level)) : -99.0f);
        out += (side ? "," : "") + std::string(text);
    }
    return out;
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
