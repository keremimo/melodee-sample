// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A track's pattern chain (round 2, seq-sheet2 B): the patterns tapped in
// order while Seq is held, each played once through and the next at its end,
// round again; a song for one track without opening Song. The first tap
// launches as ever (at the next bar while playing) and starts the chain; each
// tap after it in the same hold adds a pattern. The chain is queued a
// pattern at a time: in the last bar of the one playing the next is launched
// for the bar line, where it begins as this one ends. A pattern launched by
// hand, a single tap, ends it; the song playing leaves it be.
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include <string>

namespace melodee {

void Context::chain_start(unsigned pattern) {
    auto& tr = track();
    tr.chain = {};
    tr.chain[0] = uint8_t(pattern % pattern_count);
    tr.chain_count = 1;
    tr.chain_at = 0;
    chain_queued[ui.track % track_count] = false;
}

void Context::chain_add(unsigned pattern) {
    auto& tr = track();
    if (tr.chain_count >= max_chain) { notice("Chain full"); return; }
    if (!tr.chain_count) { chain_start(pattern); return; }
    tr.chain[tr.chain_count++] = uint8_t(pattern % pattern_count);
    std::string shown;
    for (unsigned n = 0; n < tr.chain_count && n < 4; ++n) shown += (n ? " > " : "") + std::string("P") + std::to_string(tr.chain[n] + 1);
    if (tr.chain_count > 4) shown += " +" + std::to_string(tr.chain_count - 4);
    notice("Chain " + shown);
    dirty = true;
}

void Context::chain_tick() {
    bool running = host.playing() && !host.song_playing();
    uint32_t bar = std::max(1u, project.bar_ticks());
    for (unsigned t = 0; t < track_count; ++t) {
        auto& tr = project.tracks[t];
        if (!running || !tr.chained()) { chain_queued[t] = false; continue; }
        unsigned playing = host.pattern_of(t) % pattern_count;
        if (playing != tr.chain[tr.chain_at]) { // the queued one began (or the chain is still to start): find it in the chain
            for (unsigned k = 1; k < tr.chain_count; ++k)
                if (tr.chain[(tr.chain_at + k) % tr.chain_count] == playing) { tr.chain_at = uint8_t((tr.chain_at + k) % tr.chain_count); break; }
            if (playing != tr.chain[tr.chain_at]) continue; // not one of the chain's yet: the first waits for its bar
            chain_queued[t] = false;
        }
        if (chain_queued[t] || !chain_due(host.position_of(t), patterns.at[t][playing].length, bar)) continue;
        chaining = true;
        launch(t, tr.chain_ahead(1)); // for the bar line: it begins as this one ends
        chaining = false;
        chain_queued[t] = true;
    }
}

} // namespace melodee
