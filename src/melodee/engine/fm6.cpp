// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/fm6.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace fm6 {
namespace {
const char* const algorithm_names[] {"1 Stack", "2 Two stacks", "3 Three and three", "4 Three pairs", "5 Stack and two",
                                     "6 One to five", "7 Pair and four", "8 All heard"};
// into[n]: the operators that modulate op n+1 (bit k: op k+1).
const Routing routings[algorithms] {
    {0b000001, {0b000010, 0b000100, 0b001000, 0b010000, 0b100000, 0}},           // 6>5>4>3>2>1
    {0b000101, {0b000010, 0, 0b001000, 0b010000, 0b100000, 0}},                  // 2>1, 6>5>4>3
    {0b001001, {0b000010, 0b000100, 0, 0b010000, 0b100000, 0}},                  // 3>2>1, 6>5>4
    {0b010101, {0b000010, 0, 0b001000, 0, 0b100000, 0}},                         // 2>1, 4>3, 6>5
    {0b000111, {0, 0, 0b001000, 0b010000, 0b100000, 0}},                         // 1, 2 and 6>5>4>3
    {0b011111, {0b100000, 0b100000, 0b100000, 0b100000, 0b100000, 0}},           // 6 into all of 1-5
    {0b111101, {0b000010, 0, 0, 0, 0, 0}},                                       // 2>1, 3, 4, 5, 6
    {0b111111, {0, 0, 0, 0, 0, 0}},                                              // six sines heard
};
const char* const algorithm_choices[] {"1", "2", "3", "4", "5", "6", "7", "8"};

constexpr ParamInfo P(const char* label, int min, int max, int initial, Format format = Format::Number, const char* const* choices = nullptr) {
    return {label, int16_t(min), int16_t(max), int16_t(initial), format, choices};
}
// The operators share labels; the page title says which operator.
const std::array<ParamInfo, Count> params = [] {
    std::array<ParamInfo, Count> out {};
    out[Algorithm] = P("ALGO", 0, 7, 4, Format::Choice, algorithm_choices);
    out[Feedback] = P("FEEDBACK", 0, 7, 3);
    out[Volume] = P("VOLUME", 0, 100, 80, Format::Percent);
    for (unsigned op = 0; op < ops; ++op) {
        bool carrier = op == 0 || op == 2 || op == 4;
        out[size_t(op_param(op, Ratio))] = P("RATIO", 0, 31, 2);
        out[size_t(op_param(op, Fine))] = P("FINE", 0, 99, 0);
        out[size_t(op_param(op, Level))] = P(op == 1 ? "OP LEVEL" : "LEVEL", 0, 99, carrier ? 99 : 70);
        out[size_t(op_param(op, Attack))] = P("ATTACK", 0, 100, 0, Format::Time);
        out[size_t(op_param(op, Decay))] = P("DECAY", 0, 100, carrier ? 75 : 55, Format::Time);
        out[size_t(op_param(op, Sustain))] = P("SUSTAIN", 0, 100, carrier ? 70 : 40, Format::Percent);
        out[size_t(op_param(op, Release))] = P("RELEASE", 0, 100, 40, Format::Time);
        out[size_t(op_param(op, Velocity))] = P("VELOCITY", 0, 100, 40, Format::Percent);
    }
    return out;
}();

constexpr int8_t _ = -1;
constexpr EditPage op_page(const char* title, const char* key, unsigned op) {
    return {title, key, Picture::Bars,
            {{{int8_t(op_param(op, Ratio)), int8_t(op_param(op, Fine)), int8_t(op_param(op, Level))},
              {int8_t(op_param(op, Attack)), int8_t(op_param(op, Decay)), int8_t(op_param(op, Sustain))},
              {int8_t(op_param(op, Release)), int8_t(op_param(op, Velocity)), _}}}};
}
const EditPage pages[] {
    {"Voice", "VOICE", Picture::Bars, {{{Algorithm, Feedback, Volume}, {_, _, _}, {_, _, _}}}},
    op_page("Operator 1", "OP 1", 0), op_page("Operator 2", "OP 2", 1), op_page("Operator 3", "OP 3", 2),
    op_page("Operator 4", "OP 4", 3), op_page("Operator 5", "OP 5", 4), op_page("Operator 6", "OP 6", 5),
};
#define OP(n, k) int16_t(op_param(n, k))
const Sound sounds[] {
    {"INIT VOICE", {{{-1, 0}}}},
    {"TINE EP", {{{Algorithm, 3}, {Feedback, 2}, {OP(1, Ratio), 14}, {OP(1, Level), 62}, {OP(1, Decay), 40}, {OP(1, Sustain), 10},
                 {OP(0, Decay), 80}, {OP(0, Sustain), 30}, {OP(3, Ratio), 2}, {OP(3, Level), 55}, {OP(2, Level), 80}, {OP(4, Level), 0}, {-1, 0}}}},
    {"GLASS BELL", {{{Algorithm, 3}, {OP(1, Ratio), 7}, {OP(1, Fine), 41}, {OP(1, Level), 78}, {OP(0, Decay), 90}, {OP(0, Sustain), 0},
                    {OP(1, Decay), 80}, {OP(1, Sustain), 0}, {OP(0, Release), 70}, {OP(3, Ratio), 11}, {OP(3, Level), 60}, {OP(4, Level), 0}, {-1, 0}}}},
    {"ROUND BASS", {{{Algorithm, 0}, {Feedback, 4}, {OP(0, Ratio), 1}, {OP(1, Ratio), 1}, {OP(1, Level), 72}, {OP(1, Decay), 45},
                    {OP(1, Sustain), 20}, {OP(2, Level), 50}, {OP(3, Level), 0}, {OP(4, Level), 0}, {OP(5, Level), 0}, {-1, 0}}}},
    {"BRASS SECT", {{{Algorithm, 1}, {Feedback, 5}, {OP(1, Attack), 30}, {OP(1, Level), 80}, {OP(0, Attack), 20}, {OP(0, Sustain), 85},
                    {OP(3, Level), 75}, {OP(3, Attack), 25}, {OP(2, Level), 85}, {-1, 0}}}},
    {"SOFT PAD", {{{Algorithm, 6}, {OP(0, Attack), 65}, {OP(0, Release), 70}, {OP(0, Sustain), 90}, {OP(2, Attack), 70}, {OP(2, Ratio), 3},
                  {OP(2, Fine), 2}, {OP(2, Level), 70}, {OP(3, Level), 50}, {OP(3, Ratio), 4}, {OP(1, Level), 40}, {OP(4, Level), 0}, {OP(5, Level), 0}, {-1, 0}}}},
    {"MARIMBA", {{{Algorithm, 3}, {OP(1, Ratio), 8}, {OP(1, Level), 70}, {OP(1, Decay), 25}, {OP(1, Sustain), 0}, {OP(0, Decay), 50},
                 {OP(0, Sustain), 0}, {OP(0, Release), 30}, {OP(2, Level), 0}, {OP(4, Level), 0}, {-1, 0}}}},
    {"FM ORGAN", {{{Algorithm, 7}, {OP(0, Ratio), 1}, {OP(1, Ratio), 2}, {OP(2, Ratio), 4}, {OP(3, Ratio), 6}, {OP(4, Ratio), 8},
                  {OP(5, Ratio), 10}, {OP(1, Level), 80}, {OP(3, Level), 60}, {OP(5, Level), 50}, {OP(0, Sustain), 100}, {-1, 0}}}},
    {"DIGI LEAD", {{{Algorithm, 1}, {Feedback, 7}, {OP(1, Ratio), 3}, {OP(1, Level), 75}, {OP(3, Ratio), 2}, {OP(3, Level), 70},
                   {OP(0, Sustain), 90}, {-1, 0}}}},
};
#undef OP
} // namespace

