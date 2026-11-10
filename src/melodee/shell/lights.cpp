// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad, fader and button lights for the page in view (MIDI thread, every
// 5 ms; the runtime sends only what changed). Colours come from the track
// palette; button light values from surface/leds.h.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project_file.h"
#include "melodee/model/scales.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/pad_bank.h"
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
namespace {
Rgb track_light(unsigned track, float level) {
    uint32_t c = ui::track_shades[track % track_count][0];
    return led_colour::rgb(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, level);
}
} // namespace

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
    // Power was pressed: the Exit sheet opens (the service thread only counts presses).
    // The library found files or finished a sample: the lengths go into the project.
    if (library_ && library_->generation() != library_seen) {
        library_seen = library_->generation();
        for (unsigned n = 0; n < sample_slots; ++n)
            if (project_->samples[n].used()) project_->samples[n].frames = library_->slot(n).frames;
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
            if (loaded->error.empty() && load_project(loaded->bytes, *project, *bank, &version).empty()) {
                for (auto& t : project->tracks) upgrade_params(t.engine, version, t.params); // older files: pad expression at its defaults
                if (!context_.kit_from.empty() && context_.kit_from == loaded->path) // Load Kit: only its kit, onto the selected track
                    context_.load_kit(*project, std::filesystem::path(loaded->path).stem().string());
                else context_.switch_to(*project, *bank, loaded->path);
            }
            if (context_.kit_from == loaded->path) context_.kit_from.clear();
        }
        if (leave_after_save >= 0 && int(store_->saves()) >= leave_after_save) leave.store(true);
    }
    context_.song_tick();
    context_.stage_tick(now); // Stage's PRESET: the sound turned to, once the turning stops
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
        if (v.settings_read) { ui_.settings.read(v.settings); settings_loaded = true; }
    }
    // The backlight stays as the stock app left it until Screen asks for less (and then follows it).
    if (ui_.settings.screen != 100 || screen_wanted.load(std::memory_order_relaxed)) screen_wanted.store(ui_.settings.screen, std::memory_order_relaxed);
    audio_wanted.store(ui_.settings.audio_rate | uint32_t(ui_.settings.audio_periods) << 20, std::memory_order_relaxed);
    // The Project page's row count, for K2's range.
    if (store_ && ui_.page == PageId::Project && store_->generation() != store_seen) {
        store_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        ui_.project_rows = uint16_t(v.entries.size() + (context_.project_path.empty() ? 2 : 1) + (context_.project_folder.empty() ? 2 : 0)); // + DEMOS, SD card access
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
    unsigned presses = power.load(std::memory_order_relaxed);
    if (presses != power_seen) { power_seen = presses; ui_.exit_open = true; }

    const auto& ui = ui_;
    {
        const auto& t = project_->tracks[ui_.track];
        const auto& pad = t.pads[t.last_pad % sampler_pads];
        page_track.store(uint32_t(ui_.page) | uint32_t(ui_.track) << 8, std::memory_order_relaxed);
        { // Stage: the button and page in view, the card the wheel edits, a sound waiting to load
            const auto& t = project_->tracks[ui_.track];
            auto at = stage_at(ui_, t);
            stage_bits.store(at.group | at.sub << 4 | unsigned(stage_focus(ui_)) << 8 | unsigned(ui_.preset_due != 0) << 12
                             | unsigned(ui_.tracks_half) << 13 | unsigned(t.preset & 0x3ff) << 16 | unsigned(t.bank % sampler_banks) << 26,
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
            motion_now.store(project_->motion.count, std::memory_order_relaxed);
        }
        auto sig = project_->time_signature();
        signature_now.store(sig.beats * 100u + sig.unit, std::memory_order_relaxed);
        pad_sample.store(pad.sample, std::memory_order_relaxed);
        { // phase 4: the last pad's Sample Mode switches, gain, link and filter; 16 Levels' type
            pad_bits.store(uint32_t(pad.loop) | uint32_t(pad.loop_lock) << 1 | uint32_t(pad.play == PadPlay::Gate) << 2 | uint32_t(pad.reverse) << 3
                           | uint32_t(pad.mono) << 4 | uint32_t(t.levels) << 5 | uint32_t(t.levels_kind % 4) << 6 | uint32_t(t.last_pad) << 8
                           | uint32_t(pad.filter) << 16 | uint32_t(pad.link) << 20 | uint32_t(pad.decay_from_start) << 25, std::memory_order_relaxed);
            pad_gain.store(pad.gain, std::memory_order_relaxed);
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
            knob_bits.store(uint32_t(k.on) | uint32_t(k.bypass) << 1 | uint32_t(ui_.settings.input.fx) << 2 | uint32_t(k.choice) << 8
                            | uint32_t(k.kind) << 16 | uint32_t(ui_.knob_cursor) << 24, std::memory_order_relaxed);
            unsigned first = t.bank % sampler_banks * 16;
            uint32_t bank_pads = 0;
            for (unsigned n = 0; n < 16; ++n) bank_pads |= uint32_t(k.pad_on(ui_.track, first + n)) << n;
            knob_targets.store(uint32_t(k.tracks) | bank_pads << 16, std::memory_order_relaxed);
            const auto& c = project_->compressor;
            comp_bits.store(uint32_t(c.on) | uint32_t(c.color) << 1 | uint32_t(c.boost) << 2 | uint32_t(c.attack) << 8 | uint32_t(c.release) << 16
                            | uint32_t(c.amount) << 24, std::memory_order_relaxed);
        }
        { // phase 7b: Pad FX (bank, engaged, latched, selected; the slots' kinds + 1) and Flex Beat's settings
            unsigned latched = 0;
            for (unsigned n = 0; n < ui_.engaged_count; ++n) latched += ui_.engaged[n].latched;
            padfx_bits.store(uint32_t(project_->punch_bank % 4) | uint32_t(ui_.engaged_count & 0x1f) << 2 | uint32_t(latched & 0x1f) << 7
                             | uint32_t(uint8_t(ui_.punch_selected + 1)) << 12 | uint32_t(project_->flex.mode) << 20
                             | uint32_t(project_->flex.quantize) << 21 | uint32_t(project_->flex.mix) << 22, std::memory_order_relaxed);
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
                             | uint32_t(ui_.settings.fader_takeover) << 12 | uint32_t(light & 0x7f) << 16 | uint32_t(t.wheel & 0x7f) << 24,
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
        // phase 10: the undo history's depth and the redo side, Seq + Sample, the sampler map, DEMOS, a gesture open
        phase10_bits.store(uint32_t(context_.edits.size()) | uint32_t(context_.edits.can_redo()) << 8 | uint32_t(ui_.seq_sample) << 9
                           | uint32_t(ui_.map_open) << 10 | uint32_t(ui_.project_demos) << 11 | uint32_t(bool(context_.gesture.edit)) << 12
                           | uint32_t(project_->song.count & 0xfff) << 16 | uint32_t(project_->tracks[ui_.track].engine) << 28,
                           std::memory_order_relaxed);
        if (context_.gesture.edit && now - context_.gesture.last >= gesture_quiet_ms) context_.close_gesture(); // a pot left still
    }
    const auto& project = *project_;
    const auto& track = project.tracks[ui.track];
    bool playing = host.playing();
    int position = host.position_of(ui.track);
    uint32_t ticks = host.ticks();
    bool tracks = tracks_on_pads(ui), muting = ui.mute_mode() && !ui.name.open() && !tracks;
    bool mute_kit = muting && mute_on_kit(ui, kit_pads());
    bool shift_pads = ui.shift && !ui.name.open() && !ui.exit_open && !tracks && !muting;
    const Rgb muted_red = led_colour::rgb(1, 0, 0, 0.35f); // muted, as the MPC lights it
    uint32_t lit = 0; // for `status`
    for (unsigned pad = 0; pad < 16; ++pad) {
        bool held = ui.held_pads & (1u << pad);
        Rgb colour {};
        if (mute_kit) {
            // Mute mode on a kit: the bank's pads, muted red, soloed white, the rest in the track's colour.
            unsigned kit = track.bank % sampler_banks * 16 + pad;
            colour = !track.pads[kit].used() ? Rgb {} : track.pad_muted(kit) ? muted_red
                   : pad_soloed(track, kit) ? led_colour::white(0.8f) : track_light(ui.track, 0.6f);
        } else if ((ui.page == PageId::Record || ui.resample_pick) && track.engine == EngineKind::Sampler && !tracks && !muting) {
            // Record and Resample's pick: every pad can take a sample (empty ones faint); the take's pad
            // red, flashing while it waits; the pads cut from it white.
            unsigned kit = track.bank % sampler_banks * 16 + pad;
            bool take_here = ui.rec_track == ui.track;
            bool cut = false;
            for (unsigned n = 0; n < ui.rec_chops && take_here; ++n) cut |= ui.rec_chop_pad[n] == kit;
            auto state = host.recorder().state();
            if (take_here && ui.rec_pad == int(kit))
                colour = state == TakeRecorder::State::Recording || (now / 250) % 2 ? led_colour::rgb(1, 0, 0, 1) : led_colour::rgb(1, 0, 0, 0.2f);
            else if (cut) colour = led_colour::white(0.9f);
            else colour = track_light(ui.track, track.pads[kit].used() ? 0.3f : 0.07f);
        } else if (tracks || muting || ui.page == PageId::Tracks || ui.page == PageId::Mix || ui.page == PageId::Song) {
            // The pads are the tracks.
            const auto& t = project.tracks[pad];
            float level = !t.used() ? 0 : pad == ui.track ? 1.0f : t.mix.mute ? 0.08f : 0.35f;
            colour = track_light(pad, level);
            if (muting && t.used()) colour = t.mix.mute ? muted_red : (t.mix.solo ? led_colour::white(0.8f) : track_light(pad, 0.6f));
            if (ui.copying == UiState::Copying::Tracks && ui.copy_from == int(pad)) colour = led_colour::white(1); // the track being copied
        } else if (ui.page == PageId::Patterns && !ui.seq_sample) {
            // The pads are the selected track's sixteen patterns: playing bright, queued
            // blinking, holding notes dim, empty dark.
            bool notes = !patterns_->at[ui.track][pad].empty();
            bool live = host.pattern_of(ui.track) == pad, queued = playing && track.pattern == pad && !live;
            float level = live ? 1.0f : queued ? ((now / 150) % 2 ? 0.8f : 0.15f) : notes ? 0.25f : 0;
            colour = track_light(ui.track, level);
        } else if (ui.page == PageId::Steps) {
            const auto& pattern = context_.pattern();
            unsigned step = ui.steps_bar * bar_steps + pad;
            bool inside = step < pattern.steps(), on = inside && !pattern.step(step).empty();
            colour = on ? track_light(ui.track, 0.8f) : Rgb {};
            if (playing && inside && position >= 0 && unsigned(position) / pattern.q() == step) colour = led_colour::white(0.7f);
            if (int(step) == ui.held_step) colour = led_colour::white(1);
        } else if (ui.page == PageId::StepEdit) {
            // The pads with an event on the step bright, the selected event's white, the rest faint.
            const auto& pattern = context_.pattern();
            int key = pad_key(project, track, pad), chosen = selected_event(pattern, ui);
            auto in = step_events(pattern, unsigned(std::max<int>(ui.step_cursor, 0)) % pattern.steps());
            bool here = false;
            for (unsigned n = 0; n < in.count; ++n) here |= pattern.events[in.first + n].key == key;
            bool empty_pad = track.engine == EngineKind::Sampler && !track.pads[(unsigned(key) - sampler_first_note) % sampler_pads].used();
            colour = key < 0 ? Rgb {} : chosen >= 0 && pattern.events[size_t(chosen)].key == key ? led_colour::white(1)
                   : here ? track_light(ui.track, 1.0f) : empty_pad ? Rgb {} : track_light(ui.track, 0.12f);
        } else if (ui.page == PageId::KnobFx) {
            // Knob FX: the pads it is on bright, the rest faint (as on the MPC); kit pads of the bank, or tracks.
            const auto& k = project.knob_fx;
            if (track.engine == EngineKind::Sampler) {
                unsigned kit = track.bank % sampler_banks * 16 + pad;
                colour = track_light(ui.track, k.pad_on(ui.track, kit) ? 1.0f : track.pads[kit].used() ? 0.12f : 0.04f);
            } else {
                colour = track_light(pad, k.track_on(pad) ? 1.0f : project.tracks[pad].used() ? 0.12f : 0.04f);
            }
        } else if (ui.page == PageId::Punch) {
            // The bank's punches: sounding bright, engaged but bypassed (a fifth) blinking, the rest faint; white on the master.
            int kind = punch_on_pad(project.punch_bank, pad);
            bool waiting = kind >= 0 && ui.engaged_at(kind) >= 0 && !ui.punch_sounding(kind);
            float level = kind < 0 ? 0 : ui.punch_sounding(kind) ? 1.0f : waiting ? ((now / 250) % 2 ? 0.5f : 0.15f) : 0.25f;
            colour = project.punch_target ? track_light(ui.track, level) : led_colour::white(level * 0.7f);
        } else if (ui.page == PageId::FlexBeat) {
            // Flex Beat: the pad playing bright (Empty when none), the others faint.
            unsigned playing = host.flex_playing();
            colour = led_colour::white(pad == playing ? 0.8f : pad == 0 ? 0.08f : 0.2f);
        } else if (ui.page == PageId::PadPunch && track.engine == EngineKind::Sampler) {
            unsigned kit = track.bank % sampler_banks * 16 + pad;
            const auto& sound = track.pads[kit];
            colour = sound.used() ? track_light(ui.track, ui.pad_punch_held == int(kit) ? 1.0f : sound.punch ? 0.5f : 0.12f) : Rgb {};
        } else if (track.engine == EngineKind::Sampler) {
            unsigned kit = track.bank % sampler_banks * 16 + pad;
            colour = track.pads[kit].used() ? track_light(ui.track, kit == track.last_pad ? 1.0f : 0.3f) : Rgb {};
        } else if (track.engine == EngineKind::Kit909) {
            colour = pad < 11 ? track_light(ui.track, pad == unsigned(track.last_note - 36) ? 0.9f : 0.3f) : Rgb {};
        } else if (track.used()) {
            auto note = pad_note(project, track, pad);
            colour = track_light(ui.track, note.root ? 0.9f : 0.22f);
        }
        if (shift_pads) { // Shift held: the pads with a printed job glow, a toggle that is on brightly
            unsigned level = shift_pad_level(ui, pad);
            colour = level ? track_light(ui.track, level == 2 ? 1.0f : 0.3f) : Rgb {};
        }
        if (held && ui.page != PageId::Steps && !tracks && !muting) colour = led_colour::white(1);
        if (ui.settings.pad_lights == 1) { // white, as bright as the colour was
            float full = std::max<float>(led_colour::white(1).r, 1);
            colour = led_colour::white(std::max({colour.r, colour.g, colour.b}) / full);
        }
        if (ui.settings.pad_lights == 2) colour = {};
        lights.pad(pad, colour);
        if (colour.r || colour.g || colour.b) lit |= 1u << pad;
    }
    pads_lit.store(lit, std::memory_order_relaxed);
    { // The fader light shows the value it drives (brightest at the middle for pan and tune), dark with nothing to drive;
      // white where that value is the master's (Compressor, Flex Beat, Knob FX).
        auto job = fader_job(project, ui);
        float level = fader_light_level(job);
        bool master = job.source == FaderJob::Source::Page
                   && (ui.page == PageId::Compressor || ui.page == PageId::FlexBeat || ui.page == PageId::KnobFx);
        lights.fader(level <= 0 ? Rgb {} : master ? led_colour::white(level) : track_light(ui.track, level));
    }
    // Bank lights A-H: the sampler track's bank in view.
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank)
        lights.bank(bank, track.engine == EngineKind::Sampler && bank == track.bank % sampler_banks);

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
    if (ui.shift && !ui.name.open() && !ui.exit_open) { // as Akai's guide has it: the Shift jobs faint, bright while on
        for (B button : {B::Sample, B::Seq, B::PadFx, B::KnobFx, B::Shift, B::PadBank, B::Erase, B::NoteRepeat, B::Chop, B::Mute,
                         B::Loop, B::SixteenLevels, B::SampleSelect, B::TapTempo, B::Minus, B::Plus, B::SampleRecord, B::SeqRecord,
                         B::Stop, B::Play, B::WheelPush}) {
            bool shift_job = button_has_shift_job(project, ui, button, kit);
            bool lit = button == B::Shift || (button == B::TapTempo && project.click) || (button == B::Erase && ui.copying != UiState::Copying::Off)
                    || (button == B::NoteRepeat && track.arp) || (button == B::Mute && ui.mute_mode())
                    || (track.engine == EngineKind::Sampler && button == B::Loop && track.pads[track.last_pad % sampler_pads].reverse)            // reverse on
                    || (track.engine == EngineKind::Sampler && button == B::Chop && track.pads[track.last_pad % sampler_pads].play == PadPlay::Gate); // Note On
            set(button, !shift_job ? L::Off : lit ? L::Bright : L::Dim);
        }
        button_levels.store(levels, std::memory_order_relaxed);
        return;
    }
    bool play_group = ui.page == PageId::Stage || ui.page == PageId::Keys || ui.page == PageId::Pads || ui.page == PageId::Mix;
    on(B::Sample, play_group || ui.page == PageId::InputConfig);
    on(B::Seq, ui.page == PageId::Patterns || ui.page == PageId::Song || ui.page == PageId::StepEdit || ui.page == PageId::Steps
                   || ui.page == PageId::Roll || ui.page == PageId::Motion);
    if (ui.seq_sample) { // Seq + Sample: both blink while the pads play the kit on Patterns
        L blink = (now / 250) % 2 ? L::Bright : L::Dim;
        set(B::Seq, blink); set(B::Sample, blink);
    }
    on(B::SampleSelect, ui.page == PageId::Sounds || ui.page == PageId::Samples);
    on(B::Chop, ui.page == PageId::Chop);
    on(B::NoteRepeat, track.arp);
    {
        auto state = host.recorder().state();
        bool blink = (now / 250) % 2;
        set(B::SampleRecord, state == TakeRecorder::State::Recording ? L::Bright
                      : state == TakeRecorder::State::Armed || state == TakeRecorder::State::Cued ? (blink ? L::Bright : L::Dim)
                      : job(B::SampleRecord) ? L::Dim : L::Off);
    }
    on(B::PadFx, ui.page == PageId::Punch || ui.page == PageId::PadPunch || ui.page == PageId::FlexBeat || ui.engaged_count || host.flex_playing());
    on(B::KnobFx, project.knob_fx.on); // the mode, as on the MPC (Shift + Knob FX opens the chains)
    on(B::Loop, track.engine == EngineKind::Sampler && track.pads[track.last_pad % sampler_pads].loop); // the last pad loops
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
