// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/melodee_app.h"
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

void MelodeeApp::render(const AudioBlock& input, AudioBlock& output) { host.render(output.data(), audio_frames, shell.volume(), input.data()); }

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
        if (remote.kind == 3) { // a pot moved to a position
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control; event.input.index = remote.index; event.input.value = uint8_t(remote.delta);
            shell.surface(event, now_ms());
            continue;
        }
        if (remote.kind == 2) { // a pad, struck and let go
            for (bool down : {true, false}) {
                SurfaceEvent event;
                event.pad = remote.index;
                event.midi = {uint8_t(down ? 0x99 : 0x89), surface_pad_notes[remote.index % 16], uint8_t(down ? 100 : 0)};
                shell.surface(event, now_ms());
            }
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
    if (argument.size() > 3 && argument[0] == 'k' && argument[1] >= '1' && argument[1] <= '3' && argument[2] == '=') { // k1=64
        int value = std::clamp(std::atoi(argument.c_str() + 3), 0, 127);
        auto pot = SurfaceControl(unsigned(SurfaceControl::K1) + unsigned(argument[1] - '1'));
        return presses.push({3, uint8_t(pot), int8_t(value)}) ? "OK melodee-press " + argument : "ERR busy";
    }
    if (argument.rfind("pad", 0) == 0 && argument.size() > 3) {
        int pad = std::atoi(argument.c_str() + 3);
        if (pad < 1 || pad > 16) return "ERR pads are pad1 to pad16";
        return presses.push({2, uint8_t(pad - 1), 0}) ? "OK melodee-press " + argument : "ERR busy";
    }
    bool shifted = argument.rfind("shift+", 0) == 0;
    std::string wanted = shifted ? argument.substr(6) : argument;
    for (unsigned n = 0; n < surface_buttons.size(); ++n) {
        std::string name = surface_buttons[n].name;
        for (auto& c : name) c = char(c == ' ' ? '-' : std::tolower(static_cast<unsigned char>(c)));
        if (name == wanted) return presses.push({0, uint8_t(n | (shifted ? 0x80 : 0)), 0}) ? "OK melodee-press " + argument : "ERR busy";
    }
    return "ERR melodee-press takes a button, optionally shift+: sample, seq, play, stop, top-left, wheel-push, ...";
}

void MelodeeApp::draw(juce::Graphics& g) {
    using namespace melodee;
    shell.snapshot(*frame, now_ms());
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(frame->ui.page).draw(p, *frame);
    if (frame->ui.exit_open) draw_exit_sheet(p, *frame);
    else if (frame->ui.shift && !frame->ui.shift_used) draw_shift_layer(p, *frame);
}

void MelodeeApp::service(Hardware& hardware) {
    library.service(hardware);
    store.service(hardware);
    // A short press of Power opens the Exit sheet.
    uint64_t presses = hardware.power.presses();
    if (power_known && presses > power_presses && !hardware.power.pressed()) shell.power_pressed();
    if (!hardware.power.pressed()) { power_presses = presses; power_known = true; }
}

std::string MelodeeApp::status() const {
    return "melodee_playing=" + std::to_string(host.playing()) + " melodee_dropped=" + std::to_string(host.dropped()) + " " + shell.summary();
}
