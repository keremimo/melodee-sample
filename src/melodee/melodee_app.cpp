// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/melodee_app.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "surface/pads.h"
#include "drivers/power_button.h"
#include "melodee/pages/page.h"
#include "melodee/ui/paint.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>


MelodeeApp::MelodeeApp()
    : shell(host), library(host), frame(std::make_unique<melodee::Snapshot>()) {
    shell.set_library(&library);
    shell.set_store(&store);
    host.load(shell.project(), shell.patterns());
}

uint32_t MelodeeApp::now_ms() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

static_assert(usb_stem_count == melodee::track_count && usb_stem_frames == melodee::EngineHost::block && audio_frames <= usb_stem_frames,
              "one USB-C stem a track, a block at a time");

void MelodeeApp::render(const AudioBlock& input, AudioBlock& output) {
    host.render(output.data(), audio_frames, shell.volume(), input.data(), &stem_block.track[0][0]);
}

void MelodeeApp::surface(const SurfaceEvent& event) {
    // A remote tool's prompt owns the controls while it is shown.
    if (session().prompting.load() && event.input.kind == SurfaceInput::Kind::Button) return;
    shell.surface(event, now_ms());
}

void MelodeeApp::lights(SurfaceLights& lights, Clock::time_point) {
    Remote remote;
    while (presses.pop(remote)) {
        if (remote.kind == 1) { // the wheel
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control; event.input.index = uint8_t(SurfaceControl::Wheel); event.input.delta = remote.delta;
            shell.surface(event, now_ms());
            continue;
        }
        auto press = [&](uint8_t index, bool down) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button; event.input.index = index; event.input.pressed = down;
            shell.surface(event, now_ms());
        };
        if (remote.kind == 3) { // a pot moved to a position; the top bit holds Shift around it
            bool shifted = remote.index & 0x80;
            if (shifted) press(uint8_t(SurfaceButton::Shift), true);
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control; event.input.index = remote.index & 0x7f; event.input.value = uint8_t(remote.delta);
            shell.surface(event, now_ms());
            if (shifted) press(uint8_t(SurfaceButton::Shift), false);
            continue;
        }
        if (remote.kind == 4) { press(remote.index, remote.delta != 0); continue; } // hold-BUTTON, release-BUTTON
        if (remote.kind == 5) { // hold-padN, release-padN
            SurfaceEvent event;
            bool down = remote.delta != 0;
            event.pad = remote.index;
            event.midi = {uint8_t(down ? 0x99 : 0x89), surface_pad_notes[remote.index % 16], uint8_t(down ? 100 : 0)};
            shell.surface(event, now_ms());
            continue;
        }
        if (remote.kind == 2) { // a pad, struck and let go; delta - 1 is a button held around it (Shift, Erase, ...)
            int held = remote.delta - 1;
            auto hold = [&](bool down) {
                SurfaceEvent event;
                event.input.kind = SurfaceInput::Kind::Button; event.input.index = uint8_t(held); event.input.pressed = down;
                shell.surface(event, now_ms());
            };
            bool holding = held >= 0;
            if (holding) hold(true);
            for (bool down : {true, false}) {
                SurfaceEvent event;
                event.pad = remote.index;
                event.midi = {uint8_t(down ? 0x99 : 0x89), surface_pad_notes[remote.index % 16], uint8_t(down ? 100 : 0)};
                shell.surface(event, now_ms());
            }
            if (holding) hold(false);
            continue;
        }
        uint8_t button = remote.index;
        // A remote press, as if by hand: down, then up; the top bit holds Shift around it.
        auto send = [&](uint8_t index, bool down) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button; event.input.index = index; event.input.pressed = down;
            shell.surface(event, now_ms());
        };
        bool shifted = button & 0x80;
        if (shifted) send(uint8_t(SurfaceButton::Shift), true);
        send(button & 0x7f, true); send(button & 0x7f, false);
        if (shifted) send(uint8_t(SurfaceButton::Shift), false);
    }
    shell.lights(lights, now_ms());
}

