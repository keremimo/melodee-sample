// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad, fader and button lights for the page in view (MIDI thread, every
// 5 ms; the runtime sends only what changed). Colours come from the track
// palette; button light values from surface/leds.h.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/step_sounds.h"
#include "melodee/shell/history_jobs.h"
#include "melodee/shell/lesson_jobs.h"
#include "melodee/shell/track_colours.h"
#include "melodee/shell/features.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/project_file.h"
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/model/scales.h"
#include "melodee/model/slice_hits.h"
#include "melodee/shell/autosave.h"
#include "melodee/shell/card_paths.h"
#include "melodee/shell/sampler_map.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/button_jobs.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/note_repeat.h"
#include "melodee/shell/light_show.h"
#include "melodee/shell/pad_lights.h"
#include "melodee/shell/pad_bank.h"
#include "melodee/shell/roll.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/print_engine.h"
#include <filesystem>
#include "melodee/shell/shift_pad_jobs.h"
#include "melodee/shell/step_edit.h"
#include "melodee/ui/tokens.h"
#include "surface/led_colours.h"
#include "melodee/shell/mpc_wav_slices.h"
#include "melodee/model/slice_banks.h"
#include "melodee/shell/zoom_view.h"
#include "melodee/model/card_folders.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace melodee {

namespace {
// A sample saved in Samples at the card's top, where older builds wrote samples, and since moved into Melodee/Samples:
// its slot names it there before it loads (model/card_folders.h), and the project is saved with that path.
void follow_moved_sample(Project& project, unsigned slot, const SampleLibrary& library) {
    const auto& ref = project.samples[slot % sample_slots];
    if (!ref.used() || ref.path[0] != '/') return;
    const std::string now = card_sample_path(library.card(), ref.path);
    if (now != ref.path && now.size() < sizeof(SampleRef::path)) std::snprintf(project.samples.edit(slot).path, sizeof(SampleRef::path), "%s", now.c_str());
}
} // namespace

// Updates from the card (shell/update.h). Opens the update question when the service thread found a newer
// release, playback is stopped and no other sheet is open. After INSTALL (shell/ask.cpp), waits for the project
// save, then asks the service thread to start the install, and copies its progress to the sheet. Also shows
// notices from the service thread ("Updated to v0.2.0").
// Background save, as on the stock app (shell/autosave.h): once the project has changed, nothing runs and no
// control has been used for a moment, it goes to its file. A held control counts as in use.
void Shell::autosave_tick(uint32_t now) {
    const auto& ui = ui_;
    if (ui.held_pads || ui.shift || ui.seq_held || ui.pad_bank || ui.sample_held || ui.erase || tap.down) last_input_ = now;
    const auto& host = context_.host;
    AutosaveFacts f;
    f.enabled = ui.settings.autosave;
    f.dirty = context_.dirty;
    f.playing = host.playing() || host.song_playing() || host.looper().recording() || host.looper().armed();
    f.recording = ui.recording || ui.rec_armed || host.recorder().state() != TakeRecorder::State::Idle
               || (context_.printer && context_.printer->busy()) || ui.print_sheet.printing
               || ui.auto_sheet.sampling || ui.auto_sheet.checking || (store_ && store_->exporting());
    f.sheet = ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open || card_owns_controls(ui) || ui.update.phase != 0;
    f.loading = ui.loading || context_.samples_to_load < sample_slots;
    f.editing = (ui.page == PageId::Chop && ui.chop_take_at) || ui.page == PageId::Record || context_.import_unsaved || ui.lesson; // a lesson's project is never saved; Chop saves between takes
    f.failed = autosave_failed_;
    f.now = now; f.last_input = last_input_; f.last_try = autosave_try_;
    if (!autosave_due(f)) return;
    autosave_failed_ = !context_.autosave();
    autosave_try_ = now;
}

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

// Light show (Sample + Mute, shell/light_show.h): every light from the show; false where the page's lights stay
// (SD card access). Pads pressed flash (in Ripple, send rings); sounds starting on the selected track's pads flash.
void Shell::light_style_step(int by) {
    constexpr int count = int(ShowStyle::Count);
    ui_.sample_used = true; // Sample's release opens nothing
    ui_.light_style = uint8_t(((int(ui_.light_style) + by) % count + count) % count);
    context_.notice(std::string("Light show: ") + show_style_names[ui_.light_style]);
}

