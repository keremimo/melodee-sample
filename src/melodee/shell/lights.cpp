// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad, fader and button lights for the page in view (MIDI thread, every
// 5 ms; the runtime sends only what changed). Colours come from the track
// palette; button light values from surface/leds.h.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project_file.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/model/scales.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_lights.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/shell.h"
#include <filesystem>
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/step_edit.h"
#include "melodee/ui/tokens.h"
#include "surface/led_colours.h"
#include <algorithm>
#include <cstdio>

namespace melodee {

// Updates from the card (shell/update.h). Opens the update question when the service thread found a newer
// release, playback is stopped and no other sheet is open. After INSTALL (shell/ask.cpp), waits for the project
// save, then asks the service thread to start the install, and copies its progress to the sheet. Also shows
// notices from the service thread ("Updated to v0.2.0").
void Shell::update_tick() {
    std::string text;
    if (update_link.take_note(text)) context_.notice(text);
    if (update_link.take_offer(text)) update_offer = text;
    if (!update_offer.empty() && !update_link.offering()) update_offer.clear(); // zip no longer on the card
    auto& u = ui_.update;
    if (!update_offer.empty() && !u.phase && ui_.ask == UiState::Ask::None && !ui_.exit_open && !ui_.name.open()
        && !ui_.loading && !card_owns_controls(ui_) && !host.playing() && !ui_.recording) {
        std::snprintf(u.version, sizeof u.version, "%s", update_offer.c_str());
        update_offer.clear();
        ui_.shift = false;
        ask(context_, UiState::Ask::InstallUpdate, 0);
    }
    if (u.phase == uint8_t(UpdatePhase::Saving) && (!store_ || int(store_->saves()) >= u.after_save)
        && !(library_ && library_->loops_busy())) {
        u.phase = uint8_t(UpdatePhase::Checking);
        update_link.install();
    }
    if (u.phase >= uint8_t(UpdatePhase::Checking) && update_link.phase() != UpdatePhase::None) {
        u.phase = uint8_t(update_link.phase());
        u.error = uint8_t(update_link.error());
    }
}

void Shell::lights(SurfaceLights& lights, uint32_t now) {
    std::lock_guard<std::mutex> guard(lock); // the screen thread may be copying the state
    { // SD card access: the service thread's view of the card. Lent from anywhere (the remote too): the page
      // opens and owns the controls; back: Project again, its lists read anew.
        CardView view;
        view.state = card_link.state.load(); view.connected = card_link.connected.load(); view.ejected = card_link.ejected.load();
        view.locked = card_link.locked.load(); view.note = CardNote(card_link.note.load()); view.returns = card_link.returns.load();
        auto change = card_follow(ui_, view);
        if (change.left && ui_.page != PageId::CardAccess) { ui_.shift = false; ui_.ask = UiState::Ask::None; context_.open(PageId::CardAccess); }
        if (change.back && ui_.page == PageId::CardAccess) { context_.open(PageId::Project); context_.notice("Card back"); }
    }
    update_tick();
    // Power was pressed: the Exit sheet opens (the service thread only counts presses).
    // The library found files or finished a sample: the lengths go into the project.
    if (library_ && library_->generation() != library_seen) {
        library_seen = library_->generation();
        for (unsigned n = 0; n < sample_slots; ++n)
            if (project_->samples[n].used()) {
                const auto shown = library_->slot(n);
                project_->samples[n].frames = shown.frames;
                project_->samples[n].zones = uint16_t(std::min<size_t>(shown.shapes.size(), 0xffff));
            }
        ui_.files_count = uint16_t(std::min<size_t>(library_->files().size(), 0xffff));
        if (ui_.page == PageId::Chop && !ui_.slices.count) context_.rechop();
    }
    context_.files_landed();  // Trim Sample, Extract, Save Sample, takes, Recall, Resample: done on the service thread
    context_.record_tick(now); // Input Config's route and monitor, settings kept, renders followed
    context_.browser_tick();  // the browser's listings and previews
    // The device's rate changed: the project again, worked out at the new rate.
    if (unsigned changes = host.format_changes(); changes != format_seen && context_.reformat()) format_seen = changes;
    // Projects: a finished pack to free, a loaded project to switch to, a save to leave after.
    {
        const ProjectPack* done = nullptr;
        while (host.take_done_pack(done)) delete done;
        context_.free_retired(); // pattern copies played until a newer one came
        const TrackPack* track_done = nullptr; // a track copy, or its undo
        while (host.take_done_track(track_done)) delete track_done;
    }
    if (store_) {
        if (auto loaded = store_->take_loaded()) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            unsigned version = 0;
            PluginStates plugin_states;
            if (loaded->error.empty() && load_project(loaded->bytes, *project, *bank, &version, &plugin_states).empty()) {
                for (auto& t : project->tracks) upgrade_params(t.engine, version, t.params); // older files: pad expression at its defaults
                if (!context_.kit_from.empty() && context_.kit_from == loaded->path) // Load Kit: only its kit, onto the selected track
                    context_.load_kit(*project, std::filesystem::path(loaded->path).stem().string());
                else if (loaded->imported) { context_.switch_to(*project, *bank, {}); context_.dirty = true; context_.notice(loaded->note); } // not saved yet: the .xpj stays as it is
                else context_.switch_to(*project, *bank, loaded->path, &plugin_states);
            } else if (!loaded->note.empty()) context_.notice(loaded->note);
            if (context_.kit_from == loaded->path) context_.kit_from.clear();
        }
        if (leave_after_save >= 0 && int(store_->saves()) >= leave_after_save && !(library_ && library_->loops_busy())) { // loop files too
            if (!off_after_save) leave.store(true);
            else if (!ui_.exit_saved) { ui_.exit_saved = true; off_saved_at = now; } // the sheet says it is saved, then off
        }
    }
    context_.now = now;
    context_.reload_tick(); // a project parked for: built once the audio thread is still; the Loading sheet till it is in
    if (ui_.exit_saved && now - off_saved_at >= 1000) power_off.store(true); // a second to read that it is saved
    context_.song_tick();
    context_.plugins_tick(now); // the rack follows the project; the plug-in in view's cards
    context_.stage_tick(now); // Stage's PRESET: the sound turned to, once the turning stops
    context_.chain_tick();    // pattern chains: the next queued in the last bar of the one playing (seq-sheet2 B)
    context_.follow_tick();   // roadmap P5: chains told to the audio thread, follow actions that came followed
    context_.capture_tick();  // roadmap P5: the transport's runs, for what Capture keeps
    // Sounds of one's own, when the card's list changes.
    if (store_ && store_->generation() != sounds_seen) {
        sounds_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        context_.user_sounds = std::move(v.sounds);
        if (!v.moved_from.empty() && v.moved_from == context_.project_path) context_.project_path = v.moved_to; // the open project followed its file
    }
    ui_.user_sound_count = uint16_t(context_.user_sound_count(project_->tracks[ui_.track].engine));
    // Melodee's settings, once the card's file has been read.
    if (store_ && !settings_loaded && store_->generation() != settings_seen) {
        settings_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        if (v.settings_read) { ui_.settings.read(v.settings); settings_loaded = true; context_.send_click(); }
    }
    // The backlight stays as the stock app left it until Screen asks for less (and then follows it).
    if (ui_.settings.screen != 100 || screen_wanted.load(std::memory_order_relaxed)) screen_wanted.store(ui_.settings.screen, std::memory_order_relaxed);
    want_audio_format();
    // The Project page's row count, for K2's range.
    if (store_ && ui_.page == PageId::Project && store_->generation() != store_seen) {
        store_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        ui_.project_rows = uint16_t(project_rows(v.entries.size(), context_.project_folder));
    }
    // Slots a Load Kit or Load Bank named past what the queue holds, as it drains.
    while (library_ && !context_.slots_waiting.empty()) {
        unsigned slot = context_.slots_waiting.front();
        const auto& ref = project_->samples[slot % sample_slots];
        if (ref.used() && !library_->load(slot, ref.path)) break;
        context_.slots_waiting.erase(context_.slots_waiting.begin());
    }
    // After a switch, every sample slot loads again (or empties), a few each pass.
    while (library_ && context_.samples_to_load < sample_slots) {
        const auto& ref = project_->samples[context_.samples_to_load];
        bool real = ref.used() && ref.path[0] == '/';
        if (!library_->load(context_.samples_to_load, real ? ref.path : "")) break;
        ++context_.samples_to_load;
    }
    // Tap Tempo held half a second: the Tempo page.
    if (tap_held && now - tap_down > 500 && ui_.page != PageId::Tempo) { tap_held = false; context_.open(PageId::Tempo); }
    context_.now = now;
    looper_tick(context_, now); // Loop held, a clear pad held, refusals, the metronome (shell/looper_jobs.cpp)
    unsigned presses = power.load(std::memory_order_relaxed);
    if (presses != power_seen) { // a second press while it is open does nothing more
        power_seen = presses;
        if (!ui_.exit_open) ui_.exit_pick = 0; // the wheel starts on STAY
        ui_.exit_open = true;
    }

