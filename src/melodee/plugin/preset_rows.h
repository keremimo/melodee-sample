// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// PRESETS' folders (pages/plugin_presets_page.cpp): a plug-in's presets as the
// rows of the folder in view, its folders first and then its presets, from the
// folder each preset is in (PluginProgram::folder, "/" between levels). A
// plug-in whose presets have no folders lists them all at the top, as before.
// Pure, for tests/melodee.cpp.
#pragma once
#include "melodee/plugin/plugin_panel.h"

#include <string>
#include <string_view>
#include <vector>

namespace melodee {

struct PresetRow {
    bool folder = false;
    std::string name;      // the folder's own name (its last level), or the preset's
    size_t at = 0;         // a preset: its place in the list; a folder: the first preset's inside it
    unsigned count = 0;    // a folder: the presets inside it, at any depth
    bool current = false;  // the preset picked last, or the folder it is inside
};

// The folder above `folder` ("" the top).
inline std::string preset_folder_up(std::string_view folder) {
    auto slash = folder.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(folder.substr(0, slash));
}

// `folder`'s last level: its name as the folder above lists it.
inline std::string preset_folder_name(std::string_view folder) {
    auto slash = folder.rfind('/');
    return std::string(slash == std::string_view::npos ? folder : folder.substr(slash + 1));
}

// The rows of `inside` ("" the top): the folders in it, in the order the list first reaches them, then its presets
// in the list's order. `current`: the program picked last (PluginPrograms::current).
inline std::vector<PresetRow> preset_rows(const PluginProgramList& list, std::string_view inside, int current) {
    std::vector<PresetRow> folders, presets;
    for (size_t n = 0; n < list.size(); ++n) {
        const auto& p = list[n];
        std::string_view f = p.folder;
        if (f == inside) {
            presets.push_back({false, p.name, n, 0, p.index == current});
            continue;
        }
        if (!inside.empty()) {
            if (f.size() <= inside.size() || f.compare(0, inside.size(), inside) != 0 || f[inside.size()] != '/') continue;
            f.remove_prefix(inside.size() + 1);
        }
        auto name = f.substr(0, f.find('/'));
        PresetRow* row = !folders.empty() && folders.back().name == name ? &folders.back() : nullptr; // the list keeps a folder together
        for (size_t k = 0; !row && k < folders.size(); ++k)
            if (folders[k].name == name) row = &folders[k];
        if (!row) {
            folders.push_back({true, std::string(name), n, 0, false});
            row = &folders.back();
        }
        ++row->count;
        if (p.index == current) row->current = true;
    }
    folders.insert(folders.end(), presets.begin(), presets.end());
    return folders;
}

// Where PRESETS opens on the program picked last: its folder and its row there. Not found (or none picked yet):
// the top, row 0.
struct PresetPlace {
    std::string folder;
    unsigned row = 0;
};
inline PresetPlace preset_place(const PluginProgramList& list, int current) {
    PresetPlace out;
    for (const auto& p : list) {
        if (p.index != current) continue;
        out.folder = p.folder;
        auto rows = preset_rows(list, out.folder, current);
        for (size_t r = 0; r < rows.size(); ++r)
            if (!rows[r].folder && rows[r].current) out.row = unsigned(r);
        return out;
    }
    return out;
}

// After going up out of `from`: the row of the folder it came out of in the one above.
inline unsigned preset_row_of_folder(const PluginProgramList& list, std::string_view from, int current) {
    auto above = preset_folder_up(from);
    auto name = preset_folder_name(from);
    auto rows = preset_rows(list, above, current);
    for (size_t r = 0; r < rows.size(); ++r)
        if (rows[r].folder && rows[r].name == name) return unsigned(r);
    return 0;
}

} // namespace melodee
