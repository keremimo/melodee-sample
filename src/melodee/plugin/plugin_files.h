// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What a folder of plug-ins holds, as the PLUG-INS picker lists it: folders
// first, then the plug-ins in it, each by name. Three kinds are plug-ins:
//
// - a VST3 bundle (Name.vst3/, its library in Contents/aarch64-linux);
// - an Open MPC plug-in folder (open-mpc.github.io: plugin-meta.xml beside a
//   VST2 .so, its Q-Link pages in Plugin Skins/Q-Links.json);
// - a VST2 library (.so) on its own.
//
// Only builds for this MPC can load: aarch64 Linux, or 32-bit ARM (armv7)
// through the bridge (plugin/vst2_bridge_instance.h). Others are listed with
// the reason, so a Windows or Mac download says why it does nothing.
namespace melodee {

enum class PluginFormat : uint8_t { Vst3, Vst2, Vst2Arm32 };
const char* format_tag(PluginFormat format); // "VST3", "VST2", "ARMV7"

struct PluginFile {
    std::string path;          // the bundle, the Open MPC folder or the .so
    std::string name;          // as listed: the plug-in's own name, else the file's
    bool folder = false;       // a plain folder to open
    PluginFormat format = PluginFormat::Vst3;
    std::string library;       // the shared library inside (absolute), empty when there is none for this MPC
    std::string why;           // why it cannot load here ("Windows and Mac builds only"); empty when it can
    int8_t instrument = -1;    // 1 instrument, 0 effect, -1 not known until it is opened once
    // The rate it runs at whatever it is opened at, 0 none: an Open MPC plug-in, written to MPC OS's 44.1 kHz
    // (plugin/rate_bridge.h converts it to the device's)
    unsigned fixed_rate = 0;
    // A bundle holding several plug-ins (LSP's VST3) lists as one row with how many; opened, each is a row of its
    // own with its `id` (JUCE's unique id for it). 0 plugins: not known yet.
    unsigned plugins = 0;
    int32_t id = 0;
    bool loadable() const { return !folder && why.empty(); }
    bool several() const { return plugins > 1 && id == 0; }
};

// The folder's entries; an unreadable folder lists nothing.
std::vector<PluginFile> list_plugins(const std::string& folder);
// One path as list_plugins() would describe it (a project names its plug-ins by path).
PluginFile describe_plugin(const std::string& path);

// An Open MPC plug-in's Q-Link pages: its parameter indices in the order the
// skin puts them on Q-Links 1-16, tab by tab, each once. Empty: none given.
std::vector<int> qlink_order(const std::string& plugin_folder);

// 1: a 32-bit ARM ELF, 2: a 64-bit ARM (aarch64) ELF, 0: anything else.
int arm_elf(const std::string& path);

} // namespace melodee
