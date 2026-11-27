// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include <array>
#include <string>
#include <vector>

// The controls audit of the --melodee-preview run (tools.cpp): each preview's B1-B3 and K1-K3 labels as drawn
// (ui::ControlsSeen), checked against the rules the screens keep, so a new page or sheet cannot break them unseen.
// A break is printed as MELODEE_CONTROLS and fails the run, as text under the floor does; every row goes into the
// preview folder's controls.tsv.
//
//   back:  BACK (or a key drawn with the Back icon) is B1 only.
//   sends: the cards go SEND, BUS, RETURN from K1 (a kit's KIT SEND counts as SEND); not on Motion, whose cards
//          are the lanes it holds, in lane order.
//   seq:   a page of the SEQ hub keeps its row: B1 SEQUENCES or PATTERNS, B2 ROLL, B3 MOTION, MOD, MACRO or none.
//   mix:   a page of the MIX hub keeps its row: B1 MIXER, B2 empty, B3 FX. Loop and submix chains have their own row.
// The hub rows count only when nothing is over the page's own row (controls_overlay).
namespace melodee {

struct ControlsRow {
    PageId page = PageId::Stage;
    bool overlay = false; // controls_overlay(): a held key, Shift, a sheet, a question or the MENU over the page
    bool loop_chain = false; // a Looper loop's chain (or a mixer submix's) on the Rack page, not the MIX hub's
    bool keys_drawn = true;  // the frame drew a soft-key row (a full-screen sheet may not): the hub rules need one
    std::array<std::string, 3> keys, knobs;
    std::array<bool, 3> back {};
};

// Whether something other than the page's own B1-B3 row may be drawn: held keys and Shift redraw it, sheets,
// questions, pickers, the MENU, Name and Exit draw their own.
inline bool controls_overlay(const UiState& ui) {
    return ui.shift || ui.ask != UiState::Ask::None || ui.menu || ui.exit_open || ui.sheet_open || ui.map_open || ui.picker_open
        || ui.scene_capture || ui.sample_held || ui.seq_held || ui.move_open || ui.settings_look || ui.loop_sheet || ui.loop_held
        || ui.levels_held || ui.levels_sheet || ui.pad_bank || ui.erase || ui.repeat_held || ui.copying != UiState::Copying::Off
        || ui.name.open() || ui.loading || ui.lesson;
}

inline bool controls_seq_hub(PageId page) {
    return page == PageId::Scenes || page == PageId::Patterns || page == PageId::Roll || page == PageId::Motion || page == PageId::ModPage;
}
inline bool controls_mix_hub(PageId page) {
    return page == PageId::Rack || page == PageId::Bus || page == PageId::Master
        || page == PageId::PadFx;
}

// The page and its state, from what the preview drew over (the labels come from ui::ControlsSeen).
inline ControlsRow controls_row(const UiState& ui) {
    ControlsRow row;
    row.page = ui.page;
    row.overlay = controls_overlay(ui);
    row.loop_chain = ui.page == PageId::Rack && (ui.fx_loop >= 0 || ui.fx_sub >= 0);
    return row;
}

// What the row breaks, a line each ("back: BACK on B3"); none when it keeps every rule.
inline std::vector<std::string> controls_breaks(const ControlsRow& row) {
    std::vector<std::string> out;
    auto is = [](const std::string& label, std::initializer_list<const char*> any) {
        for (const char* a : any) if (label == a) return true;
        return false;
    };
    for (unsigned k = 1; k < 3; ++k)
        if (row.keys[k] == "BACK" || row.back[k]) out.push_back("back: BACK on B" + std::to_string(k + 1));
    int send = -1, bus = -1, ret = -1;
    for (int k = 2; k >= 0; --k) {
        const auto& l = row.knobs[size_t(k)];
        if (l == "SEND" || l.rfind("SEND ", 0) == 0 || l.rfind("KIT SEND", 0) == 0) send = k; // not SENDING (SysEx)
        if (l == "BUS") bus = k;
        if (l == "RETURN") ret = k;
    }
    if (row.page != PageId::Motion && ((send >= 0 && bus >= 0 && send > bus) || (bus >= 0 && ret >= 0 && bus > ret) || (send >= 0 && ret >= 0 && send > ret)))
        out.push_back("sends: " + row.knobs[0] + " / " + row.knobs[1] + " / " + row.knobs[2]);
    if (!row.overlay && row.keys_drawn && controls_seq_hub(row.page)
        && !(is(row.keys[0], {"SEQUENCES", "PATTERNS"}) && row.keys[1] == "ROLL" && is(row.keys[2], {"MOTION", "MOD", "MACRO", ""})))
        out.push_back("seq: " + row.keys[0] + " / " + row.keys[1] + " / " + row.keys[2]);
    if (!row.overlay && row.keys_drawn && !row.loop_chain && controls_mix_hub(row.page)
        && !(row.keys[0] == "MIXER" && row.keys[1].empty() && row.keys[2] == "FX"))
        out.push_back("mix: " + row.keys[0] + " / " + row.keys[1] + " / " + row.keys[2]);
    return out;
}

} // namespace melodee
