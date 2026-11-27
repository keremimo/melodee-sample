// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/ui_state.h"
#include "surface/controls.h"
#include <cstdint>

// Lessons (the MENU's GO, pad 16 LEARN). A lesson is a list of steps, each with one control to use. That control's
// light blinks and every other light is off; Shift, Minus / Plus, B1-B3, the encoder and K1-K3 have no light, so
// their step pulses on the screen (pages/overlays.cpp). The header's first slot shows the step and two or three
// words. Any other press does nothing. A lesson runs on a project of its own, made new with the MPC's factory kit
// on track 1: the open project is saved first and opened again when the lesson ends, and the lesson's project is
// never saved (shell/lesson_jobs.h). Only Basic's pages show while it runs (shell/features.h), so it reads the same
// on Basic and Full. The encoder held opens the MENU, where pad 16 (LEAVE) ends it. Pure, for the shell, the screen
// and the tests.
namespace melodee {

// The kit New project's KIT starts with, and a lesson plays: one of the MPC's own, on its internal storage.
constexpr const char* factory_kit_path = "/content/Projects/Kits/Kit-SFM MPC60 Bang 75.xpj";
constexpr const char* factory_kit_name = "MPC60 Bang";

enum class LessonKind : uint8_t {
    Wait,   // nothing to press: the kit loads
    Count,  // nothing to press: Seq Record's count-in runs out (Shift + Pad 4; off, the step passes at once)
    Button, // id: a SurfaceButton, pressed
    Pad,    // id: the pad, 0-15, hit
    Knob,   // id: K1-K3 as 0-2, turned a little
    Wheel,  // the encoder turned
    Push,   // the encoder pushed and let go
};
struct LessonStep {
    const char* words;  // the header's: two or three
    LessonKind kind;
    uint8_t id = 0;
    int8_t with = -1;   // a SurfaceButton held with it (Shift, Seq, Pad FX), -1 none
    // The page the control opens, when it opens one: the step is done once that page is open (a tap held too long
    // opens nothing, and the step waits for another). Count: none, the step is done as the control is used.
    PageId page = PageId::Count;
};
struct Lesson {
    const char* name;
    uint8_t minutes;
    const LessonStep* steps;
    uint8_t count;
};

constexpr unsigned lesson_count = 4;
constexpr uint8_t lesson_knob_travel = 10;    // a knob's step is done once it has moved this far (of 127)
constexpr uint32_t lesson_wait_ms = 15000;    // the kit not there by then: the lesson ends
constexpr uint32_t lesson_done_ms = 1500;     // DONE stays this long before the project opens again
constexpr uint32_t lesson_count_ms = 200;     // a count-in step looks for the count this long after Play at the earliest

inline const Lesson& lesson_at(unsigned n) {
    using K = LessonKind;
    using B = SurfaceButton;
    constexpr auto b = [](B button) { return uint8_t(button); };
    constexpr auto w = [](B button) { return int8_t(button); };
    // First beat: two pads recorded into a pattern.
    static const LessonStep beat[] {
        {"LOADING KIT", K::Wait},
        {"TAP PAD 1", K::Pad, 0},
        {"TAP PAD 2", K::Pad, 1},
        {"PRESS SEQ RECORD", K::Button, b(B::SeqRecord)},
        {"PRESS PLAY", K::Button, b(B::Play)},
        {"COUNT-IN", K::Count}, // a pad hit in the count-in is not recorded: the pads wait for the take
        {"TAP PAD 1", K::Pad, 0},
        {"TAP PAD 2", K::Pad, 1},
        {"PRESS SEQ RECORD", K::Button, b(B::SeqRecord)},
        {"PRESS STOP", K::Button, b(B::Stop)},
    };
    // Sequences: two sequences launched from their page, then the track's pattern 2 with Seq held from another page.
    static const LessonStep sequences[] {
        {"LOADING KIT", K::Wait},
        {"PRESS PLAY", K::Button, b(B::Play)},
        {"PRESS SEQ", K::Button, b(B::Seq), -1, PageId::Scenes},
        {"TAP PAD 2", K::Pad, 1},
        {"TAP PAD 1", K::Pad, 0},
        {"PRESS SAMPLE", K::Button, b(B::Sample), -1, PageId::Stage},
        {"SEQ + PAD 2", K::Pad, 1, w(B::Seq)},
        {"PRESS STOP", K::Button, b(B::Stop)},
    };
    // Mix: the mixer opened, a track's send, another track picked, its effects, and back (docs/MIXER_PLAN.md).
    static const LessonStep mix[] {
        {"LOADING KIT", K::Wait},
        {"PAD FX + KNOB FX", K::Button, b(B::KnobFx), w(B::PadFx), PageId::Mixer},
        {"TURN K1", K::Knob, 0},
        {"TAP PAD 2", K::Pad, 1},
        {"SHIFT + KNOB FX", K::Button, b(B::KnobFx), w(B::Shift), PageId::Rack},
        {"PRESS B1", K::Button, b(B::TopLeft), -1, PageId::Mixer},
        {"PRESS SAMPLE", K::Button, b(B::Sample), -1, PageId::Stage},
    };
    // Shape a pad: its end on Trim, then its tune.
    static const LessonStep shape[] {
        {"LOADING KIT", K::Wait},
        {"TAP PAD 1", K::Pad, 0},
        {"TURN K2", K::Knob, 1},
        {"TAP PAD 1", K::Pad, 0},
        {"PRESS B1", K::Button, b(B::TopLeft)},
        {"TURN K1", K::Knob, 0},
        {"TAP PAD 1", K::Pad, 0},
    };
    static const Lesson lessons[lesson_count] {
        {"First beat", 2, beat, uint8_t(sizeof(beat) / sizeof(beat[0]))},
        {"Sequences", 2, sequences, uint8_t(sizeof(sequences) / sizeof(sequences[0]))},
        {"Mix a track", 2, mix, uint8_t(sizeof(mix) / sizeof(mix[0]))},
        {"Shape a pad", 2, shape, uint8_t(sizeof(shape) / sizeof(shape[0]))},
    };
    return lessons[n % lesson_count];
}

// The step in hand; nullptr when no lesson runs or its last step is done (DONE shows, then it ends).
inline const LessonStep* lesson_step(const UiState& ui) {
    if (!ui.lesson) return nullptr;
    const Lesson& l = lesson_at(ui.lesson - 1u);
    return ui.lesson_step < l.count ? &l.steps[ui.lesson_step] : nullptr;
}
inline bool lesson_finished(const UiState& ui) { return ui.lesson && !lesson_step(ui); }
// Whether the step's held button is down (a step without one: yes).
inline bool lesson_held(const UiState& ui, const LessonStep& s) { return s.with < 0 || ui.lesson_with == s.with; }

// Whether a press acts during a lesson: the step's control, the button it is held with, and the encoder (held,
// it opens the MENU: the way out). With the MENU open: B1 and pad 16 only.
inline bool lesson_takes_button(const UiState& ui, SurfaceButton button) {
    if (button == SurfaceButton::WheelPush) return true;
    if (ui.menu) return button == SurfaceButton::TopLeft;
    const LessonStep* s = lesson_step(ui);
    if (!s) return false;
    if (s->with >= 0 && int(button) == int(s->with)) return true;
    return s->kind == LessonKind::Button && unsigned(button) == s->id && lesson_held(ui, *s);
}
inline bool lesson_takes_pad(const UiState& ui, unsigned pad) {
    if (ui.menu) return pad % 16 == 15;
    const LessonStep* s = lesson_step(ui);
    return s && s->kind == LessonKind::Pad && s->id == pad % 16 && lesson_held(ui, *s);
}
inline bool lesson_takes_knob(const UiState& ui, unsigned knob) {
    const LessonStep* s = lesson_step(ui);
    return !ui.menu && s && s->kind == LessonKind::Knob && s->id == knob % 3;
}
inline bool lesson_takes_wheel(const UiState& ui) {
    const LessonStep* s = lesson_step(ui);
    return !ui.menu && s && s->kind == LessonKind::Wheel;
}
inline bool lesson_takes_push(const UiState& ui) {
    const LessonStep* s = lesson_step(ui);
    return !ui.menu && s && s->kind == LessonKind::Push;
}

// On to the next step.
inline void lesson_next(UiState& ui, uint32_t now) {
    ++ui.lesson_step;
    ui.lesson_pot = -1;
    ui.lesson_at = now;
}
// A button that acted: the held button goes down or up, the step's own button ends the step.
inline void lesson_button(UiState& ui, SurfaceButton button, bool pressed, uint32_t now) {
    const LessonStep* s = lesson_step(ui);
    if (!pressed) { if (ui.lesson_with == int8_t(button)) ui.lesson_with = -1; return; }
    if (!s || ui.menu) return;
    if (s->with >= 0 && int(button) == int(s->with)) { ui.lesson_with = int8_t(button); return; }
    if (s->page != PageId::Count) return; // done when its page is open (lesson_page())
    if (s->kind == LessonKind::Button && unsigned(button) == s->id && lesson_held(ui, *s)) lesson_next(ui, now);
}
// Often: a step whose control opens a page is done once that page is open.
inline void lesson_page(UiState& ui, uint32_t now) {
    const LessonStep* s = lesson_step(ui);
    if (s && s->page != PageId::Count && ui.page == s->page && !ui.menu) lesson_next(ui, now);
}
inline void lesson_pad(UiState& ui, unsigned pad, uint32_t now) { if (lesson_takes_pad(ui, pad) && !ui.menu) lesson_next(ui, now); }
inline void lesson_knob(UiState& ui, unsigned knob, int position, uint32_t now) {
    if (!lesson_takes_knob(ui, knob)) return;
    if (ui.lesson_pot < 0) { ui.lesson_pot = int16_t(position); return; }
    int moved = position - ui.lesson_pot;
    if ((moved < 0 ? -moved : moved) >= int(lesson_knob_travel)) lesson_next(ui, now);
}
inline void lesson_wheel(UiState& ui, uint32_t now) { if (lesson_takes_wheel(ui)) lesson_next(ui, now); }
inline void lesson_push(UiState& ui, uint32_t now) { if (lesson_takes_push(ui)) lesson_next(ui, now); }

// The lights: whether a button or a pad is the one that blinks (the step's, and the button held with it).
inline bool lesson_lights_button(const UiState& ui, SurfaceButton button) {
    const LessonStep* s = lesson_step(ui);
    if (!s) return false;
    if (s->with >= 0 && int(button) == int(s->with)) return true;
    return s->kind == LessonKind::Button && unsigned(button) == s->id;
}
inline bool lesson_lights_pad(const UiState& ui, unsigned pad) {
    const LessonStep* s = lesson_step(ui);
    return s && s->kind == LessonKind::Pad && s->id == pad % 16;
}
// The screen: the soft key (0-2) or the knob card (0-2) that pulses, -1 none.
inline int lesson_soft_key(const UiState& ui) {
    const LessonStep* s = lesson_step(ui);
    if (!s || s->kind != LessonKind::Button) return -1;
    return s->id == uint8_t(SurfaceButton::TopLeft) ? 0 : s->id == uint8_t(SurfaceButton::TopMiddle) ? 1 : s->id == uint8_t(SurfaceButton::TopRight) ? 2 : -1;
}
inline int lesson_knob_card(const UiState& ui) {
    const LessonStep* s = lesson_step(ui);
    return s && s->kind == LessonKind::Knob ? int(s->id % 3) : -1;
}

} // namespace melodee
