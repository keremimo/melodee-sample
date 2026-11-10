// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

// Melodee's own settings, not the project's: how the pads respond and
// light, whether pots pick up, the screen, and the MPC's recording toggles
// (Shift + Pad 4 Count-In, Shift + Pad 10 Rec Quantize). Kept as key=value lines in the
// `settings` file next to the projects (with the project to open at start).
namespace melodee {

struct Settings {
    enum Row : uint8_t { Palette, PadResponse, PadLights, Pickup, Screen, Rows };
    uint8_t pad_response = 1; // 0 soft, 1 medium, 2 hard, 3 full level (127, Shift + Pad 1)
    uint8_t response_before = 1; // what Shift + Pad 1 goes back to
    bool count_in = false;    // a bar before Seq Record records (phase 2)
    bool rec_quantize = true; // recorded notes onto the grid (off: phase 2)
    bool full_level() const { return pad_response == 3; }
    // Shift + Pad 1: full level on, or back to the curve it came from.
    void toggle_full_level() {
        if (full_level()) pad_response = response_before == 3 ? 1 : response_before;
        else { response_before = pad_response; pad_response = 3; }
    }
    uint8_t pad_lights = 0;   // 0 track colours, 1 white, 2 off
    bool pickup = true;       // off: a pot's value jumps to where it sits
    uint8_t screen = 100;     // brightness, 20..100 %
    // Starred factory sounds, by engine (EngineKind's order), for the Sounds page's FAVOURITES.
    static constexpr unsigned engines = 7, sounds = 256;
    std::array<std::bitset<sounds>, engines> favourites {};
    bool favourite(unsigned engine, unsigned sound) const { return engine < engines && sound < sounds && favourites[engine][sound]; }
    void toggle_favourite(unsigned engine, unsigned sound) { if (engine < engines && sound < sounds) favourites[engine].flip(sound); }

    static const char* row_name(unsigned row) {
        static const char* names[] {"Palette", "Pad response", "Pad lights", "Knob pickup", "Screen"};
        return names[row % Rows];
    }
    std::string value_text(unsigned row) const {
        static const char* responses[] {"Soft", "Medium", "Hard", "Full level"};
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
        if (pad_response == 3) return 127; // Full Level, as on the MPC
        if (pad_response == 1 || v == 0) return v;
        float x = v / 127.0f, curved = std::pow(x, pad_response == 0 ? 0.6f : 1.7f);
        return uint8_t(std::clamp(int(curved * 127 + 0.5f), 1, 127));
    }

    std::string text() const {
        std::ostringstream out;
        out << "pad_response=" << int(pad_response) << "\npad_lights=" << int(pad_lights) << "\npickup=" << int(pickup)
            << "\nscreen=" << int(screen) << "\ncount_in=" << int(count_in) << "\nrec_quantize=" << int(rec_quantize)
            << "\nresponse_before=" << int(response_before) << '\n';
        for (unsigned e = 0; e < engines; ++e) {
            if (favourites[e].none()) continue;
            out << "favourites" << e << '=';
            bool first = true;
            for (unsigned s = 0; s < sounds; ++s)
                if (favourites[e][s]) { out << (first ? "" : ",") << s; first = false; }
            out << '\n';
        }
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
            else if (key == "count_in") count_in = value != 0;
            else if (key == "rec_quantize") rec_quantize = value != 0;
            else if (key == "response_before") response_before = uint8_t(std::clamp(value, 0, 2));
            else if (key.rfind("favourites", 0) == 0) { // favourites3=0,4,12
                unsigned e = unsigned(std::atoi(key.c_str() + 10));
                if (e >= engines) continue;
                favourites[e].reset();
                std::istringstream list(line.substr(eq + 1));
                std::string item;
                while (std::getline(list, item, ','))
                    if (!item.empty()) { int s = std::atoi(item.c_str()); if (s >= 0 && unsigned(s) < sounds) favourites[e].set(unsigned(s)); }
            }
        }
    }
};

} // namespace melodee
