// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine_host.h"
#include <string>

// Roadmap P4 (mix, effects, MIDI): what `status` reports of it, for checks from afar.
namespace melodee {

// Plug-in delay compensation: the frames everything is held back by; each track's own latency (its plug-in and
// chain), the buses' and the master's chain's, comma separated.
inline std::string p4_summary(const EngineHost& host) {
    std::string tracks, buses;
    for (unsigned t = 0; t < track_count; ++t) tracks += (t ? "," : "") + std::to_string(host.track_latency(t));
    for (unsigned b = 0; b < bus_count; ++b) buses += (b ? "," : "") + std::to_string(host.bus_latency(b));
    return " melodee_pdc=" + std::to_string(host.compensation()) + " melodee_pdc_tracks=" + tracks + " melodee_pdc_buses=" + buses +
           " melodee_pdc_master=" + std::to_string(host.master_latency()) +
           " melodee_fx_pool=" + std::to_string(host.delay_blocks_free()) + "/" + std::to_string(DelayPool::blocks); // free / all
}

} // namespace melodee
