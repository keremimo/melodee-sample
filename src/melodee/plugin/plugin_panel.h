// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// What the page in view shows of one plug-in: its state, the row of three
// parameters on K1-K3, and its programs for PRESETS. The shell refreshes it from the rack (plugin/plugin_rack.h)
// on the MIDI thread and keeps it in the UI state, so the cards and the pots'
// pickup work from it like any other page's values. No JUCE here.
namespace melodee {

enum class PluginStatus : uint8_t { None, Loading, Ready, Failed, Crashed };

struct PluginPanelParam {
    bool used = false;
    char name[48] {};   // as the plug-in names it (the card writes it in capitals where they fit, else over two lines)
    char text[20] {};   // its value as the plug-in shows it
    int16_t value = 0;  // 0..1000 of its range
};

struct PluginPanel {
    uint8_t entry = no_plugin;     // Project::plugins
    PluginStatus status = PluginStatus::None;
    bool instrument = false;
    bool qlinks = false;           // the rows follow an Open MPC skin's Q-Link pages
    char name[32] {};
    char format[8] {};             // VST3, VST2, ARMV7
    char error[64] {};             // why it failed
    uint16_t row = 0, rows = 0;    // rows of three parameters
    uint16_t load = 0;             // the share of a block it takes, in tenths of a percent
    uint16_t programs = 0;         // its named programs (presets), for PRESETS
    std::array<PluginPanelParam, 3> params {};
    // The six parameters around the row in hand, its own three among them (the row before, or at the top the row
    // after), for the page's list; `window_first` the first one's place in the list of shown ones.
    std::array<PluginPanelParam, 6> window {};
    uint16_t window_first = 0;
};

// A plug-in's programs as PRESETS lists them: the named ones, each with its
// number in the plug-in, and the one picked last (-1: none picked here yet).
struct PluginProgram {
    int index = 0;
    std::string name;
};
struct PluginPrograms {
    uint8_t entry = no_plugin;
    std::vector<PluginProgram> list;
    int current = -1;
    bool loading = false;          // a pick still on its way to the plug-in
};

constexpr int plugin_param_max = 1000; // a card's range for a plug-in parameter
// A plug-in card's binding id: the entry and the parameter's place in the plug-in's list of shown ones.
constexpr int32_t plugin_binding(unsigned entry, unsigned shown) { return int32_t(entry << 16 | shown); }
constexpr unsigned plugin_binding_entry(int32_t id) { return unsigned(id) >> 16; }
constexpr unsigned plugin_binding_shown(int32_t id) { return unsigned(id) & 0xffff; }

} // namespace melodee
