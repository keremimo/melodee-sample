// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/pages/common.h"
#include "melodee/ui/widgets.h"

namespace melodee {
using namespace ui;

bool draw_custom_plugin_gui(Paint& p, const PluginPanel& panel, float x, float y, float w, float h, bool bypass) {
    if (panel.status != PluginStatus::Ready || !panel.gui_enabled || h < 56 || w < 180) return false;
    const auto accent = juce::Colour(0xff000000u | panel.gui_accent);
    const auto ink = bypass ? colour::dim : accent;
    const auto count = std::count_if(panel.gui_params.begin(), panel.gui_params.end(),
                                    [](const auto& param) { return param.used; });
    const float scope_h = h - (count ? 26 : 0);
    p.fill(x, y, w, scope_h, colour::panel, 4);
    p.ring(x, y, w, scope_h, ink.withAlpha(.4f), 4);

    const float lane_h = (scope_h - 6) / 2;
    const float dx = (w - 28) / panel.gui_trace.size();
    for (unsigned ch = 0; ch < 2; ++ch) {
        const float mid = y + 3 + (ch + .5f) * lane_h;
        p.text(ch ? "R" : "L", x + 6, mid - 6, 12, 12, caps_text(colour::mid));
        p.fill(x + 22, mid, w - 28, 1, ink.withAlpha(.3f));
        if (bypass) continue;
        const float gain = (lane_h - 2) / (2 * 127);
        for (unsigned n = 0; n < panel.gui_trace.size(); ++n) {
            const uint32_t pair = panel.gui_trace[n] >> (16 * ch);
            const int top = (pair >> 8) & 255;
            const int low = pair & 255;
            const int bottom = low >= 128 ? 256 - low : 0;
            if (top || bottom)
                p.fill(x + 22 + dx * n, mid - gain * top, std::max(1.0f, dx - 1),
                       std::max(1.0f, gain * (top + bottom)), ink);
        }
    }

    if (!count) return true;
    const float tile_w = (w - 4 * (count - 1)) / count;
    const float tile_y = y + scope_h + 4;
    unsigned tile = 0;
    for (const auto& param : panel.gui_params) {
        if (!param.used) continue;
        const float left = x + tile++ * (tile_w + 4);
        const auto name = juce::String::fromUTF8(param.name);
        const auto value = juce::String::fromUTF8(param.text);
        p.fill(left, tile_y, tile_w, 22, colour::panel, 3);
        auto style = plain(colour::text, type::small);
        const float value_w = std::min(p.width(value, style) + 8, tile_w * .48f);
        const float name_w = tile_w - value_w - 8;
        p.text(name, left + 4, tile_y + 1, name_w, 16, p.fit(name, style, name_w, type::floor));
        p.text(value, left + tile_w - value_w - 4, tile_y + 1, value_w, 16,
               p.fit(value, style, value_w, type::floor), juce::Justification::centredRight);
        const float bar = (tile_w - 8) * param.value / plugin_param_max;
        if (bar > 0) p.fill(left + 4, tile_y + 18, bar, 2, ink);
    }
    return true;
}

} // namespace melodee
