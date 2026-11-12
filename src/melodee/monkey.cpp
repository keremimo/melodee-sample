// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// --melodee-monkey: the shell under a random hand. Every control is pressed,
// held and let go in any order (buttons, pads, K1-K3, the fader, the wheel),
// the page is drawn now and then, and after each move what the hand holds is
// checked against what the shell thinks it holds: no layer (Seq, Pad Bank,
// Erase, Mute, Loop, Note Repeat, Sample, Shift) stays on after its button is
// up, no pad sounds after it is up, no step or pattern stays held. Seeded, so a
// failure prints the moves that led to it and runs again the same way.
#include "melodee/tools.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/page.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "melodee/ui/paint.h"
#include "runtime/app.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include <cstdio>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace melodee {
namespace {
using B = SurfaceButton;

class Monkey {
public:
    explicit Monkey(unsigned seed) : rng(seed), seed(seed) { restart(); }

    // `steps` moves; the number of invariants broken (the first few are printed with their moves).
    unsigned run(unsigned steps) {
        for (step = 0; step < steps && failures < 4; ++step) {
            move();
            check();
            host.render(audio.data(), 256, 0.7f);
            if (step % 7 == 0) draw();
            if (step % 500 == 499) { let_go_all(); check(); }
            if (shell->leaving() || shell->powering_off()) { let_go_all(); restart(); } // the Exit sheet left
        }
        let_go_all();
        check();
        return failures;
    }

private:
    EngineHost host;
    std::unique_ptr<Shell> shell;
    SurfaceLights lights;
    std::mt19937 rng;
    unsigned seed, step = 0, failures = 0;
    uint32_t now = 1000;
    std::array<bool, size_t(B::Count)> held {};
    std::array<bool, 16> pad_held {};
    std::vector<std::string> moves;
    std::vector<int32_t> audio = std::vector<int32_t>(4096 * 2);

    void restart() {
        shell = std::make_unique<Shell>(host);
        host.load(shell->project(), shell->patterns());
        note("-- start");
    }
    unsigned pick(unsigned n) { return std::uniform_int_distribution<unsigned>(0, n - 1)(rng); }
    void note(const std::string& what) {
        moves.push_back(what);
        if (moves.size() > 40) moves.erase(moves.begin());
    }

    void button(B b, bool pressed) {
        SurfaceEvent e;
        e.input.kind = SurfaceInput::Kind::Button;
        e.input.index = uint8_t(b);
        e.input.pressed = pressed;
        held[size_t(b)] = pressed;
        note(std::string(pressed ? "press " : "let go ") + surface_buttons[size_t(b)].name);
        shell->surface(e, now);
    }
    void pad(unsigned p, bool down, uint8_t velocity) {
        SurfaceEvent e;
        e.pad = int(p);
        e.midi.status = uint8_t(down ? 0x90 : 0x80);
        e.midi.key = uint8_t(p);
        e.midi.value = down ? velocity : 0;
        pad_held[p] = down;
        note(std::string(down ? "pad " : "pad up ") + std::to_string(p + 1));
        shell->surface(e, now);
    }
    void control(SurfaceControl c, int value, int delta) {
        static const char* const names[] {"volume", "K1", "K2", "K3", "fader", "wheel"};
        SurfaceEvent e;
        e.input.kind = SurfaceInput::Kind::Control;
        e.input.index = uint8_t(c);
        e.input.value = uint8_t(value);
        e.input.delta = delta;
        note(std::string(names[size_t(c) % 6]) + " " + std::to_string(delta ? delta : value));
        shell->surface(e, now);
    }

