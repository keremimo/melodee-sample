// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine.h"
#include "melodee/engine/macros.h"
#include "melodee/plugin/plugin_runner.h"
#include <algorithm>
#include <cmath>

// A PLUGIN track: the notes, wheel, bend and pressure as MIDI to the plug-in
// instrument the rack loaded for it, its stereo output as the track's. With no
// plug-in in place yet (still loading, or none chosen) the track is silent.
// The sequencer's accent goes out as the notes' velocity, and a Slide or Tie
// step as CC 84 (portamento control: the key the next note goes on from), a
// tie with the legato pedal (CC 68) held over it, as Melodee's own synths
// (Melodee Poly, PD and Chip) take them. Its three performance knobs (engine/macros.h) run on what the plug-in
// plays, before the track's level, pan and chain.
namespace melodee {

class PluginEngine final : public Engine {
public:
    // Audio thread: the plug-in this track plays now (nullptr: none).
    void attach(PluginRunner* plugin) {
        if (plugin == runner) return;
        if (runner) runner->silence();
        runner = plugin;
    }
    PluginRunner* attached() const { return runner; }

    void note_on(int key, int velocity) override {
        int heard = accent != 1 ? int(std::lround(float(velocity) * accent)) : velocity;
        if (runner) runner->midi(0x90, uint8_t(std::clamp(key, 0, 127)), uint8_t(std::clamp(heard, 1, 127)));
    }
    void set_accent(float gain) override { accent = gain; }
    void slide(int from, int to, int velocity, bool glide) override {
        if (!runner || from == to) return;
        if (!glide) runner->midi(0xb0, 68, 127); // a tie: legato, no glide
        runner->midi(0xb0, 84, uint8_t(std::clamp(from, 0, 127)));
        note_on(to, velocity);
        note_off(from);
        if (!glide) runner->midi(0xb0, 68, 0);
    }
    void note_off(int key) override {
        if (runner) runner->midi(0x80, uint8_t(std::clamp(key, 0, 127)), 0);
    }
    void all_notes_off() override {
        if (!runner) return;
        runner->midi(0xb0, 64, 0);  // sustain up
        runner->midi(0xb0, 123, 0); // all notes off
    }
    void silence() override { if (runner) runner->silence(); }
    // The track's parameters are its performance knobs (plugin_track); a plug-in's own are the rack's to set.
    void set_param(int id, int value) override {
        if (id < 0 || id >= plugin_track::Count) return;
        stored[size_t(id)] = value;
        unsigned slot = unsigned(id) % macro_slots;
        macros.set(slot, macro_kind(stored[plugin_track::Kind1 + slot], slot), stored[plugin_track::Macro1 + slot]);
    }
    void set_pool(DelayPool* pool) { macros.set_pool(pool); }
    void release_macros() { macros.release(); } // the track plays something else now
    void set_wheel(float amount) override {
        if (runner) runner->midi(0xb0, 1, uint8_t(std::lround(std::clamp(amount, 0.0f, 1.0f) * 127)));
    }
    void set_bend(float amount) override {
        if (!runner) return;
        int value = std::clamp(int(std::lround((amount + 1) * 8192)), 0, 16383);
        runner->midi(0xe0, uint8_t(value & 0x7f), uint8_t(value >> 7));
    }
    void control(int cc, int value) override {
        if (runner) runner->midi(0xb0, uint8_t(std::clamp(cc, 0, 127)), uint8_t(std::clamp(value, 0, 127)));
    }
    void set_pressure(int key, float amount) override {
        if (runner) runner->midi(0xa0, uint8_t(std::clamp(key, 0, 127)), uint8_t(std::lround(std::clamp(amount, 0.0f, 1.0f) * 127)));
    }
    void render(float* out, unsigned frames) override { // not used: the engine is stereo
        float left[256] {}, right[256] {};
        frames = std::min(frames, 256u);
        render_stereo(left, right, frames);
        for (unsigned f = 0; f < frames; ++f) out[f] += 0.5f * (left[f] + right[f]);
    }
    bool stereo() const override { return true; }
    void render_stereo(float* left, float* right, unsigned frames) override {
        if (runner) runner->render(left, right, frames);
        macros.process(left, right, frames);
    }
    // A plug-in's tail is its own business: while one is attached it is heard (and while a knob's echo or reverb rings).
    bool sounding() const override { return runner != nullptr || macros.sounding(); }

private:
    PluginRunner* runner = nullptr;
    float accent = 1; // for the notes started next (set_accent)
    MacroStage macros;
    std::array<int, plugin_track::Count> stored {};
};

} // namespace melodee
