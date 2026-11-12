// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Generators (roadmap P1, model/generators.h) from Steps: the EUCLID row of
// cards (STEPS pressed again twice, a kit) lays a Euclidean rhythm on the last
// pad's lane as its hits, steps and rotation turn; Push's VARY sheet makes a
// variation of the pattern into the next empty one, which then plays.
#include "melodee/model/generators.h"
#include "melodee/model/scales.h"
#include "melodee/shell/context.h"
#include "melodee/shell/step_lane.h"
#include <algorithm>

namespace melodee {

void Context::set_euclid(Binding::Kind kind, int value) {
    using Kind = Binding::Kind;
    auto& t = track();
    int lane = steps_lane(t);
    if (lane < 0) return;
    auto& p = pattern();
    const EuclidLane* now = euclid_of(p, unsigned(lane));
    unsigned hits = now ? now->hits : 0, steps = now ? now->steps : std::min(p.steps(), euclid_max_steps), rotate = now ? now->rotate : 0;
    if (kind == Kind::EuclidHits) hits = unsigned(std::max(value, 0));
    else if (kind == Kind::EuclidSteps) steps = unsigned(std::clamp(value, 1, int(euclid_max_steps)));
    else if (kind == Kind::EuclidRotate) rotate = unsigned(std::max(value, 0));
    else return;
    hits = std::min(hits, steps); rotate %= steps;
    if (now && now->hits == hits && now->steps == steps && now->rotate == rotate) return;
    if (!lay_euclid(p, uint8_t(lane), hits, steps, rotate)) notice("The pattern is full");
    sync_pattern(ui.track, t.pattern);
    ui.held_edited = true;
}

void Context::vary(Variation v) {
    auto& t = track();
    if (!t.used()) return;
    bool kit = t.engine == EngineKind::Sampler || t.engine == EngineKind::Kit909;
    if (kit && !variation_on_kit(v)) { notice(std::string(variation_name(v)) + ": not on a kit"); return; }
    if (pattern().empty()) { notice("No notes to vary"); return; }
    int into = -1; // the next empty pattern after the one playing, round to the first (as Capture)
    for (unsigned k = 1; k <= pattern_count && into < 0; ++k)
        if (patterns.at[ui.track][(t.pattern + k) % pattern_count].empty()) into = int((t.pattern + k) % pattern_count);
    if (into < 0) { notice("No empty pattern for a variation"); return; }
    VaryPlace place;
    place.kit = kit; place.bar = project.bar_ticks(); place.beat = project.beat_ticks(); place.root = project.root;
    place.degree_count = scale_degrees(project.scale, place.degrees);
    Pattern made = vary_pattern(pattern(), v, ui.vary_amount / 100.0, uint64_t(now) * 2654435761ull + unsigned(v), place);
    std::string what = std::string(variation_name(v)) + " into P" + std::to_string(into + 1);
    edit_pattern(ui.track, unsigned(into), what.c_str(), [&] {
        patterns.at[ui.track][unsigned(into)] = made;
        sync_pattern(ui.track, unsigned(into));
    });
    launch(ui.track, unsigned(into)); // it plays from the next bar (stopped: at once)
    notice(what);
}

} // namespace melodee
