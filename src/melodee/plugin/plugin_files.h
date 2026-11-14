// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/plugin/drum_map.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What a folder of plug-ins holds, as the PLUG-INS picker lists it: folders
// first, then the plug-ins in it, each by name. Five kinds are plug-ins:
//
// - a VST3 bundle (Name.vst3/, its library in Contents/aarch64-linux);
// - an LV2 bundle (Name.lv2/, its manifest.ttl beside its library);
// - a CLAP library (Name.clap, a shared library on Linux);
// - an Open MPC plug-in folder (open-mpc.github.io: plugin-meta.xml beside a
//   VST2 .so, its Q-Link pages in Plugin Skins/Q-Links.json);
// - a VST2 library (.so) on its own.
//
// Only builds for this MPC can load: aarch64 Linux, or a 32-bit ARM (armv7)
// VST2 through the bridge (plugin/vst2_bridge_instance.h). Others are listed
// with the reason, so a Windows or Mac download says why it does nothing.
namespace melodee {

enum class PluginFormat : uint8_t { Vst3, Vst2, Vst2Arm32, Clap, Lv2 };
const char* format_tag(PluginFormat format); // "VST3", "VST2", "ARMV7", "CLAP", "LV2"

struct PluginFile {
    std::string path;          // the bundle, the Open MPC folder, the .clap or the .so
    std::string name;          // as listed: the plug-in's own name, else the file's
    bool folder = false;       // a plain folder to open
    PluginFormat format = PluginFormat::Vst3;
    std::string library;       // the shared library inside (absolute), empty when there is none for this MPC
    std::string why;           // why it cannot load here ("Windows and Mac builds only"); empty when it can
    int8_t instrument = -1;    // 1 instrument, 0 effect, -1 not known until it is opened once
    // The rate it runs at whatever it is opened at, 0 none: an Open MPC plug-in, written to MPC OS's 44.1 kHz
    // (plugin/rate_bridge.h converts it to the device's)
    unsigned fixed_rate = 0;
    // A bundle holding several plug-ins (LSP's VST3 or LV2, a CLAP collection) lists as one row with how many;
    // opened, each is a row of its own with its `id` (JUCE's unique id for a VST3 or LV2, clap_id_hash() of a CLAP's
    // id). 0 plugins: not known yet.
    unsigned plugins = 0;
    int32_t id = 0;
    // A drum machine's voices (plugin/drum_map.h), read from its library; empty for any other plug-in. A PLUGIN track
    // given one plays its pads as Drums.
    std::vector<DrumVoice> drums;
    bool loadable() const { return !folder && why.empty(); }
    bool several() const { return plugins > 1 && id == 0; }
};

// The folder's entries; an unreadable folder lists nothing. A .zip left there (one unzip_plugins() could not
// open) lists dimmed.
std::vector<PluginFile> list_plugins(const std::string& folder);
// Release downloads copied in as they came: each .zip in `folder` unzipped beside it (into a folder named as
// the zip when its entries do not share one top folder; Mac `__MACOSX` and `._` files left out), and the zip
// deleted once every entry is out. One that will not open or write stays. Run on the rack's thread before a
// listing, and on the web page's thread after an upload (web/web_app.h): one call at a time. Returns the
// names of the folders it put in place.
std::vector<std::string> unzip_plugins(const std::string& folder);
// One path as list_plugins() would describe it (a project names its plug-ins by path).
PluginFile describe_plugin(const std::string& path);

// The card's plug-in folder: Melodee/Plugins under the card's top. Empty: no card.
inline std::string plugin_folder(const std::string& card_top) {
    if (card_top.empty()) return {};
    return (std::filesystem::path(card_top) / "Melodee" / "Plugins").string();
}

// Where a project's plug-in path opens on the card in now. A relative path is from the card's top. A path in
// Plugins/ at the card's top (where older builds read plug-ins) opens from the same place in Melodee/Plugins when
// it is there or when neither is, else where it was. Plain C++, no JUCE: the host tests read it too.
inline std::string card_plugin_path(const std::string& card_top, const std::string& saved) {
    namespace fs = std::filesystem;
    if (card_top.empty() || saved.empty()) return saved;
    fs::path path(saved);
    if (path.is_relative()) path = fs::path(card_top) / path;
    fs::path inside = path.lexically_relative(fs::path(card_top) / "Plugins");
    if (inside.empty() || inside == "." || *inside.begin() == "..") return path.string();
    fs::path moved = fs::path(plugin_folder(card_top)) / inside;
    std::error_code e;
    return fs::exists(moved, e) || !fs::exists(path, e) ? moved.string() : path.string();
}

// An Open MPC plug-in's Q-Link pages: its parameter indices in the order the
// skin lays them out, tab by tab, each once. Empty: none given.
std::vector<int> qlink_order(const std::string& plugin_folder);

// The same from Plugin Skins/Q-Links.json's text. A page with "Bank Direction":
// "Column" (every Open MPC skin, mpc-vst-plugins tools/shadow_skin.py
// qlink_for_slot()) holds its layout's slots 0-15, left to right and row 1
// then row 2, on Q-Links 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3, 16, 12, 8, 4:
// read in that order, each three are one K1-K3 row as the skin shows them.
// Any other direction: Q-Link number order. -1 (no parameter) is skipped.
// Plain C++, no JUCE: the host tests read it too.
inline std::vector<int> qlink_order_in(std::string_view json) {
    static constexpr int by_slot[16] = {13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3, 16, 12, 8, 4};
    std::vector<int> order;
    drum_map_detail::Reader r {json};
    auto page = [&] {
        bool column = false;
        long links[16];
        std::fill(links, links + 16, -1L);
        bool ok = r.object([&](const std::string& key) {
            if (key == "Bank Direction" && r.peek('"')) {
                std::string direction;
                if (!r.string(&direction)) return false;
                column = direction == "Column";
                return true;
            }
            if (key != "Q-Links" || !r.peek('{')) return r.skip();
            return r.object([&](const std::string& name) {
                constexpr std::string_view prefix = "Q-Link ";
                int q = 0;
                if (name.size() <= prefix.size() || name.size() > prefix.size() + 2 || name.compare(0, prefix.size(), prefix) != 0) return r.skip();
                for (size_t i = prefix.size(); i < name.size(); ++i) {
                    if (name[i] < '0' || name[i] > '9') return r.skip();
                    q = q * 10 + (name[i] - '0');
                }
                long value = -1;
                if (q < 1 || q > 16 || !r.number(&value)) return r.skip();
                links[q - 1] = value;
                return true;
            });
        });
        for (int n = 0; ok && n < 16; ++n) {
            long index = links[(column ? by_slot[n] : n + 1) - 1];
            if (index >= 0 && std::find(order.begin(), order.end(), int(index)) == order.end()) order.push_back(int(index));
        }
        return ok;
    };
    auto pages = [&] {
        if (!r.peek('[')) return r.skip();
        if (++r.depth > 32) return false;
        r.eat('[');
        if (!r.eat(']')) {
            do { if (!(r.peek('{') ? page() : r.skip())) return false; } while (r.eat(','));
            if (!r.eat(']')) return false;
        }
        --r.depth;
        return true;
    };
    bool ok = r.object([&](const std::string& key) {
        if (key != "Screen Mode Q-Links" || !r.peek('{')) return r.skip();
        return r.object([&](const std::string& field) { return field == "map" ? pages() : r.skip(); });
    });
    return ok ? order : std::vector<int> {};
}

// 1: a 32-bit ARM ELF, 2: a 64-bit ARM (aarch64) ELF, 0: anything else.
int arm_elf(const std::string& path);

// A CLAP plug-in's id ("com.example.synth") as a project keeps it: FNV-1a, never 0 (0 is "the first in the file").
inline int32_t clap_id_hash(std::string_view id) {
    uint32_t h = 2166136261u;
    for (unsigned char c : id) { h ^= c; h *= 16777619u; }
    return h == 0 ? 1 : int32_t(h);
}

} // namespace melodee