std::string MelodeeApp::command(const std::string& action, const std::string& argument, Hardware&) {
    if (action != "melodee-press") return {};
    if (argument.rfind("wheel", 0) == 0 && argument.size() > 6 && (argument[5] == '+' || argument[5] == '-') &&
        std::isdigit(static_cast<unsigned char>(argument[6]))) { // wheel+N, wheel-N (not wheel-push)
        int delta = std::atoi(argument.c_str() + 5);
        return presses.push({1, 0, int8_t(std::clamp(delta, -64, 63))}) ? "OK melodee-press " + argument : "ERR busy";
    }
    {   // k1=64, or shift+k3=64 (Shift held around the move: the card's second parameter)
        bool shift_pot = argument.rfind("shift+", 0) == 0;
        std::string pot_text = shift_pot ? argument.substr(6) : argument;
        if (pot_text.size() > 3 && pot_text[0] == 'k' && pot_text[1] >= '1' && pot_text[1] <= '3' && pot_text[2] == '=') {
            int value = std::clamp(std::atoi(pot_text.c_str() + 3), 0, 127);
            auto pot = unsigned(SurfaceControl::K1) + unsigned(pot_text[1] - '1');
            return presses.push({3, uint8_t(pot | (shift_pot ? 0x80 : 0)), int8_t(value)}) ? "OK melodee-press " + argument : "ERR busy";
        }
    }
    if (argument.rfind("fader=", 0) == 0 && argument.size() > 6) { // fader=V: the fader to V (0..127), as if moved there
        int value = std::clamp(std::atoi(argument.c_str() + 6), 0, 127);
        return presses.push({3, uint8_t(SurfaceControl::Fader), int8_t(value)}) ? "OK melodee-press " + argument : "ERR busy";
    }
    auto button_named = [](const std::string& wanted) {
        for (unsigned n = 0; n < surface_buttons.size(); ++n) {
            std::string name = surface_buttons[n].name;
            for (auto& c : name) c = char(c == ' ' ? '-' : std::tolower(static_cast<unsigned char>(c)));
            if (name == wanted) return int(n);
        }
        return -1;
    };
    // pad5, or a button held around it: shift+pad5, erase+pad5, pad-bank+pad5 (not pad-fx)
    auto plus = argument.rfind("+pad");
    size_t at = plus == std::string::npos ? 3 : plus + 4;
    bool pad_press = (plus == std::string::npos ? argument.rfind("pad", 0) == 0 : true) &&
                     argument.size() > at && std::isdigit(static_cast<unsigned char>(argument[at]));
    if (pad_press) {
        int held = plus == std::string::npos ? -1 : button_named(argument.substr(0, plus));
        if (plus != std::string::npos && held < 0) return "ERR no button " + argument.substr(0, plus);
        int pad = std::atoi(argument.c_str() + at);
        if (pad < 1 || pad > 16) return "ERR pads are pad1 to pad16";
        return presses.push({2, uint8_t(pad - 1), int8_t(held + 1)}) ? "OK melodee-press " + argument : "ERR busy";
    }
    // hold-shift ... release-shift: a button (or hold-pad5: a pad) held down across other presses
    for (const char* verb : {"hold-", "release-"}) {
        std::string prefix = verb;
        if (argument.rfind(prefix, 0) != 0) continue;
        std::string what = argument.substr(prefix.size());
        if (what.rfind("pad", 0) == 0 && what.size() > 3 && std::isdigit(static_cast<unsigned char>(what[3]))) {
            int pad = std::atoi(what.c_str() + 3);
            if (pad < 1 || pad > 16) return "ERR pads are pad1 to pad16";
            return presses.push({5, uint8_t(pad - 1), int8_t(prefix == "hold-")}) ? "OK melodee-press " + argument : "ERR busy";
        }
        int n = button_named(what);
        if (n < 0) return "ERR no button " + argument.substr(prefix.size());
        return presses.push({4, uint8_t(n), int8_t(prefix == "hold-")}) ? "OK melodee-press " + argument : "ERR busy";
    }
    bool shifted = argument.rfind("shift+", 0) == 0;
    std::string wanted = shifted ? argument.substr(6) : argument;
    if (int n = button_named(wanted); n >= 0) return presses.push({0, uint8_t(n | (shifted ? 0x80 : 0)), 0}) ? "OK melodee-press " + argument : "ERR busy";
    return "ERR melodee-press takes a button, optionally shift+: sample, seq, play, stop, top-left, wheel-push, ...";
}

void MelodeeApp::draw(juce::Graphics& g) {
    using namespace melodee;
    shell.snapshot(*frame, now_ms());
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(frame->ui.page).draw(p, *frame);
    draw_overlays(p, *frame);
}

// The speaker plays while neither headphones nor line out is plugged in. The
// runtime leaves it as the stock app did, which is often off.
void MelodeeApp::jack(const JackEvent&, Hardware& hardware) {
    bool plugged = session().plugged(Jack::Headphones) == 1 || session().plugged(Jack::LineOut) == 1;
    hardware.mixer.speaker(!plugged);
}

void MelodeeApp::service(Hardware& hardware) {
    library.service(hardware);
    store.service(hardware);
    // A short press of Power opens the Exit sheet.
    uint64_t presses = hardware.power.presses();
    if (power_known && presses > power_presses && !hardware.power.pressed()) shell.power_pressed();
    if (!hardware.power.pressed()) { power_presses = presses; power_known = true; }
    // Settings' Screen row: the backlight, as a share of its range.
    unsigned screen = shell.screen();
    if (screen && screen != screen_applied) {
        auto backlight = hardware.platform.backlight();
        if (backlight.maximum > 0) hardware.platform.brightness(unsigned(std::max(1L, backlight.maximum * long(screen) / 100)));
        screen_applied = screen;
    }
}

std::string MelodeeApp::status() const {
    return "melodee_playing=" + std::to_string(host.playing()) + " melodee_dropped=" + std::to_string(host.dropped()) + " " + shell.summary();
}
