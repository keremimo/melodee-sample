// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/plugin/drum_map.h"
#include "melodee/plugin/plugin_pages.h"
#include <algorithm>
#include <cctype>
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
// - an MPC plug-in folder ("Maker - VST - Name/"): Akai's plugin.json naming
//   its VST2 builds (the MPC plug-in SDK's folder, MPC OS 3.10.1 and MPC
//   Sample 1.4 read it from VST2 Plugins), or an Open MPC folder
//   (open-mpc.github.io: plugin-meta.xml beside a VST2 .so); either has its
//   Q-Link pages in Plugin Skins/Q-Links.json;
// - a VST2 library (.so) on its own.
//
// Only builds for this MPC can load: aarch64 Linux, or a 32-bit ARM (armv7)
// VST2 through the bridge (plugin/vst2_bridge_instance.h). Others are listed
// with the reason, so a Windows or Mac download says why it does nothing.
namespace melodee {

enum class PluginFormat : uint8_t { Vst3, Vst2, Vst2Arm32, Clap, Lv2 };
const char* format_tag(PluginFormat format); // "VST3", "VST2", "ARMV7", "CLAP", "LV2"

struct PluginFile {
    std::string path;          // the bundle, the MPC plug-in folder, the .clap or the .so
    std::string name;          // as listed: the plug-in's own name, else the file's
    bool folder = false;       // a plain folder to open
    PluginFormat format = PluginFormat::Vst3;
    std::string library;       // the shared library inside (absolute), empty when there is none for this MPC
    std::string why;           // why it cannot load here ("Windows and Mac builds only"); empty when it can
    int8_t instrument = -1;    // 1 instrument, 0 effect, -1 not known until it is opened once
    // The rate it runs at whatever it is opened at, 0 none: an MPC plug-in folder, written to MPC OS's 44.1 kHz
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
// open) lists dimmed; with `zips` false (a folder outside Melodee/Plugins, where zips are not unzipped) it is
// left out. System Volume Information is left out too.
std::vector<PluginFile> list_plugins(const std::string& folder, bool zips = true);
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

// An Open MPC plug-in's Q-Link pages (plugin/plugin_pages.h), each named after
// the skin's tab it is on (Plugin Skins/TUI.json; a tab's second page gets " 2"),
// else "Q-LINKS n". Empty: none given.
std::vector<PluginPage> qlink_pages(const std::string& plugin_folder);

// One page of Plugin Skins/Q-Links.json: the tab it is on (1 up, 0 not said),
// and its parameter indices as the skin lays them out. A page with "Bank
// Direction": "Column" (every Open MPC skin, mpc-vst-plugins
// tools/shadow_skin.py qlink_for_slot()) holds its layout's slots 0-15, left
// to right and row 1 then row 2, on Q-Links 13, 9, 5, 1, 14, 10, 6, 2, 15, 11,
// 7, 3, 16, 12, 8, 4: read in that order, each three are one K1-K3 row as the
// skin shows them. Any other direction: Q-Link number order. -1 (no
// parameter) is skipped. Plain C++, no JUCE: the host tests read it too.
struct QLinkPage {
    int tab = 0;
    std::vector<int> params;
};
inline std::vector<QLinkPage> qlink_pages_in(std::string_view json) {
    static constexpr int by_slot[16] = {13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3, 16, 12, 8, 4};
    std::vector<QLinkPage> out;
    drum_map_detail::Reader r {json};
    auto page = [&] {
        bool column = false;
        long tab = 0;
        long links[16];
        std::fill(links, links + 16, -1L);
        bool ok = r.object([&](const std::string& key) {
            if (key == "Bank Direction" && r.peek('"')) {
                std::string direction;
                if (!r.string(&direction)) return false;
                column = direction == "Column";
                return true;
            }
            if (key == "Tab" && !r.peek('"') && !r.peek('{') && !r.peek('[')) return r.number(&tab) || r.skip();
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
        QLinkPage p;
        p.tab = tab > 0 && tab < 1000 ? int(tab) : 0;
        for (int n = 0; ok && n < 16; ++n) {
            long index = links[(column ? by_slot[n] : n + 1) - 1];
            if (index >= 0 && std::find(p.params.begin(), p.params.end(), int(index)) == p.params.end()) p.params.push_back(int(index));
        }
        if (ok) out.push_back(std::move(p));
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
    return ok ? out : std::vector<QLinkPage> {};
}

// Every page's parameters in one list, each once.
inline std::vector<int> qlink_order_in(std::string_view json) {
    std::vector<int> order;
    for (const auto& page : qlink_pages_in(json))
        for (int p : page.params)
            if (std::find(order.begin(), order.end(), p) == order.end()) order.push_back(p);
    return order;
}

// The tab names of an Open MPC skin's Plugin Skins/TUI.json ("pageData":
// {"tabs": [{"tabName": "OSC"}, ...]}), tab 1 first; "" for a tab with none.
inline std::vector<std::string> skin_tab_names_in(std::string_view json) {
    std::vector<std::string> names;
    drum_map_detail::Reader r {json};
    auto tabs = [&] {
        if (!r.peek('[')) return r.skip();
        if (++r.depth > 32) return false;
        r.eat('[');
        if (!r.eat(']')) {
            do {
                if (!r.peek('{')) { if (!r.skip()) return false; names.emplace_back(); continue; }
                std::string name;
                if (!r.object([&](const std::string& key) { return key == "tabName" && r.peek('"') ? r.string(&name) : r.skip(); })) return false;
                names.push_back(name);
            } while (r.eat(','));
            if (!r.eat(']')) return false;
        }
        --r.depth;
        return true;
    };
    bool ok = r.object([&](const std::string& key) {
        if (key != "pageData" || !r.peek('{')) return r.skip();
        return r.object([&](const std::string& field) { return field == "tabs" ? tabs() : r.skip(); });
    });
    return ok ? names : std::vector<std::string> {};
}

// Q-Link pages named: after their tab in `tabs` (a tab's second page " 2", third " 3"), else "Q-LINKS n".
inline std::vector<PluginPage> named_qlink_pages(const std::vector<QLinkPage>& pages, const std::vector<std::string>& tabs) {
    std::vector<PluginPage> out;
    std::vector<int> seen;
    for (size_t n = 0; n < pages.size(); ++n) {
        const auto& q = pages[n];
        std::string name;
        if (q.tab > 0 && size_t(q.tab) <= tabs.size() && !tabs[size_t(q.tab) - 1].empty()) {
            name = tabs[size_t(q.tab) - 1];
            int again = int(std::count(seen.begin(), seen.end(), q.tab));
            if (again) name += " " + std::to_string(again + 1);
            seen.push_back(q.tab);
        } else {
            name = "Q-LINKS " + std::to_string(n + 1);
        }
        out.push_back({name, q.params});
    }
    return out;
}

// A path plugin.json gives, as a path inside the plug-in's folder ("x-arm64.so", "VST/x-arm64.so"; a backslash read
// as a slash). Empty when it is empty or leads out of the folder (absolute, a drive, or "..").
inline std::string folder_path_in(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos) return {};
    for (size_t from = 0; from <= path.size();) {
        size_t end = std::min(path.find('/', from), path.size());
        if (path.compare(from, end - from, "..") == 0) return {};
        from = end + 1;
    }
    return path;
}

// Akai's plugin.json, as an MPC plug-in folder has it: the plug-in's name, whether it is an instrument
// ("isInstrument", else "kind" "instrument" or "effect"; -1 not said) and its libraries by build, each inside the
// folder (folder_path_in(); empty: not given). Empty when it is not a JSON object. Plain C++, no JUCE: the host tests
// read it too.
struct PluginJson {
    std::string name;
    int8_t instrument = -1;
    std::string file64; // the aarch64 build
    std::string file32; // the armv7 build
    std::string file;   // a build not said which
};
inline PluginJson plugin_json_in(std::string_view json) {
    PluginJson out;
    std::string kind;
    int8_t flag = -1;
    drum_map_detail::Reader r {json};
    auto text = [&](std::string& to) { return r.peek('"') ? r.string(&to) : r.skip(); };
    bool ok = r.object([&](const std::string& key) {
        if (key == "name") return text(out.name);
        if (key == "kind") return text(kind);
        if (key == "file64") return text(out.file64);
        if (key == "file32") return text(out.file32);
        if (key == "file") return text(out.file);
        if (key != "isInstrument") return r.skip();
        long number = 0;
        if (r.word("true")) flag = 1;
        else if (r.word("false")) flag = 0;
        else if (r.number(&number)) flag = number != 0;
        else return r.skip();
        return true;
    });
    if (!ok) return {};
    for (auto& c : kind) c = char(std::tolower(static_cast<unsigned char>(c)));
    out.instrument = flag >= 0 ? flag : kind == "instrument" ? 1 : kind == "effect" ? 0 : -1;
    for (auto* file : {&out.file64, &out.file32, &out.file}) *file = folder_path_in(*file);
    return out;
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
