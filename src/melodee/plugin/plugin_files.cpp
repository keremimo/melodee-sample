// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/plugin_files.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace melodee {
namespace fs = std::filesystem;

namespace {
bool ends_with(const std::string& text, const char* end) {
    size_t n = std::strlen(end);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != end[i]) return false;
    return true;
}

std::string stem(const fs::path& path) { return path.stem().string(); }

// Contents/<build> folders of a VST3 bundle, as names a person knows.
std::string builds_named(const fs::path& bundle) {
    std::error_code error;
    bool windows = false, mac = false, x86 = false, other = false;
    for (const auto& entry : fs::directory_iterator(bundle / "Contents", error)) {
        auto name = entry.path().filename().string();
        if (name == "Resources") continue;
        if (name.find("-win") != std::string::npos) windows = true;
        else if (name == "MacOS") mac = true;
        else if (name.find("x86") != std::string::npos || name.find("i386") != std::string::npos) x86 = true;
        else other = true;
    }
    if (windows && mac) return "Windows and Mac builds only";
    if (windows) return "Windows build only";
    if (mac) return "Mac build only";
    if (x86) return "PC Linux build only";
    if (other) return "No build for this MPC";
    return "Empty bundle";
}

// moduleinfo.json (VST3 SDK 3.7.5 on): an instrument's class says so in its sub-categories.
int8_t vst3_instrument(const fs::path& bundle) {
    juce::File info(juce::String((bundle / "Contents/Resources/moduleinfo.json").string()));
    if (!info.existsAsFile()) return -1;
    auto json = juce::JSON::parse(info.loadFileAsString());
    auto* classes = json.getProperty("Classes", {}).getArray();
    if (!classes) return -1;
    for (const auto& c : *classes) {
        if (c.getProperty("Category", {}).toString() != "Audio Module Class") continue;
        auto subs = c.getProperty("Sub Categories", {});
        if (auto* list = subs.getArray()) {
            for (const auto& s : *list)
                if (s.toString().containsIgnoreCase("Instrument")) return 1;
            return 0;
        }
    }
    return -1;
}

void describe_vst3(PluginFile& f) {
    fs::path bundle(f.path);
    f.format = PluginFormat::Vst3;
    f.name = stem(bundle);
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(bundle / "Contents/aarch64-linux", error))
        if (ends_with(entry.path().string(), ".so")) { f.library = entry.path().string(); break; }
    if (f.library.empty()) f.why = builds_named(bundle);
    f.instrument = vst3_instrument(bundle);
}

// A drum machine's voices, from its library (at most 32 MB of it): only an instrument, or one not known yet.
void read_drums(PluginFile& f, const std::string& library) {
    if (f.instrument == 0) return;
    std::error_code error;
    auto size = fs::file_size(library, error);
    std::ifstream in(library, std::ios::binary);
    if (error || !in) return;
    std::string bytes;
    bytes.resize(size_t(std::min<std::uintmax_t>(size, 32u << 20)));
    in.read(bytes.data(), std::streamsize(bytes.size()));
    bytes.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    f.drums = drum_voices_in(bytes);
}

void describe_library(PluginFile& f, const std::string& library) {
    switch (arm_elf(library)) {
    case 2: f.format = PluginFormat::Vst2; f.library = library; break;
    case 1: f.format = PluginFormat::Vst2Arm32; f.library = library; break;
    default: f.format = PluginFormat::Vst2; f.why = "Not an ARM build"; break;
    }
}

// An Open MPC folder: plugin-meta.xml's <PLUGIN name=... file=".../x.so" isInstrument="1"/>.
bool describe_open_mpc(PluginFile& f) {
    fs::path folder(f.path);
    juce::File meta(juce::String((folder / "plugin-meta.xml").string()));
    if (!meta.existsAsFile()) return false;
    auto xml = juce::parseXML(meta);
    juce::XmlElement* plugin = xml ? (xml->hasTagName("PLUGIN") ? xml.get() : xml->getChildByName("PLUGIN")) : nullptr;
    f.name = plugin && plugin->hasAttribute("name") ? plugin->getStringAttribute("name").toStdString() : stem(folder);
    if (plugin && plugin->hasAttribute("isInstrument")) f.instrument = plugin->getIntAttribute("isInstrument") ? 1 : 0;
    f.fixed_rate = 44100; // the Open MPC engine contract (mpc-vst-plugins, wrapper/engine.h)
    std::string so = plugin ? juce::File::createFileWithoutCheckingPath(plugin->getStringAttribute("file")).getFileName().toStdString() : "";
    if (so.empty() || !fs::exists(folder / so)) { // the first .so beside it
        so.clear();
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder, error))
            if (ends_with(entry.path().string(), ".so")) { so = entry.path().filename().string(); break; }
    }
    if (so.empty()) { f.format = PluginFormat::Vst2; f.why = "No library in the folder"; return true; }
    describe_library(f, (folder / so).string());
    read_drums(f, (folder / so).string());
    return true;
}
} // namespace

const char* format_tag(PluginFormat format) {
    switch (format) {
    case PluginFormat::Vst3: return "VST3";
    case PluginFormat::Vst2: return "VST2";
    case PluginFormat::Vst2Arm32: return "ARMV7";
    }
    return "";
}

int arm_elf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char h[20] {};
    if (!in.read(reinterpret_cast<char*>(h), sizeof(h))) return 0;
    if (std::memcmp(h, "\x7f" "ELF", 4) != 0 || h[5] != 1) return 0; // little-endian only
    unsigned machine = h[18] | h[19] << 8;
    if (h[4] == 1 && machine == 40) return 1;  // EM_ARM
    if (h[4] == 2 && machine == 183) return 2; // EM_AARCH64
    return 0;
}

PluginFile describe_plugin(const std::string& path) {
    PluginFile f;
    f.path = path;
    std::error_code error;
    if (fs::is_directory(path, error)) {
        if (ends_with(path, ".vst3")) describe_vst3(f);
        else if (!describe_open_mpc(f)) { f.folder = true; f.name = fs::path(path).filename().string(); }
        return f;
    }
    f.name = stem(path);
    describe_library(f, path);
    read_drums(f, path);
    return f;
}

std::vector<PluginFile> list_plugins(const std::string& folder) {
    std::vector<PluginFile> out;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        bool dir = entry.is_directory(error);
        if (!dir && !ends_with(name, ".so")) continue;
        out.push_back(describe_plugin(entry.path().string()));
    }
    std::sort(out.begin(), out.end(), [](const PluginFile& a, const PluginFile& b) {
        if (a.folder != b.folder) return a.folder;
        return juce::String(a.name).compareNatural(juce::String(b.name)) < 0;
    });
    return out;
}

std::vector<int> qlink_order(const std::string& plugin_folder) {
    juce::File skin(juce::String((fs::path(plugin_folder) / "Plugin Skins/Q-Links.json").string()));
    if (!skin.existsAsFile()) return {};
    return qlink_order_in(skin.loadFileAsString().toStdString());
}

} // namespace melodee
