// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/plugin/custom_gui.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>

namespace melodee {

CustomGui read_custom_gui(const std::filesystem::path& package) {
    namespace fs = std::filesystem;
    const auto path = package / "melodee-gui.txt";
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error))) return {};
    std::ifstream file(path, std::ios::binary);
    std::array<char, 1025> bytes {};
    file.read(bytes.data(), bytes.size());
    if (!file.eof() || file.bad() || file.gcount() > 1024) return {};
    std::string_view input(bytes.data(), size_t(file.gcount()));
    auto next = [&] {
        auto end = input.find('\n');
        auto line = input.substr(0, end);
        input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return line;
    };
    if (next() != "MELODEE-GUI 1") return {};
    auto colour = next();
    if (colour.size() != 13 || colour.substr(0, 7) != "accent ") return {};
    CustomGui gui;
    auto end = colour.data() + colour.size();
    auto parsed = std::from_chars(colour.data() + 7, end, gui.accent, 16);
    if (parsed.ec != std::errc() || parsed.ptr != end || next() != "scope stereo") return {};
    while (!input.empty()) {
        auto line = next();
        constexpr std::string_view prefix = "parameter ";
        if (line.substr(0, prefix.size()) != prefix || gui.count == gui.parameters.size()) return {};
        line.remove_prefix(prefix.size());
        auto& param = gui.parameters[gui.count];
        parsed = std::from_chars(line.data(), line.data() + line.size(), param.index);
        if (parsed.ec != std::errc() || param.index < 0 || parsed.ptr == line.data() + line.size() ||
            *parsed.ptr != ' ') return {};
        auto name = line.substr(size_t(parsed.ptr - line.data()) + 1);
        if (name.empty() || name.size() >= param.name.size() || name.front() == ' ' || name.back() == ' ' ||
            !std::all_of(name.begin(), name.end(), [](char c) { return c >= ' ' && c <= '~'; })) return {};
        for (unsigned n = 0; n < gui.count; ++n)
            if (gui.parameters[n].index == param.index) return {};
        std::copy(name.begin(), name.end(), param.name.begin());
        ++gui.count;
    }
    gui.enabled = true;
    return gui;
}

} // namespace melodee
