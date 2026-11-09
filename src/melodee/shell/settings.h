// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

// Melodee's own settings, not the project's: how the pads respond and
// light, whether pots pick up, the screen. Kept as key=value lines in the
// `settings` file next to the projects (with the project to open at start).
namespace melodee {

struct Settings {
    enum Row : uint8_t { Palette, PadResponse, PadLights, Pickup, Screen, Rows };
    uint8_t pad_response = 1; // 0 soft, 1 medium, 2 hard, 3 fixed
    uint8_t pad_lights = 0;   // 0 track colours, 1 white, 2 off
    bool pickup = true;       // off: a pot's value jumps to where it sits
    uint8_t screen = 100;     // brightness, 20..100 %

    static const char* row_name(unsigned row) {
        static const char* names[] {"Palette", "Pad response", "Pad lights", "Knob pickup", "Screen"};
        return names[row % Rows];
    }
    std::string value_text(unsigned row) const {
        static const char* responses[] {"Soft", "Medium", "Hard", "Fixed"};
        static const char* lights[] {"Track colours", "White", "Off"};
        switch (row) {
        case Palette: return "NIGHT";
        case PadResponse: return responses[pad_response % 4];
        case PadLights: return lights[pad_lights % 3];
        case Pickup: return pickup ? "On" : "Off";
        default: return std::to_string(screen) + " %";
        }
    }
    // One step along a row (the palette has one value).
    void step(unsigned row, int way) {
        switch (row) {
        case PadResponse: pad_response = uint8_t((pad_response + 4 + way) % 4); break;
        case PadLights: pad_lights = uint8_t((pad_lights + 3 + way) % 3); break;
        case Pickup: pickup = !pickup; break;
        case Screen: screen = uint8_t(std::clamp(int(screen) + way * 10, 20, 100)); break;
        default: break;
        }
    }
    // A pad's velocity through the response curve.
    uint8_t velocity(uint8_t v) const {
        if (pad_response == 3) return 100;
        if (pad_response == 1 || v == 0) return v;
        float x = v / 127.0f, curved = std::pow(x, pad_response == 0 ? 0.6f : 1.7f);
        return uint8_t(std::clamp(int(curved * 127 + 0.5f), 1, 127));
    }

    std::string text() const {
        std::ostringstream out;
        out << "pad_response=" << int(pad_response) << "\npad_lights=" << int(pad_lights) << "\npickup=" << int(pickup)
            << "\nscreen=" << int(screen) << '\n';
        return out.str();
    }
    void read(const std::string& text) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            int value = std::atoi(line.c_str() + eq + 1);
            if (key == "pad_response") pad_response = uint8_t(std::clamp(value, 0, 3));
            else if (key == "pad_lights") pad_lights = uint8_t(std::clamp(value, 0, 2));
            else if (key == "pickup") pickup = value != 0;
            else if (key == "screen") screen = uint8_t(std::clamp(value, 20, 100));
        }
    }
};

} // namespace melodee
