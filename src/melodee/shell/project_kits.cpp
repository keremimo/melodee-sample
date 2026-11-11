// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/project_kits.h"
#include "melodee/engine/engine_info.h"
#include "melodee/model/scales.h"
#include <cstdio>

namespace melodee {
// A new project: one empty kit on track 1, every note in reach.
void starting_project(Project& project, PatternBank& patterns) {
    project = Project {};
    std::snprintf(project.name, sizeof(project.name), "Untitled");
    project.tempo = 1200;
    project.root = 0; project.scale = chromatic_scale; // C Chromatic
    load_sound(project.tracks[0], EngineKind::Sampler, 0); // Sample Select fills its pads
    patterns = PatternBank {};
}

int kit_track(const Project& from) {
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = from.tracks[t];
        if (tr.engine != EngineKind::Sampler) continue;
        for (const auto& pad : tr.pads) if (pad.used()) return int(t);
    }
    return -1;
}

KitImport import_kit(const Project& from, unsigned track, Track& into, const std::function<int(const SampleRef&)>& slot_for) {
    KitImport result;
    const auto& src = from.tracks[track % track_count];
    into.engine = EngineKind::Sampler;
    into.preset = src.preset;
    into.params = src.params;
    into.set_name(src.name);
    into.pad_fx = src.pad_fx;
    into.pad_mutes = {};
    into.bank = 0; into.last_pad = 0; into.levels = false;
    std::array<int, sample_slots> mapped;
    mapped.fill(-2); // -2 not asked yet
    for (unsigned n = 0; n < sampler_pads; ++n) {
        PadSound pad = src.pads[n];
        if (pad.used()) {
            unsigned s = pad.sample % sample_slots;
            if (mapped[s] == -2) mapped[s] = from.samples[s].used() ? slot_for(from.samples[s]) : -1;
            if (mapped[s] < 0) { pad = PadSound {}; into.pad_fx.at[n] = Chain {{}, pad_slots}; ++result.missing; }
            else { pad.sample = uint16_t(mapped[s]); ++result.pads; }
        }
        into.pads[n] = pad;
    }
    return result;
}

KitImport import_bank(const BankFile& from, unsigned bank, Track& into, const std::function<int(const SampleRef&)>& slot_for) {
    KitImport result;
    std::array<int, bank_pads> mapped;
    mapped.fill(-2); // -2 not asked yet
    for (unsigned k = 0; k < bank_pads; ++k) {
        unsigned n = (bank % sampler_banks) * bank_pads + k;
        PadSound pad = from.pads[k];
        Chain chain = from.chains[k];
        if (pad.used()) {
            unsigned s = pad.sample % bank_pads;
            if (mapped[s] == -2) mapped[s] = from.samples[s].used() ? slot_for(from.samples[s]) : -1;
            if (mapped[s] < 0) { pad = PadSound {}; ++result.missing; }
            else { pad.sample = uint16_t(mapped[s]); ++result.pads; }
        }
        if (!pad.used()) chain = Chain {{}, pad_slots};
        into.pads[n] = pad;
        into.pad_fx.at[n] = chain;
        into.set_pad_muted(n, false);
    }
    return result;
}

} // namespace melodee
