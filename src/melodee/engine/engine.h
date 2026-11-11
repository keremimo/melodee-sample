// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <cstdint>

// A sound engine as the engine host drives it, on the audio thread only.
// Engines are created up front and never allocate; render() adds the
// engine's mono output into `out`.
namespace melodee {

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
    // An engine with its own stereo picture (the sampler's pad pans) renders
    // both sides itself; the track's pan then works as a balance.
    virtual bool stereo() const { return false; }
    virtual void render_stereo(float* /*left*/, float* /*right*/, unsigned /*frames*/) {}
    virtual bool sounding() const = 0;
};

} // namespace melodee
