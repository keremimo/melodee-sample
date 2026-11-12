// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>

// A sound engine as the engine host drives it, on the audio thread only.
// Engines are created up front and never allocate; render() adds the
// engine's mono output into `out`.
namespace melodee {

// Roadmap P2: a synth's modulation (its LFOs, pitch, filter tuning and levels) worked out once a step of `frames` of
// its own, counted from its last change (a note, a value, the wheel), never from where a render call starts: so
// however the host cuts a block between two of its calls, the engine plays the same, to the sample. At a step's
// start what moves with time moves on by the frames played since the last one began (`since`).
struct ControlStep {
    unsigned left = 0;  // frames of the step in hand still to play; 0: the next frame starts one
    unsigned since = 0; // frames played since the step in hand began
    void restart() { left = 0; } // a change: the next frame starts a step
    // At a frame to play: true when a step starts here (and `frames` is its length); then run() with each run's frames.
    bool starts(unsigned frames) {
        if (left) return false;
        left = frames;
        return true;
    }
    unsigned run(unsigned wanted) const { return wanted < left ? wanted : left; } // the frames before the step ends
    // After a run: true when it ended the step (what is checked once a step, such as a voice let go, comes now).
    bool played(unsigned frames) { left -= frames; since += frames; return left == 0; }
    unsigned take() { unsigned s = since; since = 0; return s; } // at a step's start: the frames since the last one's
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual void note_on(int key, int velocity) = 0;
    virtual void note_off(int key) = 0;
    virtual void all_notes_off() = 0; // release held notes
    virtual void silence() = 0;       // stop every voice at once
    virtual void set_param(int id, int value) = 0;
    virtual void set_wheel(float /*0..1*/) {}
    virtual void set_bend(float /*-1..1*/) {}
    virtual void set_mod(unsigned /*route*/, const ModRoute&) {}
    virtual void set_pressure(int /*key*/, float /*0..1*/) {}
    // The sequencer's accent: the notes started until the next call this much louder (1: none).
    virtual void set_accent(float /*gain*/) {}
    // A note's Slide or Tie: `from`, still held, carries on as `to` without a new attack, its pitch
    // gliding there when `glide`; the same key just goes on. An engine that cannot (the sampler, the
    // 909) plays `to` and releases `from`. A synth with no held `from` plays `to` as ever.
    virtual void slide(int from, int to, int velocity, bool /*glide*/) {
        if (from == to) return;
        note_on(to, velocity);
        note_off(from);
    }
    virtual void render(float* out, unsigned frames) = 0;
    // Roadmap P2, the overload guard (EngineHost): the blocks are near their time, so the next ones must be lighter.
    // Every voice already let go ends its tail in a few tens of ms; with `steal` the oldest held voice too.
    virtual bool relieve(bool /*steal*/) { return false; } // true: a voice was stolen
    // An engine with its own stereo picture (the sampler's pad pans) renders
    // both sides itself; the track's pan then works as a balance.
    virtual bool stereo() const { return false; }
    virtual void render_stereo(float* /*left*/, float* /*right*/, unsigned /*frames*/) {}
    virtual bool sounding() const = 0;
};

} // namespace melodee
