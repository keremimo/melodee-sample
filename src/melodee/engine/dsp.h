// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numeric>

// Small realtime-safe DSP pieces for Melodee's engines, written for
// sample-base: band-limited oscillators, a four-pole ladder filter, ADSR
// envelopes, an LFO and noise. No allocation, no locks, no I/O.
namespace melodee::dsp {

// The audio device's rate, 44.1, 48 or 96 kHz (Settings' SAMPLE RATE). The
// audio thread sets it (EngineHost::set_audio_format) before it renders at a
// new rate; what the engines keep worked out from it is rebuilt then.
inline std::atomic<float> device_rate {44100.0f};
inline float sample_rate() { return device_rate.load(std::memory_order_relaxed); }
// Samples, takes, renders and every frame count a project keeps (a pad's
// start, a slice, a take's length) are at 44.1 kHz whatever the device runs,
// so a project sounds and opens the same at any rate.
constexpr float model_rate = 44100.0f;
// The highest device rate: delay memory made up front is sized for it.
constexpr float max_rate = 96000.0f;
constexpr float pi = 3.14159265358979f;

// Constants written a 44.1 kHz frame at a time (a fall, a one-pole's step, a count of frames, a chance), made the
// device's: the same time, corner and density at any rate, and at 44.1 kHz the constant itself, bit for bit.
// Worked out where the rate is known (an update, a note, a block), never a frame at a time.
inline float model_frames() { return model_rate / sample_rate(); } // 44.1 kHz frames in one device frame (1 at 44.1 kHz)
inline float device_frames() { return sample_rate() / model_rate; } // device frames in one 44.1 kHz frame
// x *= k each 44.1 kHz frame: the k for each device frame.
inline float fall_per_frame(float k) { const float r = model_frames(); return r == 1.0f ? k : std::pow(k, r); }
// x += (aim - x) * k each 44.1 kHz frame: the k for each device frame.
inline float step_per_frame(float k) { const float r = model_frames(); return r == 1.0f ? k : 1.0f - std::pow(1.0f - k, r); }
// x *= k each 128-frame block at 44.1 kHz (a meter's fall): the k for a block of `frames` device frames.
inline float fall_per_block(float k, unsigned frames) {
    const float r = float(frames) / 128.0f * model_frames();
    return r == 1.0f ? k : std::pow(k, r);
}
// White noise a frame at a time is spread over the whole band, so at 96 kHz less of it is heard: its level
// for the same hiss in the audible band (the square root of the rates' ratio).
inline float noise_level() { return std::sqrt(device_frames()); }
// A one-sample click: its level for the same sound in the audible band.
inline float click_level() { return device_frames(); }

// The rates' ratio as whole numbers: 44.1 kHz frames to device frames, reduced (1/1, 147/160, 147/320).
struct RateRatio { uint32_t model = 1, device = 1; };
inline RateRatio rate_ratio() {
    const uint32_t a = uint32_t(model_rate), b = std::max(uint32_t(sample_rate() + 0.5f), 1u), g = std::gcd(a, b);
    return {a / g, b / g};
}
// Something done every `every` 44.1 kHz frames, on device frame `frame` of a count: at 44.1 kHz frame % every == 0,
// at any rate the frames where another `every` 44.1 kHz frames have gone by.
inline bool every_model_frames(uint64_t frame, uint32_t every, RateRatio r) {
    return frame * r.model % (uint64_t(every) * r.device) < r.model;
}
// A chance taken each frame, written for 44.1 kHz frames, coming as often in time at any rate: with `keep`
// (model_frames()) under 1, a further draw from `seed` keeps that share of what came. At 44.1 kHz, no draw.
inline bool kept(uint32_t& seed, float keep) {
    if (keep >= 1.0f) return true;
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) * (1.0f / 16777216.0f) < keep;
}

inline float midi_hz(float key) { return 440.0f * std::exp2((key - 69.0f) / 12.0f); }