bool Shell::light_show_pass(SurfaceLights& lights, uint32_t now) {
    if (card_owns_controls(ui_)) return false;
    const auto style = ShowStyle(ui_.light_style % unsigned(ShowStyle::Count));
    if (!show_was_on) {
        show_lights.start(now);
        show_was_on = true;
        show_pads_seen = ui_.held_pads;
        show_voices_seen = {};
    }
    for (unsigned pad = 0; pad < 16; ++pad) // pressed since the last pass
        if ((ui_.held_pads & ~show_pads_seen) >> pad & 1) show_lights.tap(pad, 1.0f, now, style);
    show_pads_seen = ui_.held_pads;
    { // the selected track's voices: a new one on a pad of the bank in view (the sequence, Note Repeat) flashes it
        host.watch_taps(int(ui_.track % track_count));
        TrackTaps taps;
        std::array<uint32_t, TrackTaps::voices> seen {};
        if (host.taps(taps) && taps.track == ui_.track % track_count) {
            const auto& t = project_->tracks[ui_.track];
            unsigned first = t.bank % sampler_banks * 16;
            for (unsigned n = 0; n < taps.count && n < TrackTaps::voices; ++n) {
                const auto& v = taps.voice[n];
                seen[n] = v.serial;
                bool known = std::find(show_voices_seen.begin(), show_voices_seen.end(), v.serial) != show_voices_seen.end();
                if (known || v.pad < 0 || unsigned(v.pad) >= sampler_pads || double(v.age) > dsp::sample_rate() / 10) continue; // started within a tenth of a second
                unsigned kit = unsigned(v.pad);
                if (kit < first || kit >= first + 16 || ui_.held_pads >> (kit - first) & 1) continue; // a hand's press flashed already
                show_lights.play(kit - first, 0.5f + 0.5f * v.level, now);
            }
        }
        show_voices_seen = seen;
    }
    ShowClock clock;
    clock.now = now;
    clock.playing = host.playing();
    clock.bar_beats = std::max(1u, unsigned(project_->time_signature().beats));
    if (clock.playing) clock.beats = double(host.ticks()) / std::max(1u, project_->beat_ticks());
    else clock.beats = double(now) * tempo_now.load(std::memory_order_relaxed) / 600000.0; // tenths of a BPM
    clock.meters = meter_reads.at(now, false, [&](unsigned side) { return host.out_level(side); });
    clock.level = {host.out_level(0), host.out_level(1)};
    auto frame = show_lights.frame(clock, style);
    show_frame(lights, frame);
    { // for `status`: the pads' rungs (white), the buttons' levels (red as bright), the meters
        uint64_t rungs = 0, hues = 0, levels = 0;
        for (unsigned pad = 0; pad < 16; ++pad) {
            const auto c = frame.lights.pads[pad];
            float level = std::sqrt(std::max({c.r, c.g, c.b}) / 127.0f);
            if (!level) continue;
            rungs |= uint64_t(pad_light::rung(level)) << (4 * pad);
            hues |= uint64_t(pad_light::Hue::White) << (4 * pad);
        }
        for (auto b : panel_lit_buttons) {
            auto l = frame.lights.buttons[size_t(b)];
            levels |= uint64_t(l == ButtonLight::RedBright ? 2 : l == ButtonLight::RedDim ? 1 : unsigned(l)) << (2 * unsigned(b));
        }
        pad_rungs.store(rungs, std::memory_order_relaxed);
        pad_hues.store(hues, std::memory_order_relaxed);
        button_levels.store(levels, std::memory_order_relaxed);
        meter_bits.store(uint32_t(frame.meters[0]) | uint32_t(frame.meters[1]) << 8, std::memory_order_relaxed);
    }
    return true;
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
        if (change.back && ui_.page == PageId::CardAccess) { context_.open(PageId::Project); context_.notice("Card ejected"); }
    }
    update_tick();
    autosave_tick(now);
    usage_tick(now);
    context_.chop_tick(now); // Chop: a moved cut heard once, gone still
    context_.browse_tick(now); // Samples' trim sheet: a moved end heard once, the sheet down off its file
    // Power was pressed: the Exit sheet opens (the service thread only counts presses).
    // The library found files or finished a sample: the lengths go into the project.
    if (library_ && library_->generation() != library_seen) {
        library_seen = library_->generation();
        for (unsigned n = 0, span = unsigned(project_->samples.span()); n < span; ++n)
            if (project_->samples[n].used()) {
                const auto shown = library_->measure(n); // its length and zones alone: not its waveform and hits, a slot at a time
                auto& ref = project_->samples.edit(n);
                ref.frames = shown.frames;
                ref.zones = uint16_t(std::min<size_t>(shown.shapes.size(), 0xffff));
                std::array<uint64_t, 2> keys {}; // 16 Levels' Zones: the keys a press sounds
                bool one_key = true; // and whether each such zone has a key of its own (16 Levels opens in Zones)
                for (const auto& z : shown.shapes)
                    if (!(z.flags & ZoneShape::Release)) {
                        one_key = one_key && z.low_key == z.high_key;
                        for (unsigned k = z.low_key; k <= z.high_key && k < 128; ++k) keys[k / 64] |= uint64_t(1) << (k % 64);
                    }
                ref.keys = keys;
                ref.one_key_zones = one_key && zone_key_count(ref) >= 2;
                ref.streamed = shown.streamed;
                if (ref.root == root_unread) ref.root = ref.instrument() ? root_none : library_->root_of(n, ref.path); // once, then kept
            }
        ui_.files_count = uint16_t(std::min<size_t>(library_->files().size(), 0xffff));
        if (ui_.files_cursor >= ui_.files_count) ui_.files_cursor = uint16_t(ui_.files_count ? ui_.files_count - 1 : 0); // a row deleted: the last one left
        if (ui_.page == PageId::Chop && !ui_.slices.count) context_.rechop();
    }
    // A KEYGROUP track of one sample plays from that sample's note once it is found, and again when its file changes.
    for (unsigned t = 0; t < track_count; ++t) {
        const auto root = int16_t(keyed_root(*project_, project_->tracks[t]));
        if (root == keyed_roots_sent[t]) continue;
        keyed_roots_sent[t] = root;
        context_.send({Command::Type::KeyedRoot, uint8_t(t), 0, 0, root});
    }
    context_.files_landed();  // Trim Sample, Extract, Save Sample, takes, Recall, Resample: done on the service thread
    context_.sync_chops();    // the chops slice hits play, as Chop and the project hold them now (issue 24)
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
            PrintTable prints;
            if (loaded->error.empty() && load_project(loaded->bytes, *project, *bank, &version, &plugin_states, &prints).empty()) {
                for (auto& t : project->tracks) upgrade_params(t.engine, version, t.params); // older files: pad expression at its defaults
                samples_on_card_now(*project, loaded->card); // saved on another card: its files at the same place on this one
                if (!context_.kit_from.empty() && context_.kit_from == loaded->path) // Load Kit: only its kit, onto the selected track
                    context_.load_kit(*project, std::filesystem::path(loaded->path).stem().string());
                else if (loaded->imported) { // not saved yet: the .xpj stays as it is (its takes wait beside the projects)
                    context_.switch_to(*project, *bank, {});
                    context_.audio_staging_from = loaded->audio_from;
                    context_.dirty = true;
                    context_.import_unsaved = true; // opening one writes nothing: no background save until it has a file
                    context_.notice(loaded->note);
                }
                else {
                    context_.switch_to(*project, *bank, loaded->path, &plugin_states, &prints);
                    read_jams(loaded->bytes, context_.jams); // jam capture: its jams (a JAMS section)
                    context_.publish_jams();
                }
            } else if (!loaded->note.empty()) context_.notice(loaded->note);
            if (context_.kit_from == loaded->path) context_.kit_from.clear();
        }
        if (auto note = store_->take_note()) context_.notice(*note); // an MPC 3 or Live Set export's outcome
        if (leave_after_save >= 0 && int(store_->saves()) >= leave_after_save && !(library_ && library_->loops_busy())) { // loop files too
            if (!off_after_save) leave.store(true);
            else if (!ui_.exit_saved) { ui_.exit_saved = true; off_saved_at = now; } // the sheet says it is saved, then off
        }
    }
    context_.now = now;
    context_.reload_tick(); // a project parked for: built once the audio thread is still; the Loading sheet till it is in
    if (ui_.exit_saved && now - off_saved_at >= 1000) power_off.store(true); // a second to read that it is saved
    context_.song_tick();
    context_.jam_tick();      // jam capture: the bar playing outside Song (shell/jam_jobs.cpp)
    context_.free_tick();     // Free length: a take on a Free pattern grows it (shell/free_length.h)
    context_.plugins_tick(now); // the rack follows the project; the plug-in in view's cards
    context_.print_tick();      // prints made (freeze plan F2a) into the project's records
    context_.export_tick();     // an MPC 3 or Live Set export's plug-in tracks printed, then the export
    context_.stems_tick();      // a sample's stems written: onto their pads (shell/stems_jobs.cpp)
    context_.stage_tick(now); // Stage's PRESET: the sound turned to, once the turning stops
    context_.live_erase_tick(); // Erase + pad while playing: the notes the playhead passed
    context_.chain_tick();    // pattern chains: the next queued in the last bar of the one playing (seq-sheet2 B)
    context_.follow_tick();   // roadmap P5: chains told to the audio thread, follow actions that came followed
    context_.capture_tick();  // roadmap P5: the transport's runs, for what Capture keeps
    seq_follow(context_);     // Settings > SEQ > Follow: Step Edit and Roll with the playhead
    context_.note_preview_tick(); // Settings > SEQ > Note preview: a note written or moved heard, and ended on time
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
        if (v.settings_read) {
            ui_.settings.read(v.settings); settings_loaded = true; context_.send_click(); context_.send_repeat_velocity(); context_.send_mute_kind();
            if (ui_.settings.usage_log) context_.usage.start(now, (long long)std::time(nullptr), melodee_version(), ui_.settings.full_features);
        }
    }
    // The backlight stays as the stock app left it until Screen asks for less (and then follows it).
    if (ui_.settings.screen != 100 || screen_wanted.load(std::memory_order_relaxed)) screen_wanted.store(ui_.settings.screen, std::memory_order_relaxed);
    want_audio_format();
    // The Project page's row count, for K2's range.
    if (store_ && ui_.page == PageId::Project && store_->generation() != store_seen) {
        store_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        ui_.project_rows = uint16_t(project_rows(v.entries.size()));
        project_listed(context_); // the open project's row, the first time the list opens
    }
    // Slots a Load Kit or Load Bank named past what the queue holds, as it drains.
    while (library_ && !context_.slots_waiting.empty()) {
        unsigned slot = context_.slots_waiting.front();
        follow_moved_sample(*project_, slot, *library_);
        const auto& ref = project_->samples[slot % sample_slots];
        if (ref.used() && !library_->load(slot, ref.path, ref.root == root_unread)) break;
        context_.slots_waiting.erase(context_.slots_waiting.begin());
    }
    // After a switch, every sample slot loads again (or empties), a few each pass. A slot the project leaves empty
    // and the library holds nothing in is passed over: nothing to ask for.
    while (library_ && context_.samples_to_load < sample_slots) {
        const unsigned slot = context_.samples_to_load;
        follow_moved_sample(*project_, slot, *library_);
        const auto& ref = project_->samples[slot];
        bool real = ref.used() && ref.path[0] == '/';
        if ((real || library_->held(slot)) && !library_->load(slot, real ? ref.path : "", real && ref.root == root_unread)) break;
        if (real && !project_->chops[slot].unwritten) project_->chops.edit(slot) = read_mpc_slices(ref.path); // the file's slices are its chop (an older project's waits to be written)
        ++context_.samples_to_load;
    }
    // Long samples (read from the card as they play): the chunks where their pads start, end and loop held, a tenth of
    // a second at most after a pad changes; what no pad needs any more let go (shell/stream_holds.h).
    if (library_ && now - holds_at >= 100) {
        holds_at = now;
        const bool chop = ui_.page == PageId::Chop && ui_.slices.count;
        const auto& t = project_->tracks[ui_.track % track_count];
        const FrameView view = zoomed_view(); // a close view's frames too, drawn from memory (SampleLibrary::detail())
        const unsigned view_sample = ui_.page == PageId::Chop ? ui_.chop_sample
                                   : ui_.page == PageId::Samples && ui_.browse.up() ? unsigned(ui_.browse.slot) // the trim sheet's file
                                   : t.pads[t.last_pad % sampler_pads].sample;
        SlicedChops sliced; // slice hits' samples, their chops as they play them (issue 24)
        for (uint16_t slot : sliced_samples(*project_, context_.patterns)) sliced.emplace_back(slot, &context_.live_chop(slot));
        auto holds = stream_holds(*project_, chop ? &ui_.slices : nullptr, chop ? ui_.chop_sample : no_sample,
                                  view.width() ? view_sample : no_sample, HeldRange {view.from, view.to, false}, sliced);
        auto find = [](const StreamHolds& in, unsigned slot) {
            auto at = std::lower_bound(in.begin(), in.end(), slot, [](const auto& e, unsigned s) { return e.first < s; });
            return at != in.end() && at->first == slot ? &at->second : nullptr;
        };
        for (const auto& [slot, ranges] : holds) {
            const auto* sent = find(holds_sent, slot);
            if (!sent || *sent != ranges) library_->hold(slot, ranges);
        }
        for (const auto& [slot, ranges] : holds_sent) if (!find(holds, slot)) library_->hold(slot, {});
        holds_sent = std::move(holds);
    }
    if (encoder_tick(enc, now)) menu_open(context_); // the encoder held: the MENU (shell/menu.h)
    if (history_tick(history_hold, now)) history_open(context_); // Shift + Minus held: the undo list (shell/history_list.h)
    mixer_view_tick(mixer_view, ui_, now); // the mixer's Plus / Minus held: the card or the strips (shell/mixer_channels.h)
    lesson_tick(context_, now); // a lesson: its kit loaded, or its last step done (shell/lesson_jobs.h)
    if (ui_.levels_sheet && ui_.track != ui_.levels_sheet_track) ui_.levels_sheet = 0; // 16 Levels' FILE or SET (shell/levels_keys.h): another track selected closes it
    if (ui_.colour_sheet && !ui_.colour_seq && ui_.track != ui_.colour_track) track_colour_button(context_, SurfaceButton::TopRight); // TRACK COLOUR: kept as it is
    show_track_colours(*project_); // issue 69: the colour each track shows, for the pads and the screen
    ui_.tempo_locked = ui_.settings.midi.sync_in == MidiSync::Clock && midi_io.tempo_in(); // MIDI clock in leads the tempo
    if (ui_.page == PageId::Song) { // issue 60: K2's list, the selected track's patterns that hold notes
        ui_.song_used = {};
        const unsigned t = ui_.track % track_count;
        for (unsigned n = 0; n < pattern_count; ++n)
            if (!context_.patterns.at[t][n].empty()) ui_.song_used[n / 64] |= uint64_t(1) << (n % 64);
    }
    // Tap Tempo held half a second with nothing turned: the Tempo page; held on it, back to the page under it.
    switch (tap_tick(tap, now, ui_.page == PageId::Tempo)) {
    case TapDo::Open: context_.open(PageId::Tempo); break;
    case TapDo::Close: context_.back(); break;
    default: break;
    }
    context_.now = now;
    looper_tick(context_, now); // Loop held, a clear pad held, refusals, the metronome (shell/looper_jobs.cpp)
    unsigned presses = power.load(std::memory_order_relaxed);
    if (presses != power_seen) { // a second press while it is open closes it, as B1 STAY does
        power_seen = presses;
        if (ui_.exit_open) { ui_.exit_open = ui_.exit_off = ui_.exit_saved = false; leave_after_save = -1; off_after_save = false; }
        else { ui_.exit_pick = 0; ui_.exit_open = true; } // the wheel starts on STAY
    }

    const auto& ui = ui_;
    {
        const auto& t = project_->tracks[ui_.track];
        const auto& pad = t.pads[t.last_pad % sampler_pads];
        page_track.store(uint32_t(ui_.page) | uint32_t(ui_.track) << 8, std::memory_order_relaxed);
        { // Stage: the button and page in view, the card the wheel edits, a sound waiting to load
            const auto& t = project_->tracks[ui_.track];
            const StageSkip instrument = stage_skip(*project_, ui_, t);
            auto at = stage_at(ui_, t, instrument);
            stage_bits.store(at.group | at.sub << 4 | unsigned(stage_focus(ui_, t)) << 8 | unsigned(ui_.preset_due != 0) << 12
                             | unsigned(t.preset & 0x3ff) << 16 | unsigned(t.bank % sampler_banks) << 26,
                             std::memory_order_relaxed);
            std::lock_guard<std::mutex> text(text_lock);
            stage_key_text = stage_page_now(ui_, t, instrument).key;
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
                roll_focus_now.store(roll_focus(shown, roll, roll_lane(context_.track())) >= 0 ? 1 : 0, std::memory_order_relaxed);
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
                           | uint32_t(pad.mono) << 4 | uint32_t(t.levels) << 5 | uint32_t(t.last_pad) << 8
                           | uint32_t(pad.filter) << 16 | uint32_t(pad.link) << 20 | uint32_t(pad.decay_from_start) << 25
                           | uint32_t(t.levels_kind & 7) << 26, std::memory_order_relaxed);
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
                            | uint32_t(m.program_change & 3) << 19 | uint32_t(m.cv_out) << 21 | uint32_t(m.cv_ppqn() & 255) << 22,
                            std::memory_order_relaxed);
            track_channel.store(t.midi_channel, std::memory_order_relaxed);
            midi_row.store(ui_.midi_cursor, std::memory_order_relaxed);
        }
        { // phase 5: Chop, the region and its file, the browser
            unsigned used = 0;
            for (size_t n = 0, span = project_->samples.span(); n < span; ++n) used += project_->samples[n].used();
            bool taking = chop_take_playing(*project_, ui_, now); // Manual's take still playing (shell/zoom_view.h)
            chop_bits.store((uint32_t(ui_.chop_mode) & 3) | (uint32_t(ui_.chop_mode) >> 2 & 1) << 4 | uint32_t(chop_armed(ui_)) << 2 | uint32_t(taking) << 3 | uint32_t(ui_.slices.count & 0xff) << 8
                            | uint32_t(ui_.chop_slice) << 16 | uint32_t(used) << 24, std::memory_order_relaxed);
            pad_start_now.store(pad.start, std::memory_order_relaxed); pad_end_now.store(pad.end, std::memory_order_relaxed);
            files_bits.store(uint32_t(ui_.files_count) | uint32_t(ui_.files_cursor & 0x3fff) << 16 | uint32_t(ui_.files_drive & 1) << 30
                             | uint32_t(ui_.settings.preview) << 31, std::memory_order_relaxed);
            std::string folder = library_ ? library_->folder() : std::string();
            std::lock_guard<std::mutex> guard(text_lock);
            pad_name_text = pad.used() ? project_->samples[pad.sample % sample_slots].name : "";
            pad_root_now.store(pad.used() ? project_->samples[pad.sample % sample_slots].root : root_none, std::memory_order_relaxed);
            folder_text = folder.empty() ? folder : folder.substr(folder.find_last_of('/') + 1);
            const auto* last = context_.edits.last();
            undo_text = last ? last->what : std::string();
        }
        // phase 10: the undo history's depth and the redo side, Seq held, the sampler map, a gesture open
        phase10_bits.store(uint32_t(context_.edits.size()) | uint32_t(context_.edits.can_redo()) << 8 | uint32_t(ui_.seq_held) << 9
                           | uint32_t(ui_.map_open) << 10 | uint32_t(bool(context_.gesture.edit)) << 12
                           | uint32_t(project_->song.count & 0xfff) << 16 | uint32_t(project_->tracks[ui_.track].engine) << 28,
                           std::memory_order_relaxed);
        jam_bits.store(uint32_t(ui_.jam_bars & 0x3ff) | uint32_t(ui_.jam_logging) << 10 | uint32_t(ui_.jam_count & 0xf) << 11
                       | uint32_t(ui_.jam_sheet) << 15 | uint32_t(project_->song.marker_count & 0x1f) << 16
                       | uint32_t(project_->song.tempo.count & 0x7f) << 21, std::memory_order_relaxed);
        if (context_.gesture.edit && now - context_.gesture.last >= gesture_quiet_ms) context_.close_gesture(); // a pot left still
    }
    const auto& project = *project_;
    const auto& track = project.tracks[ui.track];
    bool playing = host.playing();
    uint32_t ticks = host.ticks();
    if (ui.light_show && light_show_pass(lights, now)) return;
    show_was_on = false;
    { // The pads (shell/pad_lights.cpp): they light as what a press does there, chosen as Shell::hit() chooses it.
        unsigned beat = host.count_in();
        if (beat != count_beat_seen) { count_beat_seen = beat; count_beat_at = beat ? now : 0; }
        host.watch_taps(int(ui.track % track_count)); // the screen asks the same: the selected track's voices
        TrackTaps taps;
        if (!host.taps(taps) || taps.track != ui.track % track_count) taps = TrackTaps {};
        PadScene scene {ui, project, *patterns_, host, taps, pad_facts(), now, &pad_let_go, count_beat_at};
        scene.freeze = &context_.prints.freeze; // freeze plan F2c
        scene.glow = &pad_glow;
        if (ui.page == PageId::Print && context_.printer) {
            auto job = context_.printer->progress();
            if (job.busy && job.freeze && job.track == ui.print_sheet.track) { scene.printed = job.done; scene.printing = job.pattern; }
        }
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
    // Bank lights A-H: Time Correct's B2 GROOVE held, the bank of patterns it shows; with Seq held or on Patterns, the bank of patterns in view; else the sampler track's bank in view, or the Drums
    // pads' (A-D), or the Prog pads' (A-C); on Chop, the slices on the pads (A 1-16, B 17-32 ...): while Manual's take
    // plays, the bank of the red pad's slice (model/slice_banks.h).
    const unsigned chop_view = chop_bank_slice(project, ui, now); // shell/zoom_view.h
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank)
        lights.bank(bank, ui.page == PageId::TimeCorrect && ui.tc_groove_held && !ui.seq_held ? bank == ui.tc_groove_bank % pattern_banks // B2 GROOVE held
                                      : ui.seq_held || ui.page == PageId::Patterns || ui.page == PageId::Scenes ? bank == ui.pattern_bank % pattern_banks
                                      : ui.page == PageId::Chop ? bank == slice_bank(chop_view)
                                      : (track.engine == EngineKind::Sampler && bank == track.bank % sampler_banks) || (drum_pads(track) && bank == drum_bank(track))
                                        || (prog_pads(track) && bank == prog_bank(track)));
    { // The meters beside the screen, as on the MPC: the input source while Record or Input Config is in view or a
      // take waits or runs (what is about to be sampled), else main out; read every 50 ms (meter_lights.h).
        using S = TakeRecorder::State;
        auto take = host.recorder().state();
        bool input = ui.page == PageId::Record || ui.page == PageId::InputConfig || take == S::Armed || take == S::Cued
                  || take == S::Recording;
        const auto& lit = meter_reads.at(now, input, [&](unsigned side) { return input ? host.in_level(side) : host.out_level(side); });
        uint32_t bits = uint32_t(input) << 16;
        for (unsigned side = 0; side < surface_meter_lights; ++side) {
            lights.meter(side, lit[side]);
            bits |= uint32_t(lit[side]) << (8 * side);
        }
        meter_bits.store(bits, std::memory_order_relaxed);
    }

    using B = SurfaceButton;
    using L = ButtonLight;
    // As on the stock MPC: bright in use, faint where the button has a job here, dark where it has none.
    bool erase_pots = false;
    {
        auto bindings = page_bindings(project, ui, context_.pattern());
        for (const auto& b : bindings) erase_pots |= erase_resets(b);
    }
    bool kit = kit_pads();
    uint64_t levels = 0; // two bits a button, for `status`
    auto set = [&](B button, L light) {
        lights.button(button, light);
        levels = (levels & ~(uint64_t(3) << (2 * unsigned(button)))) | uint64_t(light) << (2 * unsigned(button)); // the last set wins
    };
    bool keys = key_pads();
    auto job = [&](B button) { return button_has_job(project, ui, button, kit, erase_pots, keys); };
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
    if (ui.lesson && !ui.menu) { // a lesson (shell/lesson.h): the step's button blinks (and the one held with it), every other is off
        const bool lit = (now / 250) % 2;
        for (unsigned n = 0; n < unsigned(B::Count); ++n)
            if (B(n) != B::TopLeft && B(n) != B::TopMiddle && B(n) != B::TopRight) // B1-B3 have no lights
                set(B(n), lit && lesson_lights_button(ui, B(n)) ? L::Bright : L::Off);
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
            bool shift_job = button_has_shift_job(project, ui, button, kit, keys);
            bool lit = button == B::Shift || (button == B::TapTempo && ui.settings.click) || (button == B::Erase && ui.copying != UiState::Copying::Off)
                    || (button == B::NoteRepeat && track.repeat) || (button == B::Mute && ui.mute_mode())
                    || (track.engine == EngineKind::Sampler && button == B::Loop && (ui.page == PageId::Chop && ui.chop_slice < ui.slices.count
                            ? slice_heard_reversed(track, ui.chop_sample, ui.slices, ui.chop_slice)              // on Chop, the slice in hand's
                            : track.pads[track.last_pad % sampler_pads].reverse))                                // reverse on
                    || (track.engine == EngineKind::Sampler && button == B::Chop && track.pads[track.last_pad % sampler_pads].play == PadPlay::Gate); // Note On
            set(button, !shift_job ? L::Off : lit ? L::Bright : L::Dim);
        }
        button_levels.store(levels, std::memory_order_relaxed);
        return;
    }
    bool sound_hub = ui.page == PageId::Stage || is_sampler_page(ui.page) || ui.sample_held; // Sample's hub (docs/HUBS_PLAN.md)
    on(B::Sample, sound_hub || ui.page == PageId::InputConfig);
    on(B::Seq, ui.page == PageId::StepEdit || ui.page == PageId::Patterns || ui.page == PageId::Scenes || ui.page == PageId::Roll || ui.page == PageId::Motion || ui.page == PageId::ModPage); // its hub
    on(B::SampleSelect, ui.page == PageId::Sounds || ui.page == PageId::Samples || sounds_open(ui)); // Step Edit: the sounds on the pads
    on(B::Chop, ui.page == PageId::Chop);
    on(B::NoteRepeat, track.repeat && ui.page != PageId::Song); // on Song it repeats the clip
    {
        auto state = host.recorder().state();
        bool blink = (now / 250) % 2;
        set(B::SampleRecord, state == TakeRecorder::State::Recording ? L::Bright
                      : state == TakeRecorder::State::Armed || state == TakeRecorder::State::Cued ? (blink ? L::Bright : L::Dim)
                      : job(B::SampleRecord) ? L::Dim : L::Off);
    }
    on(B::PadFx, ui.page == PageId::Punch || ui.page == PageId::FlexBeat || ui.engaged_count || host.flex_playing());
    on(B::KnobFx, project.knob_fx.on); // the mode, as on the MPC (Shift + Knob FX opens the chains)
    const bool editor = ui.page == PageId::Roll || ui.page == PageId::StepEdit;
    on(B::Loop, ui.page == PageId::Looper || ui.loop_held || (ui.page == PageId::Song ? project.song.looping()
                    : editor ? ui.edit_loop
                    : track.engine == EngineKind::Sampler && track.pads[track.last_pad % sampler_pads].loop)); // the song's brace on Song, the editor's loop, else the last pad loops; the Looper
    // 16 Levels: lit while the pads play levels (a kit pad or a synth's last note on all sixteen, or a synth's Levels layout).
    on(B::SixteenLevels, track.levels || (track.engine != EngineKind::Sampler && track.layout == PadLayout::Levels));
    on(B::Play, playing);
    on(B::Stop, false);
    // Seq Record: bright while recording, flashing while armed (Play starts the take) or counting in.
    if (ui.rec_armed || (ui.recording && host.count_in())) set(B::SeqRecord, (now / 250) % 2 ? L::Bright : L::Dim);
    else on(B::SeqRecord, ui.recording);
    // Tap Tempo flashes on the beat while playing.
    unsigned beat = project.beat_ticks();
    on(B::TapTempo, ui.settings.tap_light && playing && ticks % beat < beat / 3); // Settings > LIGHTS > Tap Tempo light
    on(B::Mute, ui.mute_mode());           // bright while mute mode is on, latched or held
    on(B::PadBank, tracks_on_pads(ui) || ui.page == PageId::Mixer); // held, a track copy under way, or the mixer: the pads pick tracks
    on(B::Erase, ui.erase || ui.copying != UiState::Copying::Off); // lit while copying
    button_levels.store(levels, std::memory_order_relaxed);
}

} // namespace melodee