const char* algorithm_name(unsigned a) { return algorithm_names[a % algorithms]; }
const Routing& routing(unsigned a) { return routings[a % algorithms]; }
// Ratio steps: 0.5, then 1 to 31.
float ratio_of(int value) { return value <= 0 ? 0.5f : float(value); }

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Fm6, "FM6", true, params.data(), Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {Algorithm, Feedback, int8_t(op_param(1, Level))}, sounds, sizeof(sounds) / sizeof(sounds[0])};
    return engine;
}
} // namespace fm6

using namespace fm6;
namespace {
// A sine table with linear reading: six operators on eight voices is too many std::sin calls.
struct SineTable {
    static constexpr unsigned size = 4096;
    std::array<float, size + 1> v {};
    SineTable() { for (unsigned n = 0; n <= size; ++n) v[n] = std::sin(2 * dsp::pi * n / size); }
    float operator()(float phase) const { // phase in turns, any value
        phase -= std::floor(phase);
        float at = phase * size;
        unsigned i = unsigned(at);
        return v[i] + (v[i + 1] - v[i]) * (at - float(i));
    }
};
const SineTable sine;
}

Fm6Engine::Fm6Engine() { for (unsigned n = 0; n < Count; ++n) p[n] = info().params[n].initial; update(); }

void Fm6Engine::set_param(int id, int value) {
    if (id < 0 || id >= Count) return;
    p[size_t(id)] = std::clamp<int>(value, info().params[id].min, info().params[id].max);
    dirty = true;
}

