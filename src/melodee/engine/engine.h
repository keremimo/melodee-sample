// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
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
    virtual void render(float* out, unsigned frames) = 0;
    virtual bool sounding() const = 0;
};

} // namespace melodee
