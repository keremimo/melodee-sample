// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Stage's jobs on the MIDI thread (pages/stage_page.cpp draws it): PRESET's
// sound loading once the turning stops, and an empty track becoming a synth,
// a kit or a MIDI track.
#include "melodee/shell/context.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/sound_list.h"
#include "melodee/shell/stage_map.h"
#include <algorithm>

namespace melodee {

void Context::preset_turn(int at) {
    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(track(), ui, list);
    if (!count || !track().used()) return;
    if (frozen_edit(UndoScope {UndoScope::Kind::Sound, uint8_t(ui.track % track_count), 0})) return; // freeze plan F2c
    ui.preset_cursor = uint16_t(std::clamp(at, 0, int(count) - 1));
    ui.preset_due = std::max<uint32_t>(now + stage_preset_ms, 1);
}

void Context::stage_tick(uint32_t at) {
    if (!ui.preset_due || at < ui.preset_due) return;
    ui.preset_due = 0;
    std::array<uint16_t, max_listed> list {};
    unsigned count = sound_list(track(), ui, list);
    if (!count || !track().used()) return;
    unsigned sound = list[std::min<unsigned>(ui.preset_cursor, count - 1)];
    if (sound != track().preset) load_sound(ui.track, track().engine, sound);
}

void Context::stage_new() {
    auto& t = track();
    if (t.used()) return;
    const auto& page = stage_page_now(ui, t);
    switch (page.kind) {
    case StageKind::NewSynth: {
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        unsigned count = new_engines(engines);
        if (!count) return;
        auto kind = engines[ui.new_engine % count];
        load_sound(ui.track, kind, std::min<unsigned>(ui.new_sound, std::max(engine_info(kind).sound_count, 1u) - 1));
        stage_go(ui, 0, 0); // PRESET
        if (kind == EngineKind::Plugin) open_plugin_picker(UiState::PluginTarget::Instrument); // which plug-in, from the card
        break;
    }
    case StageKind::NewKit:
        load_sound(ui.track, EngineKind::Sampler, 0);
        t.set_name("KIT");
        stage_go(ui, 0, 0); // TRIM
        switch (ui.new_source % new_sources) {
        case 0: open(PageId::Samples); if (library) library->rescan(); break; // Sample Select
        default: open(PageId::Record); break;                                 // Sample Record
        }
        break;
    case StageKind::NewMidi:
        load_sound(ui.track, EngineKind::Midi, 0);
        set_param(ui.track, midi_track::Port, ui.new_port % midi_track::PortCount);
        set_param(ui.track, midi_track::Channel, std::clamp<int>(ui.new_channel, 1, 16));
        stage_go(ui, 0, 0); // DEVICE
        break;
    default: return;
    }
    dirty = true;
}

} // namespace melodee