void Fm6Engine::update() {
    dirty = false;
    for (unsigned op = 0; op < ops; ++op) {
        using dsp::Envelope;
        shapes[op] = {Envelope::coefficient(time_seconds(p[size_t(op_param(op, Attack))])),
                      Envelope::coefficient(time_seconds(p[size_t(op_param(op, Decay))])),
                      Envelope::coefficient(time_seconds(p[size_t(op_param(op, Release))])), p[size_t(op_param(op, Sustain))] / 100.0f};
        ratios[op] = ratio_of(p[size_t(op_param(op, Ratio))]) * (1 + p[size_t(op_param(op, Fine))] / 100.0f);
        float l = p[size_t(op_param(op, Level))] / 99.0f;
        levels[op] = l * l;
    }
}

void Fm6Engine::note_on(int key, int velocity) {
    if (dirty) update();
    Voice* v = nullptr;
    for (auto& x : voices) if (x.key == key) { v = &x; break; }
    if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
    if (!v) { v = &voices[0]; for (auto& x : voices) if (x.age < v->age) v = &x; }
    *v = Voice {};
    v->key = key; v->held = true; v->age = ++clock; v->velocity = velocity / 127.0f;
    for (auto& e : v->env) e.gate(true);
}
void Fm6Engine::note_off(int key) { for (auto& v : voices) if (v.key == key && v.held) { v.held = false; for (auto& e : v.env) e.gate(false); } }
void Fm6Engine::all_notes_off() { for (auto& v : voices) if (v.held) { v.held = false; for (auto& e : v.env) e.gate(false); } }
void Fm6Engine::silence() { for (auto& v : voices) v = Voice {}; }
bool Fm6Engine::sounding() const { for (auto& v : voices) if (v.key >= 0) return true; return false; }

void Fm6Engine::render(float* out, unsigned frames) {
    if (dirty) update();
    const auto& r = routing(unsigned(p[Algorithm]));
    float feedback = p[Feedback] ? std::exp2(float(p[Feedback]) - 7) * 0.8f : 0;
    unsigned carriers = 0;
    for (unsigned op = 0; op < ops; ++op) carriers += (r.carriers >> op) & 1;
    float volume = p[Volume] / 100.0f * 0.5f / std::sqrt(float(std::max(carriers, 1u)));
    for (auto& v : voices) {
        if (v.key < 0) continue;
        float hz = dsp::midi_hz(v.key + bend * 2) / dsp::sample_rate;
        std::array<float, ops> gain {};
        for (unsigned op = 0; op < ops; ++op) {
            float sens = p[size_t(op_param(op, Velocity))] / 100.0f;
            gain[op] = levels[op] * (1 - sens + sens * v.velocity);
        }
        for (unsigned f = 0; f < frames; ++f) {
            float heard = 0;
            // Operators 6 down to 1, so a modulator is ready before what it modulates.
            for (int op = int(ops) - 1; op >= 0; --op) {
                float mod = 0;
                for (unsigned k = 0; k < ops; ++k) if ((r.into[size_t(op)] >> k) & 1) mod += v.out[k];
                if (op == 5) mod += v.feedback * feedback;
                float e = v.env[size_t(op)].next(shapes[size_t(op)]);
                float s = sine(v.phase[size_t(op)] + mod * 0.6f);
                v.phase[size_t(op)] += hz * ratios[size_t(op)];
                if (v.phase[size_t(op)] > 1) v.phase[size_t(op)] -= 1;
                float o = s * e * gain[size_t(op)];
                if (op == 5) v.feedback = (v.feedback + o) * 0.5f; // averaged, as feedback FM wants
                v.out[size_t(op)] = (r.carriers >> op) & 1 ? o : o * (2 + wheel * 2); // a modulator turns in radians (x 2 pi in sine())
                if ((r.carriers >> op) & 1) heard += o;
            }
            out[f] += heard * volume;
        }
        bool done = true;
        for (unsigned op = 0; op < ops; ++op) if (((r.carriers >> op) & 1) && v.env[op].active()) done = false;
        if (done && !v.held) v.key = -1;
    }
}

} // namespace melodee
