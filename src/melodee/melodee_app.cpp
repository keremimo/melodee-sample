// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/melodee_app.h"
#include "drivers/power_button.h"
#include "melodee/pages/page.h"
#include "melodee/ui/paint.h"
#include <cctype>


MelodeeApp::MelodeeApp()
    : shell(host), frame(std::make_unique<melodee::Snapshot>()) {
    host.load(shell.project(), shell.patterns());
}

uint32_t MelodeeApp::now_ms() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

void MelodeeApp::render(const AudioBlock&, AudioBlock& output) { host.render(output.data(), audio_frames, shell.volume()); }

void MelodeeApp::surface(const SurfaceEvent& event) {
    // A remote tool's prompt owns the controls while it is shown.
    if (session().prompting.load() && event.input.kind == SurfaceInput::Kind::Button) return;
    shell.surface(event, now_ms());
}

void MelodeeApp::lights(SurfaceLights& lights, Clock::time_point) {
    uint8_t button;
    while (presses.pop(button)) {
        // A remote press, as if by hand: down, then up.
        for (bool down : {true, false}) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button; event.input.index = button; event.input.pressed = down;
            shell.surface(event, now_ms());
        }
    }
    shell.lights(lights, now_ms());
}

std::string MelodeeApp::command(const std::string& action, const std::string& argument, Hardware&) {
    if (action != "melodee-press") return {};
    for (unsigned n = 0; n < surface_buttons.size(); ++n) {
        std::string name = surface_buttons[n].name;
        for (auto& c : name) c = char(c == ' ' ? '-' : std::tolower(static_cast<unsigned char>(c)));
        if (name == argument) return presses.push(uint8_t(n)) ? "OK melodee-press " + argument : "ERR busy";
    }
    return "ERR melodee-press takes a button: sample, seq, play, stop, top-left, wheel-push, ...";
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
    // A short press of Power opens the Exit sheet.
    uint64_t presses = hardware.power.presses();
    if (power_known && presses > power_presses && !hardware.power.pressed()) shell.power_pressed();
    if (!hardware.power.pressed()) { power_presses = presses; power_known = true; }
}

std::string MelodeeApp::status() const {
    return "melodee_playing=" + std::to_string(host.playing()) + " melodee_dropped=" + std::to_string(host.dropped());
}