    const auto& ui = ui_;
    {
        const auto& t = project_->tracks[ui_.track];
        const auto& pad = t.pads[t.last_pad % sampler_pads];
        page_track.store(uint32_t(ui_.page) | uint32_t(ui_.track) << 8, std::memory_order_relaxed);
        { // Stage: the button and page in view, the card the wheel edits, a sound waiting to load
            const auto& t = project_->tracks[ui_.track];
            auto at = stage_at(ui_, t);
            stage_bits.store(at.group | at.sub << 4 | unsigned(stage_focus(ui_, t)) << 8 | unsigned(ui_.preset_due != 0) << 12
                             | unsigned(t.preset & 0x3ff) << 16 | unsigned(t.bank % sampler_banks) << 26,
                             std::memory_order_relaxed);
            std::lock_guard<std::mutex> text(text_lock);
            stage_key_text = stage_page_now(ui_, t).key;
        }
        // The tempo in play: MIDI clock in's while it leads (Sync In), else the project's.
        tempo_now.store(ui_.settings.midi.sync_in == MidiSync::Clock && midi_io.tempo_in() ? host.tempo_now() : context_.tempo_now(),
                        std::memory_order_relaxed);
        const auto& shown = context_.pattern();
        rec_state.store(ui_.recording ? 2 : ui_.rec_armed ? 1 : 0, std::memory_order_relaxed);
        pattern_ticks.store(shown.length, std::memory_order_relaxed);
        pattern_events.store(uint32_t(shown.events.size()), std::memory_order_relaxed);
        capture_kept.store(context_.capture_ring->count(), std::memory_order_relaxed); // roadmap P5
        loading_now.store(uint32_t(ui_.loading) | uint32_t(ui_.loading_left) << 1, std::memory_order_relaxed); // parked loads
        load_us_now.store(context_.load_us, std::memory_order_relaxed);
        pattern_q.store(shown.q(), std::memory_order_relaxed);
        first_tick.store(shown.events.empty() ? 0 : shown.events.front().tick, std::memory_order_relaxed);
        { // Step Edit: the step, the selected event and how many the step holds; the open question; motion events
            auto in = step_events(shown, unsigned(std::max<int>(ui_.step_cursor, 0)) % shown.steps());
            int chosen = selected_event(shown, ui_);
            step_now.store(uint32_t(std::max<int>(ui_.step_cursor, 0)), std::memory_order_relaxed);
            event_now.store(chosen >= 0 ? uint32_t(chosen) - uint32_t(in.first) + 1 : 0, std::memory_order_relaxed);
            step_events_now.store(in.count, std::memory_order_relaxed);
            event_tick.store(chosen >= 0 ? shown.events[size_t(chosen)].tick : 0, std::memory_order_relaxed);
            ask_now.store(uint32_t(ui_.ask), std::memory_order_relaxed);
            motion_now.store(uint32_t(shown.motion.size()), std::memory_order_relaxed); // the pattern's Motion points (roadmap P1)
            if (ui_.page == PageId::Roll) { // the view as the page draws it
                UiState roll = ui_;
                roll_follow(shown, roll);
                roll_key_now.store(roll.roll_key, std::memory_order_relaxed);
                roll_focus_now.store(roll_focus(shown, roll) >= 0 ? 1 : 0, std::memory_order_relaxed);
                roll_view_now.store(roll_first(shown, roll), std::memory_order_relaxed);
                roll_top_now.store(roll.roll_top, std::memory_order_relaxed);
            }
            sheet_now.store(ui_.sheet_open ? 1 : 0, std::memory_order_relaxed);
        }
        auto sig = project_->time_signature();
        signature_now.store(sig.beats * 100u + sig.unit, std::memory_order_relaxed);
        pad_sample.store(pad.sample, std::memory_order_relaxed);
        { // phase 4: the last pad's Sample Mode switches, gain, link and filter; 16 Levels' type
            pad_bits.store(uint32_t(pad.loop) | uint32_t(pad.loop_lock) << 1 | uint32_t(pad.play == PadPlay::Gate) << 2 | uint32_t(pad.reverse) << 3
                           | uint32_t(pad.mono) << 4 | uint32_t(t.levels) << 5 | uint32_t(t.levels_kind % 4) << 6 | uint32_t(t.last_pad) << 8
                           | uint32_t(pad.filter) << 16 | uint32_t(pad.link) << 20 | uint32_t(pad.decay_from_start) << 25, std::memory_order_relaxed);
            pad_gain.store(pad.gain, std::memory_order_relaxed);
            unsigned layers = 0;
            for (unsigned n = 0; n < pad_layers; ++n) layers += pad.layer(n).used();
            p3_bits.store(layers | uint32_t(pad.layer_play) << 4 | uint32_t(pad.loop_xfade) << 8, std::memory_order_relaxed); // roadmap P3
        }
        pad_frames.store(pad.used() ? project_->samples[pad.sample % sample_slots].frames : 0, std::memory_order_relaxed);
        { // phase 6: Input Config, the take, renders
            const auto& in = ui_.settings.input;
            record_bits.store(uint32_t(in.source) | uint32_t(in.monitor) << 4 | uint32_t(ui_.rec_pad + 1) << 8 | uint32_t(ui_.rec_chops & 0x1f) << 16
                              | uint32_t(ui_.rendering & 3) << 21 | uint32_t(ui_.resample_pick) << 23 | uint32_t(uint8_t(in.threshold)) << 24,
                              std::memory_order_relaxed);
        }
        { // phase 7: Knob FX (mode, bypass, entry, kind; tracks and the bank's pads on), the compressor
            const auto& k = project_->knob_fx;
            knob_bits.store(uint32_t(k.on) | uint32_t(k.bypass) << 1 | uint32_t(ui_.settings.input.fx) << 2 | uint32_t(k.editing) << 3 | uint32_t(k.choice) << 8
                            | uint32_t(k.kind) << 16 | uint32_t(ui_.knob_cursor) << 24, std::memory_order_relaxed);
            unsigned first = t.bank % sampler_banks * 16;
            uint32_t bank_pads = 0;
            for (unsigned n = 0; n < 16; ++n) bank_pads |= uint32_t(k.pad_on(ui_.track, first + n)) << n;
            knob_targets.store(uint32_t(k.tracks) | bank_pads << 16, std::memory_order_relaxed);
            const auto& c = project_->compressor;
            comp_bits.store(uint32_t(c.on) | uint32_t(c.color) << 1 | uint32_t(c.boost) << 2 | uint32_t(c.attack) << 8 | uint32_t(c.release) << 16
                            | uint32_t(c.amount) << 24, std::memory_order_relaxed);
            comp_side.store(uint32_t(c.sidechain) | uint32_t(c.key) << 1 | uint32_t(c.key_pad) << 8, std::memory_order_relaxed);
        }
        { // phase 7b: Pad FX (bank, engaged, latched, selected; the slots' kinds + 1) and Flex Beat's settings
            unsigned latched = 0;
            for (unsigned n = 0; n < ui_.engaged_count; ++n) latched += ui_.engaged[n].latched;
            padfx_bits.store(uint32_t(project_->punch_bank % 4) | uint32_t(ui_.engaged_count & 0x1f) << 2 | uint32_t(latched & 0x1f) << 7
                             | uint32_t(uint8_t(ui_.punch_selected + 1)) << 12 | uint32_t(project_->flex.mode) << 20
                             | uint32_t(project_->flex.quantize) << 21 | uint32_t(project_->flex.mix) << 22
                             | uint32_t(project_->punch_quantize) << 29, std::memory_order_relaxed);
            uint32_t slots = 0;
            for (unsigned n = 0; n < 4; ++n) slots |= uint32_t(uint8_t(ui_.punch_slot[n] + 1)) << (8 * n);
            punch_slots_bits.store(slots, std::memory_order_relaxed);
        }
        { // phase 8: the fader (its target, on, what drives it here, takeover met; the takeover modes, the light, the
          // track's mod wheel) and the value it drives with that card's kind
            auto job = fader_job(*project_, ui_);
            unsigned light = unsigned(fader_light_level(job) * 100 + 0.5f);
            fader_bits.store(uint32_t(fader_target(t)) | uint32_t(t.fader_on) << 4 | uint32_t(job.source) << 5 | uint32_t(ui_.fader.engaged) << 7
                             | uint32_t(ui_.settings.knob_takeover) << 8 | uint32_t(ui_.settings.knob_fx_takeover) << 10
                             | uint32_t(ui_.settings.fader_takeover) << 12 | uint32_t(job.learned) << 14 | uint32_t(t.fader_pin) << 15 // fader C
                             | uint32_t(light & 0x7f) << 16 | uint32_t(t.wheel & 0x7f) << 24,
                             std::memory_order_relaxed);
            fader_value.store(uint32_t(uint16_t(int16_t(job.binding.value))) | uint32_t(job.binding.kind) << 16, std::memory_order_relaxed);
            fader_at.store(ui_.fader.position, std::memory_order_relaxed);
        }
        { // phase 9: MIDI Config, the track's own channel, the row under the cursor
            const auto& m = ui_.settings.midi;
            midi_bits.store(uint32_t(m.port) | uint32_t(m.in_channel) << 1 | uint32_t(m.out_channel) << 6 | uint32_t(m.pad_in) << 11
                            | uint32_t(m.pad_out) << 12 | uint32_t(m.sync_in) << 14 | uint32_t(m.sync_out) << 16 | uint32_t(m.thru) << 18
                            | uint32_t(m.program_change) << 19 | uint32_t(m.cv_out) << 20 | uint32_t(m.cv_ppqn() & 255) << 21,
                            std::memory_order_relaxed);
            track_channel.store(t.midi_channel, std::memory_order_relaxed);
            midi_row.store(ui_.midi_cursor, std::memory_order_relaxed);
        }
        { // phase 5: Chop, the region and its file, the browser
            unsigned used = 0;
            for (const auto& ref : project_->samples) used += ref.used();
            bool taking = ui_.chop_take_at && now - ui_.chop_take_at < 60000;
            chop_bits.store(uint32_t(ui_.chop_mode) | uint32_t(ui_.chop_armed) << 2 | uint32_t(taking) << 3 | uint32_t(ui_.slices.count & 0xff) << 8
                            | uint32_t(ui_.chop_slice) << 16 | uint32_t(used) << 24, std::memory_order_relaxed);
            pad_start_now.store(pad.start, std::memory_order_relaxed); pad_end_now.store(pad.end, std::memory_order_relaxed);
            files_bits.store(uint32_t(ui_.files_count) | uint32_t(ui_.files_cursor & 0x3fff) << 16 | uint32_t(ui_.files_drive & 1) << 30
                             | uint32_t(ui_.preview) << 31, std::memory_order_relaxed);
            std::string folder = library_ ? library_->folder() : std::string();
            std::lock_guard<std::mutex> guard(text_lock);
            pad_name_text = pad.used() ? project_->samples[pad.sample % sample_slots].name : "";
            folder_text = folder.empty() ? folder : folder.substr(folder.find_last_of('/') + 1);
            const auto* last = context_.edits.last();
            undo_text = last ? last->what : std::string();
        }
        // phase 10: the undo history's depth and the redo side, Seq held, the sampler map, a gesture open
        phase10_bits.store(uint32_t(context_.edits.size()) | uint32_t(context_.edits.can_redo()) << 8 | uint32_t(ui_.seq_held) << 9
                           | uint32_t(ui_.map_open) << 10 | uint32_t(bool(context_.gesture.edit)) << 12
                           | uint32_t(project_->song.count & 0xfff) << 16 | uint32_t(project_->tracks[ui_.track].engine) << 28,
                           std::memory_order_relaxed);
        if (context_.gesture.edit && now - context_.gesture.last >= gesture_quiet_ms) context_.close_gesture(); // a pot left still
    }
    const auto& project = *project_;
    const auto& track = project.tracks[ui.track];
    bool playing = host.playing();
    uint32_t ticks = host.ticks();
    { // The pads (shell/pad_lights.cpp): they light as what a press does there, chosen as Shell::hit() chooses it.
        unsigned beat = host.count_in();
        if (beat != count_beat_seen) { count_beat_seen = beat; count_beat_at = beat ? now : 0; }
        host.watch_taps(int(ui.track % track_count)); // the screen asks the same: the selected track's voices
        TrackTaps taps;
        if (!host.taps(taps) || taps.track != ui.track % track_count) taps = TrackTaps {};
        PadScene scene {ui, project, *patterns_, host, taps, pad_facts(), now, &pad_let_go, count_beat_at};
        std::array<pad_light::Look, 16> looks;
        pad_looks(scene, looks);
        uint64_t rungs = 0, hues = 0;
        for (unsigned pad = 0; pad < 16; ++pad) {
            lights.pad(pad, looks[pad].rgb);
            if (!looks[pad].lit()) continue;
            rungs |= uint64_t(pad_light::rung(looks[pad].level)) << (4 * pad);
            hues |= uint64_t(looks[pad].hue) << (4 * pad);
        }
        pad_rungs.store(rungs, std::memory_order_relaxed);
        pad_hues.store(hues, std::memory_order_relaxed);
    }
    { // The fader light shows the value it drives (brightest at the middle for pan and tune), dark with nothing to drive;
      // white where that value is the master's (Compressor, Flex Beat, Knob FX).
        auto job = fader_job(project, ui);
        float level = fader_light_level(job);
        bool master = job.source == FaderJob::Source::Page
                   && (ui.page == PageId::Compressor || ui.page == PageId::FlexBeat || ui.page == PageId::KnobFx);
        lights.fader(level <= 0 ? Rgb {} : master ? led_colour::white(level) : pad_light::track(ui.track, level).rgb);
    }
    // Bank lights A-H: with Seq held, the bank of patterns in view; else the sampler track's bank in view, or the Drums
    // pads' (A-D).
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank)
        lights.bank(bank, ui.seq_held ? bank == ui.pattern_bank % pattern_banks
                                      : (track.engine == EngineKind::Sampler && bank == track.bank % sampler_banks) || (drum_pads(track) && bank == drum_bank(track)));
    { // The meters beside the screen, as on the MPC: the input source while Record or Input Config is in view or a
      // take waits or runs (what is about to be sampled), else main out.
        using S = TakeRecorder::State;
        auto take = host.recorder().state();
        bool input = ui.page == PageId::Record || ui.page == PageId::InputConfig || take == S::Armed || take == S::Cued
                  || take == S::Recording;
        uint32_t bits = uint32_t(input) << 16;
        for (unsigned side = 0; side < surface_meter_lights; ++side) {
            uint8_t lit = input ? meter_lit(host.in_level(side), meter_input_db) : meter_lit(host.out_level(side), meter_mix_db);
            lights.meter(side, lit);
            bits |= uint32_t(lit) << (8 * side);
        }
        meter_bits.store(bits, std::memory_order_relaxed);
    }

    using B = SurfaceButton;
    using L = ButtonLight;
    // As on the stock MPC: bright in use, faint where the button has a job here, dark where it has none.
    bool erase_pots = false;
    {
        auto bindings = page_bindings(project, ui, context_.pattern());
        for (const auto& b : bindings) erase_pots |= b.kind == Binding::Kind::Param;
    }
    bool kit = kit_pads();
    uint64_t levels = 0; // two bits a button, for `status`
    auto set = [&](B button, L light) {
        lights.button(button, light);
        levels = (levels & ~(uint64_t(3) << (2 * unsigned(button)))) | uint64_t(light) << (2 * unsigned(button)); // the last set wins
    };
    auto job = [&](B button) { return button_has_job(project, ui, button, kit, erase_pots); };
    auto on = [&](B button, bool lit) { set(button, lit && job(button) ? L::Bright : job(button) ? L::Dim : L::Off); };
    if (card_owns_controls(ui)) { // SD card access: every mode waits; only the transport answers (B1-B3 have no lights)
        for (B button : {B::Sample, B::Seq, B::PadFx, B::KnobFx, B::Shift, B::PadBank, B::Erase, B::NoteRepeat, B::Chop, B::Mute,
                         B::Loop, B::SixteenLevels, B::SampleSelect, B::TapTempo, B::Minus, B::Plus, B::SampleRecord, B::SeqRecord, B::WheelPush})
            set(button, L::Off);
        set(B::Play, playing ? L::Bright : L::Dim);
        set(B::Stop, L::Dim);
        button_levels.store(levels, std::memory_order_relaxed);
        return;
    }
    if (ui.seq_held && !ui.name.open() && !ui.exit_open && ui.ask == UiState::Ask::None) {
        // Seq held (shell/seq_layer.h): Seq bright, the buttons with a job in its layer faint (B1-B3, Shift and
        // Minus / Plus have no lights), the transport as ever.
        for (B button : {B::Sample, B::PadFx, B::KnobFx, B::Shift, B::PadBank, B::NoteRepeat, B::Chop, B::Mute,
                         B::Loop, B::SixteenLevels, B::SampleSelect, B::TapTempo, B::Minus, B::Plus, B::SampleRecord})
            set(button, L::Off);
        set(B::Seq, L::Bright);
        set(B::Erase, ui.erase || ui.copying == UiState::Copying::Patterns ? L::Bright : L::Dim);
        set(B::WheelPush, L::Dim);
        if (ui.rec_armed || (ui.recording && host.count_in())) set(B::SeqRecord, (now / 250) % 2 ? L::Bright : L::Dim);
        else set(B::SeqRecord, ui.recording ? L::Bright : L::Dim);
        set(B::Play, playing ? L::Bright : L::Dim);
        set(B::Stop, L::Dim);
        button_levels.store(levels, std::memory_order_relaxed);
        return;
    }
    if (ui.shift && !ui.name.open() && !ui.exit_open) { // as Akai's guide has it: the Shift jobs faint, bright while on
        for (B button : {B::Sample, B::Seq, B::PadFx, B::KnobFx, B::Shift, B::PadBank, B::Erase, B::NoteRepeat, B::Chop, B::Mute,
                         B::Loop, B::SixteenLevels, B::SampleSelect, B::TapTempo, B::Minus, B::Plus, B::SampleRecord, B::SeqRecord,
                         B::Stop, B::Play, B::WheelPush}) {
            bool shift_job = button_has_shift_job(project, ui, button, kit);
            bool lit = button == B::Shift || (button == B::TapTempo && ui.settings.click) || (button == B::Erase && ui.copying != UiState::Copying::Off)
                    || (button == B::NoteRepeat && track.arp) || (button == B::Mute && ui.mute_mode())
                    || (track.engine == EngineKind::Sampler && button == B::Loop && track.pads[track.last_pad % sampler_pads].reverse)            // reverse on
                    || (track.engine == EngineKind::Sampler && button == B::Chop && track.pads[track.last_pad % sampler_pads].play == PadPlay::Gate); // Note On
            set(button, !shift_job ? L::Off : lit ? L::Bright : L::Dim);
        }
        button_levels.store(levels, std::memory_order_relaxed);
        return;
    }
    bool play_group = ui.page == PageId::Stage || ui.page == PageId::Tracks || ui.page == PageId::Keys || ui.page == PageId::Pads;
    on(B::Sample, play_group || ui.page == PageId::InputConfig);
    on(B::Seq, ui.page == PageId::Patterns || ui.page == PageId::Song || ui.page == PageId::StepEdit || ui.page == PageId::Steps
                   || ui.page == PageId::Roll || ui.page == PageId::Motion);
    on(B::SampleSelect, ui.page == PageId::Sounds || ui.page == PageId::Samples);
    on(B::Chop, ui.page == PageId::Chop);
    on(B::NoteRepeat, track.arp && ui.page != PageId::Song); // on Song it repeats the clip
    {
        auto state = host.recorder().state();
        bool blink = (now / 250) % 2;
        set(B::SampleRecord, state == TakeRecorder::State::Recording ? L::Bright
                      : state == TakeRecorder::State::Armed || state == TakeRecorder::State::Cued ? (blink ? L::Bright : L::Dim)
                      : job(B::SampleRecord) ? L::Dim : L::Off);
    }
    on(B::PadFx, ui.page == PageId::Punch || ui.page == PageId::PadPunch || ui.page == PageId::FlexBeat || ui.engaged_count || host.flex_playing());
    on(B::KnobFx, project.knob_fx.on); // the mode, as on the MPC (Shift + Knob FX opens the chains)
    on(B::Loop, ui.page == PageId::Looper || ui.loop_held || (ui.page == PageId::Song ? project.song.looping()
                    : track.engine == EngineKind::Sampler && track.pads[track.last_pad % sampler_pads].loop)); // the song's brace on Song, else the last pad loops; the Looper
    // 16 Levels: lit while the pads play levels (a kit pad or a synth's last note on all sixteen, or a synth's Levels layout).
    on(B::SixteenLevels, ui.page == PageId::Levels || track.levels || (track.engine != EngineKind::Sampler && track.layout == PadLayout::Levels));
    on(B::Play, playing);
    on(B::Stop, false);
    // Seq Record: bright while recording, flashing while armed (Play starts the take) or counting in.
    if (ui.rec_armed || (ui.recording && host.count_in())) set(B::SeqRecord, (now / 250) % 2 ? L::Bright : L::Dim);
    else on(B::SeqRecord, ui.recording);
    // Tap Tempo flashes on the beat while playing.
    unsigned beat = project.beat_ticks();
    on(B::TapTempo, playing && ticks % beat < beat / 3);
    on(B::Mute, ui.mute_mode());           // bright while mute mode is on, latched or held
    on(B::PadBank, tracks_on_pads(ui));    // held, or a track copy under way
    on(B::Erase, ui.erase || ui.copying != UiState::Copying::Off); // lit while copying
    button_levels.store(levels, std::memory_order_relaxed);
}

} // namespace melodee
