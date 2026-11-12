// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"

// The sampler's pages in the MPC's Sample Mode groups (parity phase 10):
// B1 Trim / Mix / Amp Env, B2 Tune / Play, B3 Filter / Filt Env; Layers
// (roadmap P3) after Trim, the samples a pad plays. Melodee's
// own pages join the group they belong with: Follow and Cloud B2's, Kit
// filter B3's. A B key on a page of its group goes to the group's next
// page, on any other page to the group's first; Push opens the map of all
// ten (pages/sampler_common.cpp). Pure, for the pages and the tests.
namespace melodee {

constexpr unsigned sampler_page_count = 11;
constexpr unsigned sampler_groups = 3;
constexpr PageId sampler_order[sampler_page_count] {PageId::Trim, PageId::LayersPage, PageId::PadMix, PageId::EnvelopePage, PageId::PadPage,
                                                    PageId::PlayPage, PageId::FollowPage, PageId::CloudPage, PageId::PadFilterPage,
                                                    PageId::FilterEnvPage, PageId::FilterPage};
constexpr unsigned sampler_group_starts[sampler_groups + 1] {0, 4, 8, sampler_page_count};

constexpr bool is_sampler_page(PageId page) {
    for (auto p : sampler_order) if (p == page) return true;
    return false;
}
constexpr unsigned sampler_index(PageId page) {
    for (unsigned n = 0; n < sampler_page_count; ++n) if (sampler_order[n] == page) return n;
    return 0;
}
constexpr unsigned sampler_group(unsigned n) {
    unsigned g = 0;
    while (g + 1 < sampler_groups && n >= sampler_group_starts[g + 1]) ++g;
    return g;
}
constexpr unsigned sampler_group_first(unsigned g) { return sampler_group_starts[g % sampler_groups]; }
constexpr unsigned sampler_group_size(unsigned g) { return sampler_group_starts[g % sampler_groups + 1] - sampler_group_starts[g % sampler_groups]; }
// What B1-B3 (`key` 0-2) open from `page`.
constexpr PageId sampler_next(PageId page, unsigned key) {
    unsigned g = key % sampler_groups, first = sampler_group_first(g), n = sampler_index(page);
    if (!is_sampler_page(page) || sampler_group(n) != g) return sampler_order[first];
    return sampler_order[first + (n - first + 1) % sampler_group_size(g)];
}

} // namespace melodee
