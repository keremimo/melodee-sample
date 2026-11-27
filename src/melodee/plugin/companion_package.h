// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A companion screen's package (sdk/melodee-screen/melodee_companion.h, docs/PLUGIN_COMPANIONS.md): the card's
// Melodee/Plugin Screens/<folder>/screen.json, read here, and whether it fits a plug-in as opened. Pure, no JUCE: the
// rack reads the folder and the plug-in (plugin/plugin_rack.cpp), the tests the rules.
#pragma once
#include "melodee/plugin/page_file.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace melodee {

constexpr long companion_abi = 1;               // MELODEE_COMPANION_ABI, the one this Melodee runs
constexpr unsigned companion_params_most = 512; // parameters a screen may list
constexpr size_t companion_manifest_most = 256 << 10;

struct CompanionParamSpec {
    int index = -1;   // the plug-in's parameter index
    std::string name; // the plug-in's name for it there
    int steps = -1;   // positions - 1 when the plug-in does not say (a VST2's), -1 not given
};

struct CompanionManifest {
    std::string name, author, version;
    std::string library = "screen-aarch64.so";
    long sdk = 0;
    // The plug-in, as a page file names one (plugin/page_file.h): CLAP id, VST2 unique ID, LV2 URI, else name and vendor.
    std::string clap, vst2, lv2, plugin_name, vendor;
    std::vector<CompanionParamSpec> params;
};

// The plug-in as opened, to check a manifest against.
struct CompanionTarget {
    PluginIdentity who;
    bool vst2 = false;               // a VST2 (the only format companions take for now)
    std::vector<std::string> names;  // every parameter's name, by index
};

enum class CompanionFit : uint8_t {
    Other, // for another plug-in
    Fits,  // for this one, every parameter where it says
    Wrong, // for this one, but it cannot be used (why says)
};

namespace companion_detail {
inline std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t from = 0;
    while (from < s.size() && (s[from] == ' ' || s[from] == '\t')) ++from;
    return s.substr(from);
}
// A file name in the package's own folder: no folders, nothing hidden.
inline bool plain_file_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || name[0] == '.') return false;
    return std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20; });
}
} // namespace companion_detail

// Reads `json` into `out`; false with `why` (a few words) when it is not a screen.json Melodee can use.
inline bool read_companion_manifest(std::string_view json, CompanionManifest& out, std::string& why) {
    using drum_map_detail::Reader;
    out = {};
    if (json.size() > companion_manifest_most) { why = "screen.json is too large"; return false; }
    Reader r {json};
    bool too_many = false;
    bool ok = r.object([&](const std::string& key) {
        if ((key == "name" || key == "author" || key == "version" || key == "library") && r.peek('"')) {
            std::string text; // Reader::string() adds to what is there: the library has a default
            if (!r.string(&text)) return false;
            (key == "name" ? out.name : key == "author" ? out.author : key == "version" ? out.version : out.library) = text;
            return true;
        }
        if (key == "sdk") return r.number(&out.sdk) || r.skip();
        if (key == "plugin" && r.peek('{'))
            return r.object([&](const std::string& k) {
                if (k == "vst2" && !r.peek('"')) { long v = 0; if (!r.number(&v)) return r.skip(); out.vst2 = std::to_string(v); return true; }
                std::string* field = k == "clap" ? &out.clap : k == "vst2" ? &out.vst2 : k == "lv2" ? &out.lv2
                                   : k == "name" ? &out.plugin_name : k == "vendor" ? &out.vendor : nullptr;
                if (field && r.peek('"')) return r.string(field);
                return r.skip();
            });
        if (key == "params" && r.peek('[')) {
            return page_file_detail::array(r, [&] {
                if (!r.peek('{')) return r.skip();
                CompanionParamSpec p;
                long v = 0;
                bool read = r.object([&](const std::string& k) {
                    if (k == "index") { if (!r.number(&v)) return false; p.index = int(std::clamp(v, -1L, 1000000L)); return true; }
                    if (k == "steps") { if (!r.number(&v)) return false; p.steps = int(std::clamp(v, 0L, 100000L)); return true; }
                    if (k == "name" && r.peek('"')) return r.string(&p.name);
                    return r.skip();
                });
                if (!read) return false;
                if (out.params.size() >= companion_params_most) { too_many = true; return true; }
                p.name = companion_detail::trimmed(p.name);
                out.params.push_back(std::move(p));
                return true;
            });
        }
        return r.skip();
    });
    if (!ok) { why = "screen.json is not JSON Melodee can read"; return false; }
    if (too_many) { why = "lists more than " + std::to_string(companion_params_most) + " parameters"; return false; }
    if (out.name.empty()) { why = "screen.json gives no name"; return false; }
    if (out.sdk <= 0) { why = "screen.json gives no sdk version"; return false; }
    if (out.sdk > companion_abi) {
        why = "needs companion SDK " + std::to_string(out.sdk) + "; this Melodee has " + std::to_string(companion_abi);
        return false;
    }
    if (!companion_detail::plain_file_name(out.library)) { why = "library is not a file in its folder"; return false; }
    if (out.clap.empty() && out.vst2.empty() && out.lv2.empty() && out.plugin_name.empty()) { why = "names no plug-in"; return false; }
    if (out.params.empty()) { why = "lists no parameters"; return false; }
    for (size_t n = 0; n < out.params.size(); ++n)
        if (out.params[n].index < 0 || out.params[n].name.empty()) {
            why = "parameter " + std::to_string(n + 1) + " needs an index and a name";
            return false;
        }
    return true;
}

// Whether `m` is for the plug-in `who`, as page_file_matches() decides for a page file.
inline bool companion_names(const CompanionManifest& m, const PluginIdentity& who) {
    PageFile as_page;
    as_page.clap = m.clap;
    as_page.vst2 = m.vst2;
    as_page.lv2 = m.lv2;
    as_page.name = m.plugin_name;
    as_page.vendor = m.vendor;
    return page_file_matches(as_page, who);
}

// Whether `m` fits `t`; `why` says what does not, for problems.txt.
inline CompanionFit companion_fit(const CompanionManifest& m, const CompanionTarget& t, std::string& why) {
    why.clear();
    if (!companion_names(m, t.who)) return CompanionFit::Other;
    if (!t.vst2) { why = "only VST2 plug-ins take companion screens in this Melodee"; return CompanionFit::Wrong; }
    for (const auto& p : m.params) {
        if (size_t(p.index) >= t.names.size()) {
            why = "no parameter " + std::to_string(p.index) + " (it has " + std::to_string(t.names.size()) + ")";
            return CompanionFit::Wrong;
        }
        const auto& has = t.names[size_t(p.index)];
        if (companion_detail::trimmed(has) != p.name) {
            why = "parameter " + std::to_string(p.index) + " is \"" + has + "\", not \"" + p.name + "\"";
            return CompanionFit::Wrong;
        }
    }
    return CompanionFit::Fits;
}

// Which package to use among those found: the one that fits; none when several fit (the user picks one later) or
// none does. `fits` holds each package's fit, by place.
struct CompanionPick {
    int package = -1;       // index into the packages, -1 none
    bool ambiguous = false; // several fit
};
inline CompanionPick pick_companion(const std::vector<CompanionFit>& fits) {
    CompanionPick out;
    for (size_t n = 0; n < fits.size(); ++n) {
        if (fits[n] != CompanionFit::Fits) continue;
        if (out.package >= 0) return {-1, true};
        out.package = int(n);
    }
    return out;
}

} // namespace melodee
