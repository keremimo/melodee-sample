// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The question asked before an erase (shell/ask.cpp), over the page: what
// goes and how much, B1 CANCEL, B3 DO IT, as the MPC asks it.
#include "melodee/pages/common.h"
#include "melodee/shell/ask.h"

namespace melodee {
using namespace ui;

void draw_ask_sheet(Paint& p, const Snapshot& s) {
    p.fill(0, zone::soft_y, zone::width, zone::soft_h, colour::bg);
    bool reset = s.ui.ask == UiState::Ask::ResetSettings; // the MPC asks this one NO / YES
    soft_keys(p, {{reset ? "NO" : "CANCEL", "", reset ? "YES" : "DO IT"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
    p.dim_zone(zone::picture_y, zone::picture_h, 0.7f);
    p.dim_zone(zone::knobs_y, zone::knobs_h);
    float w = 220, h = 54, x = (zone::width - w) / 2, y = zone::picture_y + (zone::picture_h - h) / 2;
    p.fill(x, y, w, h, colour::panel, 6);
    p.ring(x, y, w, h, colour::rec, 6);
    p.text(ask_title(s.project, s.ui), x + 10, y + 9, w - 20, 20, plain(colour::text, 13, Weight::SemiBold), juce::Justification::centred);
    p.text(ask_detail(s.project, s.ui, s.pattern), x + 10, y + 32, w - 20, 12, plain(colour::sec, 9), juce::Justification::centred);
}

} // namespace melodee
