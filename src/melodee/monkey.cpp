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
#include "melodee/shell/bindings.h"
#include "melodee/shell/fader_job.h"
#include "melodee/shell/motion_points.h"
#include "melodee/model/limits.h"
#include <algorithm>
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
        shell->load_engine();
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
        if (shell->ui().loading) { // the Loading sheet takes the hands' presses: they wait for the project, as a player does
            now += 20;
            shell->lights(lights, now);
            return;
        }
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

    // One snapshot kept from draw to draw, as the app's is. Three frames each time: the first after the moves, taken
    // whole, then two with no control used, light ones. Shell::snapshot() checks each light one against a whole one
    // (snapshot_verify) and counts those that differ: a kit or a pattern changed with no word of it. The last, a
    // light one, is the one drawn.
    std::unique_ptr<Snapshot> snap = std::make_unique<Snapshot>();
    unsigned stale_seen = 0;
    void draw() {
        for (unsigned frame = 0; frame < 3; ++frame) {
            shell->lights(lights, now);
            shell->snapshot(*snap, now);
        }
        if (unsigned stale = shell->snapshot_stale(); stale != stale_seen) {
            stale_seen = stale;
            fail("a light snapshot differed from a whole one: a kit or a pattern changed that the screen was not told of");
        }
        juce::Image image(juce::Image::ARGB, 320, 240, true);
        juce::Graphics g(image);
        ui::Paint p(g);
        g.fillAll(ui::colour::bg);
        page_spec(snap->ui.page).draw(p, *snap);
        draw_overlays(p, *snap);
    }
};
// Borrow each mutable view on a fresh shell so one accessor cannot mask a bug in the other.
bool preview_snapshot_edits() {
    auto host = std::make_unique<EngineHost>();
    for (bool edit_pattern : {false, true}) {
        auto shell = std::make_unique<Shell>(*host);
        shell->load_engine();
        auto frame = std::make_unique<Snapshot>();
        shell->snapshot(*frame, 1000);
        shell->snapshot(*frame, 1001);
        if (frame->whole) return false; // engine initialization still allows light frames
        if (edit_pattern) {
            auto& pattern = shell->patterns().at[0][0];
            shell->snapshot(*frame, 1002);
            pattern.length += sixteenth_ticks;
            shell->snapshot(*frame, 1003);
            if (frame->sketches[0][0].length != pattern.length) return false;
        } else {
            auto& pad = shell->project().tracks[0].pads[0];
            shell->snapshot(*frame, 1002);
            pad.sample ^= 1;
            shell->snapshot(*frame, 1003);
            if (frame->project.tracks[0].pads[0].sample != pad.sample) return false;
        }
        if (shell->snapshot_stale()) return false; // verification must never have to repair a frame
    }
    return true;
}
// Motion's draw mode (shell/motion_points.h) by its own hand: the controls pressed in order on a MIDI track, each
// edit checked on the pattern (what Push, a knob, the fader, Shift + wheel and Erase do to a point; a lock left
// alone; a lane Motion does not show set as on any page; the limit on lines at once). The number of checks broken.
unsigned motion_script() {
    auto host = std::make_unique<EngineHost>();
    auto shell = std::make_unique<Shell>(*host);
    shell->load_engine();
    auto& c = shell->context();
    auto& ui = shell->ui();
    uint32_t now = 1000;
    unsigned failed = 0, checked = 0;
    auto expect = [&](bool ok, const char* what) {
        ++checked;
        if (ok) return;
        ++failed;
        std::printf("MELODEE_MONKEY motion script: %s (notice \"%s\")\n", what, ui.notice);
    };
    auto button = [&](B b, bool pressed) {
        SurfaceEvent e;
        e.input.kind = SurfaceInput::Kind::Button;
        e.input.index = uint8_t(b);
        e.input.pressed = pressed;
        now += 40;
        shell->surface(e, now);
    };
    auto tap = [&](B b) { button(b, true); button(b, false); };
    auto control = [&](SurfaceControl which, int value, int delta) {
        SurfaceEvent e;
        e.input.kind = SurfaceInput::Kind::Control;
        e.input.index = uint8_t(which);
        e.input.value = uint8_t(value);
        e.input.delta = delta;
        now += 40;
        shell->surface(e, now);
    };
    auto with_shift = [&](auto&& act) { button(B::Shift, true); act(); button(B::Shift, false); };

    ui.settings.knob_takeover = ui.settings.fader_takeover = Takeover::Instant; // a turn sets the value it points at
    c.now = now;
    c.load_sound(0, EngineKind::Midi, 0);
    ui.track = 0;
    c.open(PageId::Motion);
    auto& track = c.track();
    auto lanes = motion_lanes(c.project, ui, c.pattern());
    const int lane = lanes[0];
    expect(lane >= 0, "a MIDI track shows a lane on K1");
    if (lane < 0) return failed;
    const unsigned lp = lane_param(lane), lk = lane_pad(lane);
    const uint32_t q = c.pattern().q();
    auto point = [&](uint32_t tick) { return c.pattern().point_at(tick, lp, lk); };

    with_shift([&] { tap(B::TopRight); });
    expect(ui.motion_draw, "Shift + B3 turns draw mode on");
    control(SurfaceControl::K1, 40, 0); // no point at the cursor: the track's own value, and K1's lane is the one drawn
    const int own = motion_param(track, lane, &c.project).value;
    control(SurfaceControl::Wheel, 0, 4);
    expect(ui.motion_cursor == 4 * q, "the wheel moves the cursor a step of Q a detent");
    tap(B::WheelPush);
    expect(point(4 * q) && point(4 * q)->ramp, "Push adds a point that goes in a line");
    c.undo();
    expect(!point(4 * q), "undo takes the point out again");
    tap(B::WheelPush);
    control(SurfaceControl::K1, 100, 0);
    expect(point(4 * q) && point(4 * q)->value != 40 && motion_param(track, lane, &c.project).value == own,
           "K1 on a point sets the point, not the track's value");
    if (fader_job(c.project, ui).binding.bound()) { // the fader follows K1 (fader C)
        int before = point(4 * q) ? point(4 * q)->value : -1;
        control(SurfaceControl::Fader, 10, 0);
        control(SurfaceControl::Fader, 20, 0);
        expect(point(4 * q) && point(4 * q)->value != before && motion_param(track, lane, &c.project).value == own,
               "the fader on K1's lane sets the point, not the track's value");
    } else std::puts("MELODEE_MONKEY motion script: the fader has no job on this track, its step skipped");
    with_shift([&] { control(SurfaceControl::Wheel, 0, 1); });
    expect(!point(4 * q) && point(5 * q) && ui.motion_cursor == 5 * q, "Shift + wheel moves the point a step of Q");
    tap(B::Erase);
    expect(!point(5 * q), "Erase takes the point under the cursor out");

    { // a lock stays a lock: Push on its tick adds nothing
        MotionPoint lock;
        lock.tick = 5 * q; lock.param = uint8_t(lp); lock.pad = uint8_t(lk); lock.value = 9; lock.hold = 6;
        c.pattern().write_motion(lock);
        tap(B::WheelPush);
        const auto* still = c.pattern().lock_at(5 * q, lp, lk);
        expect(still && still->value == 9 && !point(5 * q), "Push on a lock's tick leaves the lock");
        c.pattern().clear_lane(lp, lk);
    }
    { // Shift + wheel with no point under the cursor changes nothing (no fine edit of another lane's point)
        auto motion = c.pattern().motion;
        int was = motion_param(track, lane, &c.project).value;
        with_shift([&] { control(SurfaceControl::Wheel, 0, 1); });
        expect(c.pattern().motion == motion && motion_param(track, lane, &c.project).value == was,
               "Shift + wheel off a point changes nothing");
    }
    { // a lane Motion does not show is set as on any page, its point left alone
        const int hidden = motion_lane(motion_level);
        auto shown = motion_lanes(c.project, ui, c.pattern());
        std::vector<int> filled;
        for (int other : shown) if (other >= 0) filled.push_back(other);
        filled.push_back(motion_lane(motion_wheel)); filled.push_back(motion_lane(motion_pan));
        for (int other : filled) { // three or more lanes hold motion from tick 0: a later one with a point stays hidden
            MotionPoint held;
            held.param = uint8_t(lane_param(other)); held.pad = uint8_t(lane_pad(other)); held.value = 1;
            c.pattern().write_motion(held);
        }
        shown = motion_lanes(c.project, ui, c.pattern());
        expect(std::find(shown.begin(), shown.end(), hidden) == shown.end(), "the strip's level is not one of Motion's lanes");
        MotionPoint level;
        level.tick = motion_cursor(ui, c.pattern()); level.param = uint8_t(motion_level); level.value = 33; level.ramp = true;
        c.pattern().write_motion(level);
        shown = motion_lanes(c.project, ui, c.pattern());
        if (std::find(shown.begin(), shown.end(), hidden) == shown.end()) { // with a point it may show; only then it is hidden
            c.apply(Binding {Binding::Kind::Level, 0, 0, 127, int(track.mix.level)}, 50);
            expect(track.mix.level == 50 && c.pattern().point_at(level.tick, motion_level, 0)->value == 33,
                   "a hidden lane's card sets the track, not its point");
        } else std::puts("MELODEE_MONKEY motion script: the strip's level showed once it had a point, its step skipped");
        c.pattern().clear_lane(motion_level, 0);
        for (int other : filled) c.pattern().clear_lane(lane_param(other), lane_pad(other));
    }
    { // more lanes in a line at once than a track plays: Push is taken back
        control(SurfaceControl::K1, 0, 0); // K1's lane the one drawn again (the level card turned it last): a turn that changes it
        auto& p = c.pattern();
        for (unsigned k = 0; k < motion_ramps_at_once; ++k) {
            MotionPoint a, b;
            a.param = b.param = uint8_t(100 + k); a.ramp = true; b.tick = p.length;
            a.value = 0; b.value = 1;
            p.write_motion(a); p.write_motion(b);
        }
        MotionPoint end;
        end.tick = p.length - q; end.param = uint8_t(lp); end.pad = uint8_t(lk); end.value = 5;
        p.write_motion(end);
        auto motion = p.motion;
        tap(B::WheelPush); // at the cursor, before `end`: a 65th line
        expect(p.motion == motion, "Push past the limit on lines at once is taken back");
    }
    with_shift([&] { tap(B::TopRight); });
    expect(!ui.motion_draw, "Shift + B3 turns draw mode off");
    std::printf("MELODEE_MONKEY motion script: %u checks, %u failed\n", checked, failed);
    return failed;
}
} // namespace

int monkey(unsigned first, unsigned seeds, unsigned steps) {
    Shell::snapshot_verify = true;
    if (!preview_snapshot_edits()) {
        std::puts("MELODEE_MONKEY retained preview edit or light snapshot check failed");
        return 1;
    }
    if (motion_script()) return 1;
    unsigned failed = 0;
    for (unsigned s = first; s < first + seeds; ++s) {
        auto m = std::make_unique<Monkey>(s); // large: off the stack
        if (m->run(steps)) ++failed;
    }
    std::printf("MELODEE_MONKEY seeds=%u moves=%u failed=%u\n", seeds, steps, failed);
    return failed ? 1 : 0;
}

} // namespace melodee