// A cheap tanh, accurate to a few percent, for soft saturation.
inline float soft_clip(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// PolyBLEP residual for a discontinuity at phase 0, `dt` the phase step.
inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// One oscillator: band-limited saw and pulse, a triangle, any mix of them.
// `wrapped` tells the caller when the phase passed 1 (for hard sync).
struct Oscillator {
    float phase = 0;
    struct Mix { float saw = 0, pulse = 0, tri = 0, width = 0.5f; };
    float next(float dt, const Mix& m, bool& wrapped) {
        float out = 0;
        if (m.saw != 0) out += m.saw * (2.0f * phase - 1.0f - blep(phase, dt));
        if (m.pulse != 0) {
            float p2 = phase + 1.0f - m.width; p2 -= std::floor(p2);
            float sq = (phase < m.width ? 1.0f : -1.0f) + blep(phase, dt) - blep(p2, dt);
            out += m.pulse * sq;
        }
        if (m.tri != 0) out += m.tri * (1.0f - 4.0f * std::fabs(phase - 0.5f));
        phase += dt;
        wrapped = phase >= 1.0f;
        if (wrapped) phase -= 1.0f;
        return out;
    }
    void reset(float to = 0) { phase = to; }
};

// A four-pole lowpass ladder with zero-delay feedback (topology-preserving
// one-poles) and saturation in the feedback path. Set the coefficient at
// control rate with tune(); process() per sample.
struct Ladder {
    float s[4] {};
    float g = 0.1f, G = 0.09f, k = 0;
    void tune(float hz, float resonance /* 0..1 */) {
        hz = std::clamp(hz, 16.0f, sample_rate() * 0.45f);
        g = std::tan(pi * hz / sample_rate());
        G = g / (1.0f + g);
        k = 3.95f * std::clamp(resonance, 0.0f, 1.0f);
    }
    float process(float x) {
        float g2 = G * G, g4 = g2 * g2, d = 1.0f / (1.0f + g);
        float sigma = G * g2 * s[0] * d + g2 * s[1] * d + G * s[2] * d + s[3] * d;
        float y4 = (g4 * x + sigma) / (1.0f + k * g4);
        float u = soft_clip(x - k * y4) * (1.0f + 0.5f * k * 0.25f);
        for (auto& state : s) {
            float v = (u - state) * G;
            float y = v + state;
            state = y + v;
            u = y;
        }
        return u;
    }
    void reset() { s[0] = s[1] = s[2] = s[3] = 0; }
};

// An analogue-style ADSR: exponential segments toward overshooting targets,
// so attacks are near-linear and decays and releases curve.
struct Envelope {
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Stage::Idle;
    float level = 0;
    bool hurried = false; // the overload guard's (roadmap P2): released in about 30 ms, whatever its own release
    struct Shape { float attack = 0.999f, decay = 0.999f, release = 0.999f, sustain = 1; };
    static float coefficient(float seconds) { return std::exp(-1.0f / std::max(seconds * sample_rate(), 1.0f) * 3.0f); }
    void gate(bool on) {
        if (on) hurried = false;
        stage = on ? Stage::Attack : (stage == Stage::Idle ? Stage::Idle : Stage::Release);
    }
    void hurry() { if (stage != Stage::Idle) { stage = Stage::Release; hurried = true; } }
    float next(const Shape& s) {
        switch (stage) {
        case Stage::Attack:
            level = 1.25f + (level - 1.25f) * s.attack;
            if (level >= 1.0f) { level = 1.0f; stage = Stage::Decay; }
            break;
        case Stage::Decay:
            level = s.sustain + (level - s.sustain) * s.decay;
            if (std::fabs(level - s.sustain) < 1e-4f) stage = Stage::Sustain;
            break;
        case Stage::Sustain: level = s.sustain; break;
        case Stage::Release:
            level = level * (hurried ? 0.993f : s.release); // hurried: -40 dB in about 1300 frames
            if (level < 1e-4f) { level = 0; stage = Stage::Idle; }
            break;
        case Stage::Idle: level = 0; break;
        }
        return level;
    }
    bool active() const { return stage != Stage::Idle; }
};

// A low-frequency oscillator: triangle, saw, square or a random step.
struct Lfo {
    float phase = 0, held = 0;
    uint32_t seed = 22222;
    float next(float hz, unsigned shape, float seconds) {
        phase += hz * seconds;
        bool wrapped = phase >= 1.0f;
        phase -= std::floor(phase);
        switch (shape) {
        case 1: return 2.0f * phase - 1.0f;
        case 2: return phase < 0.5f ? 1.0f : -1.0f;
        case 3:
            if (wrapped) { seed = seed * 1664525u + 1013904223u; held = int32_t(seed) * (1.0f / 2147483648.0f); }
            return held;
        default: return 1.0f - 4.0f * std::fabs(phase - 0.5f);
        }
    }
};

// White noise from a linear congruential generator.
struct Noise {
    uint32_t seed = 12345;
    float next() { seed = seed * 1664525u + 1013904223u; return int32_t(seed) * (1.0f / 2147483648.0f); }
};

// A one-pole smoother for control values.
struct Smooth {
    float value = 0;
    float next(float target, float amount) { value += (target - value) * amount; return value; }
};

} // namespace melodee::dsp
