// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Pad, fader and button lights for the page in view (MIDI thread, every
// 5 ms; the runtime sends only what changed). Colours come from the track
// palette; button light values from surface/leds.h.
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include "melodee/shell/shell.h"
#include "melodee/ui/tokens.h"
#include "surface/led_colours.h"

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
    unsigned presses = power.load(std::memory_order_relaxed);
    if (presses != power_seen) { power_seen = presses; ui_.exit_open = true; }

    const auto& ui = ui_;
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
        } else if (track.used()) {
            auto note = pad_note(project, track, pad);
            colour = track_light(ui.track, note.root ? 0.9f : 0.22f);
        }
        if (held && ui.page != PageId::Steps && !ui.pad_bank && !ui.mute) colour = led_colour::white(1);
        lights.pad(pad, colour);
    }
    lights.fader(track.used() ? track_light(ui.track, 0.8f) : Rgb {});
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, false);

    using B = SurfaceButton;
    using L = ButtonLight;
    auto on = [&](B button, bool lit) { lights.button(button, lit ? L::Bright : L::Off); };
    bool play_group = ui.page == PageId::Stage || ui.page == PageId::Keys || ui.page == PageId::Mix;
    on(B::Sample, play_group);
    on(B::Seq, ui.page == PageId::Steps || ui.page == PageId::Roll || ui.page == PageId::Motion);
    on(B::SampleSelect, ui.page == PageId::Sounds);
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
