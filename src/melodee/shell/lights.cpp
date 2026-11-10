// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad, fader and button lights for the page in view (MIDI thread, every
// 5 ms; the runtime sends only what changed). Colours come from the track
// palette; button light values from surface/leds.h.
#include "melodee/engine/engine_host.h"
#include "melodee/model/project_file.h"
#include "melodee/model/scales.h"
#include "melodee/shell/shell.h"
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
    // Power was pressed: the Exit sheet opens (the service thread only counts presses).
    // The library found files or finished a sample: the lengths go into the project.
    if (library_ && library_->generation() != library_seen) {
        library_seen = library_->generation();
        for (unsigned n = 0; n < sample_slots; ++n)
            if (project_->samples[n].used()) project_->samples[n].frames = library_->slot(n).frames;
        unsigned listed = 0;
        for (const auto& f : library_->files()) listed += context_.files_match(f.path);
        ui_.files_count = uint16_t(listed);
        // A take has landed: it becomes the slot it was armed for, on the pad chosen then.
        if (ui_.rec_slot >= 0) {
            auto take = library_->slot(unsigned(ui_.rec_slot));
            if (take.ready || !take.error.empty()) {
                auto& ref = project_->samples[size_t(ui_.rec_slot)];
                std::snprintf(ref.path, sizeof(ref.path), "%s", take.path.empty() ? ("(memory) " + take.name).c_str() : take.path.c_str());
                std::snprintf(ref.name, sizeof(ref.name), "%s", take.name.c_str());
                ref.frames = take.frames;
                if (take.ready && ui_.rec_pad >= 0) {
                    auto& pad = project_->tracks[ui_.track].pads[size_t(ui_.rec_pad)];
                    pad = PadSound {}; pad.sample = uint16_t(ui_.rec_slot);
                    project_->tracks[ui_.track].last_pad = uint8_t(ui_.rec_pad);
                    context_.send_pad(ui_.track, unsigned(ui_.rec_pad));
                }
                ui_.rec_slot = ui_.rec_pad = -1;
            }
        }
        if (ui_.page == PageId::Chop && !ui_.slices.count) context_.rechop();
    }
    // Projects: a finished pack to free, a loaded project to switch to, a save to leave after.
    {
        const ProjectPack* done = nullptr;
        while (host.take_done_pack(done)) delete done;
    }
    if (store_) {
        if (auto loaded = store_->take_loaded()) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            if (loaded->error.empty() && load_project(loaded->bytes, *project, *bank).empty())
                context_.switch_to(*project, *bank, loaded->path);
        }
        if (leave_after_save >= 0 && int(store_->saves()) >= leave_after_save) leave.store(true);
    }
    context_.song_tick();
    // Melodee's settings, once the card's file has been read.
    if (store_ && !settings_loaded && store_->generation() != settings_seen) {
        settings_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        if (v.settings_read) { ui_.settings.read(v.settings); settings_loaded = true; }
    }
    // The backlight stays as the stock app left it until Screen asks for less (and then follows it).
    if (ui_.settings.screen != 100 || screen_wanted.load(std::memory_order_relaxed)) screen_wanted.store(ui_.settings.screen, std::memory_order_relaxed);
    // The Project page's row count, for K2's range.
    if (store_ && ui_.page == PageId::Project && store_->generation() != store_seen) {
        store_seen = store_->generation();
        ProjectStore::View v;
        store_->view(v);
        ui_.project_rows = uint16_t(v.entries.size() + (context_.project_path.empty() ? 2 : 1));
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
    // Record listens to the mic or the line inputs while its page is open or a take runs.
    if (library_) {
        bool busy = host.recorder().state() != TakeRecorder::State::Idle;
        library_->want_input((ui_.page == PageId::Record || busy) && ui_.rec_source < 2 ? int(ui_.rec_source) : -1);
    }
    unsigned presses = power.load(std::memory_order_relaxed);
    if (presses != power_seen) { power_seen = presses; ui_.exit_open = true; }

    const auto& ui = ui_;
    {
        const auto& t = project_->tracks[ui_.track];
        const auto& pad = t.pads[t.last_pad % sampler_pads];
        page_track.store(uint32_t(ui_.page) | uint32_t(ui_.track) << 8, std::memory_order_relaxed);
        tempo_now.store(project_->tempo, std::memory_order_relaxed);
        pad_sample.store(pad.sample, std::memory_order_relaxed);
        pad_frames.store(pad.used() ? project_->samples[pad.sample % sample_slots].frames : 0, std::memory_order_relaxed);
    }
    const auto& project = *project_;
    const auto& track = project.tracks[ui.track];
    bool playing = host.playing();
    int step_now = host.step_of(ui.track);
    uint32_t ticks = host.ticks();
    for (unsigned pad = 0; pad < 16; ++pad) {
        bool held = ui.held_pads & (1u << pad);
        Rgb colour {};
        if (ui.pad_bank || ui.mute || ui.page == PageId::Stage || ui.page == PageId::Mix || ui.page == PageId::Song) {
            // The pads are the tracks.
            const auto& t = project.tracks[pad];
            float level = !t.used() ? 0 : pad == ui.track ? 1.0f : t.mix.mute ? 0.08f : 0.35f;
            colour = track_light(pad, level);
            if (ui.mute && t.used()) colour = t.mix.mute ? Rgb {} : (t.mix.solo ? led_colour::white(0.8f) : track_light(pad, 0.6f));
        } else if (ui.page == PageId::Patterns) {
            // The pads are the selected track's sixteen patterns: playing bright, queued
            // blinking, holding notes dim, empty dark.
            bool notes = !patterns_->at[ui.track][pad].empty();
            bool live = host.pattern_of(ui.track) == pad, queued = playing && track.pattern == pad && !live;
            float level = live ? 1.0f : queued ? ((now / 150) % 2 ? 0.8f : 0.15f) : notes ? 0.25f : 0;
            colour = track_light(ui.track, level);
        } else if (ui.page == PageId::Steps) {
            const auto& pattern = context_.pattern();
            unsigned step = ui.steps_bar * bar_steps + pad;
            bool inside = step < pattern.length, on = inside && !pattern.steps[step].empty();
            colour = on ? track_light(ui.track, 0.8f) : inside ? Rgb {} : Rgb {};
            if (playing && inside && step_now == int(step)) colour = led_colour::white(0.7f);
            if (int(step) == ui.held_step) colour = led_colour::white(1);
        } else if (ui.page == PageId::Punch) {
            // The bank's punches: held or locked bright, in white on the master.
            unsigned kind = project.punch_bank % 2 * 16 + pad;
            float level = kind >= punch_kinds ? 0 : (ui.punch_held == int(kind) || ui.punch_locked == int(kind)) ? 1.0f : 0.25f;
            colour = project.punch_target ? track_light(ui.track, level) : led_colour::white(level * 0.7f);
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
        if (held && ui.page != PageId::Steps && !ui.pad_bank && !ui.mute) colour = led_colour::white(1);
        if (ui.settings.pad_lights == 1) { // white, as bright as the colour was
            float full = std::max<float>(led_colour::white(1).r, 1);
            colour = led_colour::white(std::max({colour.r, colour.g, colour.b}) / full);
        }
        if (ui.settings.pad_lights == 2) colour = {};
        lights.pad(pad, colour);
    }
    lights.fader(track.used() ? track_light(ui.track, 0.8f) : Rgb {});
    // Bank lights A-D: the sampler track's bank in view.
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank)
        lights.bank(bank, track.engine == EngineKind::Sampler && bank == track.bank % sampler_banks);

    using B = SurfaceButton;
    using L = ButtonLight;
    auto on = [&](B button, bool lit) { lights.button(button, lit ? L::Bright : L::Off); };
    bool play_group = ui.page == PageId::Stage || ui.page == PageId::Keys || ui.page == PageId::Pads || ui.page == PageId::Mix;
    on(B::Sample, play_group);
    on(B::Seq, ui.page == PageId::Steps || ui.page == PageId::Roll || ui.page == PageId::Motion);
    on(B::SampleSelect, ui.page == PageId::Sounds || ui.page == PageId::Samples);
    on(B::Chop, ui.page == PageId::Chop);
    on(B::NoteRepeat, track.arp);
    {
        auto state = host.recorder().state();
        bool blink = (now / 250) % 2;
        lights.button(B::SampleRecord, state == TakeRecorder::State::Recording ? ButtonLight::Bright
                      : state == TakeRecorder::State::Armed ? (blink ? ButtonLight::Bright : ButtonLight::Dim)
                      : ui.page == PageId::Record ? ButtonLight::Dim : ButtonLight::Off);
    }
    on(B::PadFx, ui.page == PageId::Punch || ui.page == PageId::PadPunch);
    on(B::KnobFx, ui.page == PageId::PadFx || ui.page == PageId::Rack || ui.page == PageId::Sends || ui.page == PageId::Bus || ui.page == PageId::Master);
    on(B::Loop, ui.page == PageId::Patterns || ui.page == PageId::Song || ui.page == PageId::Length);
    on(B::SixteenLevels, ui.page == PageId::Keys);
    lights.button(B::Play, playing ? L::Bright : L::Dim);
    lights.button(B::Stop, playing ? L::Dim : L::Off);
    lights.button(B::SeqRecord, ui.recording ? L::Bright : L::Dim);
    // Tap Tempo flashes on the beat while playing.
    bool beat = playing && ticks % ticks_per_quarter < ticks_per_quarter / 3;
    lights.button(B::TapTempo, beat ? L::Bright : L::Dim);
    on(B::Mute, ui.mute);
    on(B::PadBank, ui.pad_bank);
    lights.button(B::Erase, ui.erase ? L::Bright : L::Off);
}

} // namespace melodee
