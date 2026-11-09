// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/sampler.h"
#include <array>

// SAMPLER: a kit of 64 pads, each a region of a sample, written for
// sample-base after the melodee-ios sampler's behaviour: gate or one shot,
// reverse, mute groups (pads in a group cut each other), its own pitch,
// level, pan and envelope, and one filter on the whole kit. Notes 36..99
// play pads 1..64 (bank A first). Stereo. Audio thread only; the sample
// table belongs to the engine host.
namespace melodee {

namespace sampler { enum Param : int8_t { Tune, Filter, Resonance, Decay, Level, Count }; }

class SamplerEngine final : public Engine {
public:
    static constexpr unsigned max_voices = 16;
    using Table = std::array<const SampleData*, sample_slots>;

    SamplerEngine();
    void set_samples(const Table* table) { samples = table; }
    void set_pad(unsigned pad, const PadSound& sound) { if (pad < sampler_pads) pads[pad] = sound; }
    // A region of a sample on no pad, for Chop's audition (plays one shot).
    void audition(uint16_t sample, uint32_t start, uint32_t end, int velocity);
    // A sample is about to change: whatever plays it stops now.
    void sample_gone(uint16_t sample);

    void note_on(int key, int velocity) override;
    void note_off(int key) override;
    void all_notes_off() override;
    void silence() override;
    void set_param(int id, int value) override;
    void render(float* out, unsigned frames) override; // mono fold, for callers that want one channel
    bool stereo() const override { return true; }
    void render_stereo(float* left, float* right, unsigned frames) override;
    bool sounding() const override;

private:
    struct Voice {
        int pad = -1; // -1 free; sampler_pads: the audition voice's region
        const SampleData* sample = nullptr;
        uint16_t slot = no_sample;
        double position = 0, step = 1;
        uint32_t start = 0, end = 0;
        bool reverse = false, held = false, gate = false, choked = false;
        uint8_t group = 0;
        float gain = 0, left = 0.7071f, right = 0.7071f, fade = 1;
        uint32_t age = 0;
        dsp::Envelope env;
        dsp::Envelope::Shape shape;
    };
    void start(Voice& v, const PadSound& sound, int pad, int velocity);
    Voice& free_voice();

    const Table* samples = nullptr;
    std::array<PadSound, sampler_pads> pads {};
    std::array<Voice, max_voices> voices {};
    std::array<int, sampler::Count> p {};
    uint32_t clock = 0;
    float g = 1, k = 2, s1[2] {}, s2[2] {};
    bool filtered = false;
};

} // namespace melodee