    void move() {
        unsigned r = pick(100);
        now += 5 + pick(r < 5 ? 1500 : 250);
        if (r < 42) { // a button: down if up, up if down; the layers stay held a while
            static const B weighted[] {B::TopLeft, B::TopMiddle, B::TopRight, B::TopLeft, B::TopMiddle, B::TopRight, B::Shift, B::Shift,
                                       B::Seq, B::Seq, B::Sample, B::Sample, B::PadBank, B::Erase, B::Minus, B::Plus, B::WheelPush,
                                       B::WheelPush, B::Play, B::Stop, B::Mute, B::NoteRepeat, B::Loop, B::SixteenLevels, B::Chop,
                                       B::PadFx, B::KnobFx, B::SampleSelect, B::TapTempo, B::SampleRecord, B::SeqRecord};
            B b = weighted[pick(unsigned(std::size(weighted)))];
            bool layer = b == B::Shift || b == B::Seq || b == B::PadBank || b == B::Erase || b == B::Sample || b == B::Loop
                      || b == B::SixteenLevels; // held a while, it meets Loop: the Looper (shell/looper_chord.h)
            if (held[size_t(b)] && layer && pick(3)) return;
            button(b, !held[size_t(b)]);
        } else if (r < 72) {
            unsigned p = pick(16);
            pad(p, !pad_held[p], uint8_t(1 + pick(127)));
        } else if (r < 82) {
            static const SurfaceControl pots[] {SurfaceControl::K1, SurfaceControl::K2, SurfaceControl::K3};
            control(pots[pick(3)], int(pick(128)), 0);
        } else if (r < 95) {
            int d = int(pick(7)) - 3;
            control(SurfaceControl::Wheel, 0, d ? d : 1);
        } else if (r < 98) {
            control(SurfaceControl::Fader, int(pick(128)), 0);
        } else {
            let_go_all();
        }
    }

    void let_go_all() {
        for (unsigned p = 0; p < 16; ++p) if (pad_held[p]) pad(p, false, 0);
        for (unsigned b = 0; b < unsigned(B::Count); ++b) if (held[b]) button(B(b), false);
    }

    void fail(const std::string& why) {
        if (++failures > 3) return;
        const auto& ui = shell->ui();
        std::printf("MELODEE_MONKEY seed %u move %u: %s (page %u, track %u)\n", seed, step, why.c_str(), unsigned(ui.page), ui.track + 1u);
        for (const auto& m : moves) std::printf("    %s\n", m.c_str());
    }

    void check() {
        const auto& ui = shell->ui();
        auto& c = shell->context();
        if (unsigned(ui.page) >= unsigned(PageId::Count) || ui.track >= track_count || ui.back_depth > UiState::back_capacity)
            fail("the page, track or back stack out of range");
        bool any_pad = false;
        uint32_t bits = 0;
        for (unsigned p = 0; p < 16; ++p) if (pad_held[p]) { any_pad = true; bits |= 1u << p; }
        if (ui.held_pads != bits) fail("the pads lit as held are not the pads down");
        if (ui.held_step >= 0 && !any_pad) fail("step " + std::to_string(ui.held_step + 1) + " held with no pad down");
        if (ui.pattern_held >= 0 && !pad_held[size_t(ui.pattern_held) % 16]) fail("P" + std::to_string(ui.pattern_held + 1) + " held with its pad up");
        for (unsigned p = 0; p < 16; ++p)
            if (!pad_held[p] && c.pad_keys[p][0] >= 0) {
                fail("pad " + std::to_string(p + 1) + " up, its note " + std::to_string(c.pad_keys[p][0]) + " still on");
                c.pad_keys[p].fill(-1); // once
            }
        const std::pair<bool, B> layers[] {{ui.shift, B::Shift}, {ui.erase, B::Erase}, {ui.pad_bank, B::PadBank}, {ui.seq_held, B::Seq},
                                           {ui.mute, B::Mute}, {ui.sample_held, B::Sample}, {ui.loop_held, B::Loop}, {ui.repeat_held, B::NoteRepeat},
                                           {ui.levels_held, B::SixteenLevels}};
        for (const auto& [on, b] : layers)
            if (on && !held[size_t(b)]) fail(std::string(surface_buttons[size_t(b)].name) + " still held after it was let go");
    }

    void draw() {
        shell->lights(lights, now);
        auto snap = std::make_unique<Snapshot>();
        shell->snapshot(*snap, now);
        juce::Image image(juce::Image::ARGB, 320, 240, true);
        juce::Graphics g(image);
        ui::Paint p(g);
        g.fillAll(ui::colour::bg);
        page_spec(snap->ui.page).draw(p, *snap);
        draw_overlays(p, *snap);
    }
};
} // namespace

int monkey(unsigned first, unsigned seeds, unsigned steps) {
    unsigned failed = 0;
    for (unsigned s = first; s < first + seeds; ++s) {
        auto m = std::make_unique<Monkey>(s); // large: off the stack
        if (m->run(steps)) ++failed;
    }
    std::printf("MELODEE_MONKEY seeds=%u moves=%u failed=%u\n", seeds, steps, failed);
    return failed ? 1 : 0;
}

} // namespace melodee
