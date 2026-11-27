// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
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
    int16_t initial = 0; // its default, 0..1000 (Erase + pot)
    uint16_t steps = 0;  // the values it takes when fewer than plugin_param_max (Shift + wheel steps by one), else 0
};

struct PluginPanel {
    uint8_t entry = no_plugin;     // Project::plugins
    PluginStatus status = PluginStatus::None;
    bool instrument = false;
    bool qlinks = false;           // the rows follow an Open MPC skin's Q-Link pages
    // Its own screen (plugin/plugin_screen.h): `has_screen` it offers one; `screen` the page in view is that screen,
    // drawn at least once, and `params` hold the three it puts on K1-K3 (bound as plugin_screen_slot + k).
    bool has_screen = false;
    bool screen = false;
    uint8_t screen_keys = 0;       // the printed buttons that screen's page takes (screen_key_mute, screen_key_erase)
    char name[32] {};
    char format[8] {};             // VST3, VST2, ARMV7
    char error[64] {};             // why it failed
    uint16_t row = 0, rows = 0;    // rows of three parameters, every page's in turn (plugin/plugin_pages.h)
    uint16_t page = 0, pages = 0;  // the page `row` is on, and how many there are
    uint16_t page_row = 0, page_rows = 0; // `row` within its page, and the page's rows
    uint16_t prev_page = 0, next_page = 0; // the first rows of the pages before and after (Minus / Plus); `row`'s own page's at the ends
    char page_name[32] {};         // its name ("" the plug-in's one list)
    // The page's picture (plugin/plugin_pages.h PagePictureSlots): its parts' parameters in the kind's order
    // (envelope: attack, decay, sustain, release; filter: cutoff, resonance; values: up to three; EQ: bands of
    // frequency, gain, Q), each with where it is in the rows (`picture_slots`, row * 3 + k; -1 not on the page).
    uint8_t picture = 0;           // PagePicture
    std::array<PluginPanelParam, 12> picture_params {};
    std::array<int16_t, 12> picture_slots {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // The skin page for this page (plugin/plugin_skin.h; the skin itself is the snapshot's `plugin_skin`, shell/
    // snapshot.h): `skin_page` an index into its pages, -2 its strip (an effect in a chain), -1 none. The parameters
    // its controls show, in the order of those controls that have one, each with where it is in the rows
    // (`skin_slots`, -1 not on this page).
    int16_t skin_page = -1;
    std::array<PluginPanelParam, 24> skin_params {};
    std::array<int16_t, 24> skin_slots {};
    // An effect's sound as it plays, measured here for the plug-in in view (pages/fx_drawing.cpp): one block's lowest
    // and highest sample per entry, oldest first (low byte left, then right; each a signed byte as min, max), and the
    // levels in and out, 0..1000. `measured`: there is something to show.
    bool measured = false;
    std::array<uint32_t, 64> scope {};
    uint16_t level_in = 0, level_out = 0;
    uint16_t load = 0;             // the share of a block it takes, in tenths of a percent
    uint16_t programs = 0;         // its named programs (presets), for PRESETS
    std::array<PluginPanelParam, 3> params {};
    // Two rows of the row in hand's page, its own among them (the row after, or on the page's last row the one
    // before; one row alone on a page of one), for the page's arcs and list; `window_first` the first one's slot
    // (row * 3), `window_rows` how many rows it holds.
    std::array<PluginPanelParam, 6> window {};
    uint16_t window_first = 0;
    uint8_t window_rows = 0;
};

// The panel lives in UiState, which the shell copies and compares byte for byte (shell/shell.cpp usage marks): plain
// values only, no owning pointers.
static_assert(std::is_trivially_copyable_v<PluginPanel>, "PluginPanel must stay plain values");

// A plug-in's programs as PRESETS lists them: the named ones, each with its
// number in the plug-in, and the one picked last (-1: none picked here yet).
// A CLAP plug-in's presets are files, so each also has the folder it is in
// ("Leads", "Third Party Presets/Kinsey Dulcet/Percussion"; "" none).
struct PluginProgram {
    int index = 0;
    std::string name;
    std::string folder;
};
// The list shared rather than copied: a plug-in may have thousands of presets
// (Surge XT), and the pages take it with every frame. Never changed once made.
class PluginProgramList {
public:
    PluginProgramList() = default;
    explicit PluginProgramList(std::vector<PluginProgram> items)
        : items_(std::make_shared<const std::vector<PluginProgram>>(std::move(items))) {}
    size_t size() const { return items_ ? items_->size() : 0; }
    bool empty() const { return size() == 0; }
    const PluginProgram& operator[](size_t n) const { return (*items_)[n]; }
    const PluginProgram* begin() const { return items_ ? items_->data() : nullptr; }
    const PluginProgram* end() const { return items_ ? items_->data() + items_->size() : nullptr; }
private:
    std::shared_ptr<const std::vector<PluginProgram>> items_;
};
// A program's key as PluginRef::preset keeps it: "folder/name", or the name alone.
inline std::string program_key(const PluginProgram& p) { return p.folder.empty() ? p.name : p.folder + "/" + p.name; }
// The program with that key (its number in the plug-in), -1 none.
inline int program_keyed(const PluginProgramList& list, const std::string& key) {
    if (key.empty()) return -1;
    for (const auto& p : list) if (program_key(p) == key) return p.index;
    return -1;
}
struct PluginPrograms {
    uint8_t entry = no_plugin;
    PluginProgramList list;
    int current = -1;
    bool loading = false;          // a pick still on its way to the plug-in
    bool finding = false;          // its presets still being looked for (a CLAP plug-in's, on the card)
};

// A drum plug-in's voices as the selected track's Drums pads reach them (plugin/drum_map.h): the name on each of
// keys 36-99 (pads A01-D16), "" where it has none. The shell keeps the selected track's in the UI state for the Keys
// page and the pad lights; `entry` no_plugin: none.
struct DrumVoices {
    static constexpr unsigned first = 36, keys = 64;
    uint8_t entry = no_plugin;
    std::array<std::array<char, 16>, keys> names {};
    bool any() const { for (const auto& n : names) if (n[0]) return true; return false; }
    const char* at(int key) const { // nullptr: no voice there
        unsigned k = unsigned(key - int(first));
        return key >= int(first) && k < keys && names[k][0] ? names[k].data() : nullptr;
    }
};

constexpr int plugin_param_max = 1000; // a card's range for a plug-in parameter
// A plug-in card's binding id: the entry and the parameter's place in the plug-in's list of shown ones.
constexpr int32_t plugin_binding(unsigned entry, unsigned shown) { return int32_t(entry << 16 | shown); }
constexpr unsigned plugin_binding_entry(int32_t id) { return unsigned(id) >> 16; }
constexpr unsigned plugin_binding_shown(int32_t id) { return unsigned(id) & 0xffff; }
// A plug-in's own screen names its three (PluginPanel::screen): K`k` is bound as plugin_screen_slot + k.
constexpr unsigned plugin_screen_slot = 0x8000;
// A companion screen's own edit (plugin/companion_screen.h): the parameter by its index in the plug-in, as
// plugin_index_slot + index (below plugin_screen_slot: indices up to 16383).
constexpr unsigned plugin_index_slot = 0x4000;
constexpr bool plugin_binding_indexed(int32_t id) {
    return plugin_binding_shown(id) >= plugin_index_slot && plugin_binding_shown(id) < plugin_screen_slot;
}
// Which of K1-K3 a binding id is on.
constexpr unsigned plugin_binding_k(int32_t id) {
    return plugin_binding_shown(id) >= plugin_screen_slot ? (plugin_binding_shown(id) - plugin_screen_slot) % 3 : plugin_binding_shown(id) % 3;
}
// The panel's card for a binding id, null when that parameter is not on K1-K3 now.
inline const PluginPanelParam* plugin_card(const PluginPanel& panel, int32_t id) {
    unsigned shown = plugin_binding_shown(id);
    if (panel.entry != plugin_binding_entry(id) || plugin_binding_indexed(id)) return nullptr;
    if (shown >= plugin_screen_slot) {
        if (!panel.screen || shown - plugin_screen_slot >= 3) return nullptr;
        const auto& card = panel.params[shown - plugin_screen_slot];
        return card.used ? &card : nullptr;
    }
    if (panel.screen || shown < panel.row * 3u || shown >= panel.row * 3u + 3) return nullptr;
    const auto& card = panel.params[shown - panel.row * 3u];
    return card.used ? &card : nullptr;
}
// Shift + wheel on a plug-in card: `delta` of the parameter's own values when it takes fewer than plugin_param_max
// (a detent is never lost to the plug-in rounding it back), else `delta` thousandths of its range.
inline int plugin_param_step(const PluginPanelParam& card, int value, int delta) {
    if (card.steps < 2) return std::clamp(value + delta, 0, plugin_param_max);
    int last = card.steps - 1;
    int at = std::clamp(int(std::lround(value * double(last) / plugin_param_max)) + delta, 0, last);
    return int(std::lround(at * double(plugin_param_max) / last));
}

} // namespace melodee
