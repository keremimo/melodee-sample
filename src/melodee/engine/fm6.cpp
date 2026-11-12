// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/fm6.h"
#include "melodee/engine/output_trim.h"
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
        out[size_t(op_param(op, Velocity))] = P("VELOCITY", 0, 100, 70, Format::Percent);
    }
    out[AtVib] = P("AT > VIB", 0, 100, expression::default_at_vib, Format::Percent);
    out[AtBright] = P("AT > BRIGHT", 0, 100, expression::default_at_bright, Format::Percent);
    out[LfoRate] = P("LFO RATE", 0, 100, expression::default_vib_rate, Format::Rate);
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
    // Pad pressure (velocity is each operator's own); the LFO is also the matrix's.
    {"Touch", "TOUCH", Picture::Bars, {{{AtVib, AtBright, LfoRate}, {_, _, _}, {_, _, _}}}},
};
// The mod matrix on FM6: brightness scales the modulators (2 octaves of
// index at full), feedback adds to op 6's, carrier and modulator pitch apart.
const char* const mod_dests[] {"None", "Brightness", "Feedback", "Amp level", "Pitch", "Carrier pitch", "Mod pitch",
                               nullptr, "LFO rate", nullptr};
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
// Stage: B1 SOUND (preset, algorithm and feedback, mix), B2 SHAPE (the first two operators' envelopes, touch),
// B3 PLAY (arp, the first three ratios with the last three on Shift, effects).
using K = StageKind;
using S = StagePicture;
constexpr int8_t op(unsigned n, OpParam k) { return int8_t(op_param(n, k)); }
const StageGroups stage {{{
    {{{stage_preset, {"TONE", K::Params, S::Bars, {{Algorithm, Feedback, op(1, Level)}}}, stage_mix, stage_sends}}, 4},
    {{{{"OP 1 ENV", K::Params, S::Envelope, {{op(0, Attack), op(0, Decay), op(0, Sustain)}}, {{op(0, Release), op(0, Velocity), op(0, Level)}}},
       {"OP 2 ENV", K::Params, S::Envelope, {{op(1, Attack), op(1, Decay), op(1, Sustain)}}, {{op(1, Release), op(1, Velocity), op(1, Level)}}},
       {"TOUCH", K::Params, S::Bars, {{AtVib, AtBright, LfoRate}}}}}, 3},
    {{{stage_arp, stage_chord, {"RATIOS", K::Params, S::ShiftRows, {{op(0, Ratio), op(1, Ratio), op(2, Ratio)}}, {{op(3, Ratio), op(4, Ratio), op(5, Ratio)}}}, stage_fx}}, 4},
}}};
} // namespace

const char* algorithm_name(unsigned a) { return algorithm_names[a % algorithms]; }
const Routing& routing(unsigned a) { return routings[a % algorithms]; }
// Ratio steps: 0.5, then 1 to 31.
float ratio_of(int value) { return value <= 0 ? 0.5f : float(value); }

const EngineInfo& info() {
    static const EngineInfo engine {EngineKind::Fm6, "FM6", true, params.data(), Count, pages, sizeof(pages) / sizeof(pages[0]),
                                    {Algorithm, Feedback, int8_t(op_param(1, Level))}, sounds, sizeof(sounds) / sizeof(sounds[0]),
                                    mod_dests, legacy_count, true, &stage};
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
    step.restart();
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
    step.restart();
    if (dirty) update();
    Voice* v = nullptr;
    for (auto& x : voices) if (x.key == key) { v = &x; break; }
    if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
    if (!v) { v = &voices[0]; for (auto& x : voices) if (x.age < v->age) v = &x; }
    // A voice still sounding (the same key again, or the oldest taken) goes on from where it is: its operators
    // keep turning and each envelope attacks from its level now, so what it played never stops dead (a click).
    if (v->key < 0) *v = Voice {};
    v->key = key; v->held = true; v->age = ++clock; v->velocity = velocity / 127.0f; v->accent = accent;
    v->slide = 0; v->pressure = v->touch = 0;
    v->random = noise.next();
    for (auto& e : v->env) e.gate(true);
}

// Slide or Tie from the sequencer (Engine::slide): the held voice of `from` goes on as `to`.
void Fm6Engine::slide(int from, int to, int velocity, bool glide) {
    step.restart();
    bool found = false;
    for (auto& v : voices)
        if (v.key == from && v.held) {
            if (glide && from != to) v.slide += float(from - to);
            v.key = to; v.age = ++clock; v.accent = accent;
            found = true;
        }
    if (!found) note_on(to, velocity);
}
void Fm6Engine::set_pressure(int key, float value) { step.restart(); for (auto& v : voices) if (v.key == key && v.held) v.pressure = value; }
void Fm6Engine::note_off(int key) { step.restart(); for (auto& v : voices) if (v.key == key && v.held) { v.held = false; for (auto& e : v.env) e.gate(false); } }
void Fm6Engine::all_notes_off() { step.restart(); for (auto& v : voices) if (v.held) { v.held = false; for (auto& e : v.env) e.gate(false); } }
void Fm6Engine::silence() { step.restart(); for (auto& v : voices) v = Voice {}; }
bool Fm6Engine::sounding() const { for (auto& v : voices) if (v.key >= 0) return true; return false; }

// The overload guard (Engine::relieve): tails hurried; with `steal`, the oldest held voice let go and hurried too.
bool Fm6Engine::relieve(bool steal) {
    step.restart();
    Voice* oldest = nullptr;
    for (auto& v : voices) {
        if (v.key < 0) continue;
        if (!v.held) { for (auto& e : v.env) e.hurry(); continue; }
        if (!oldest || v.age < oldest->age) oldest = &v;
    }
    if (!steal || !oldest) return false;
    oldest->held = false; for (auto& e : oldest->env) e.hurry();
    return true;
}

void Fm6Engine::render(float* out, unsigned frames) {
    if (dirty) update();
    using expression::at;
    const auto& r = routing(unsigned(p[Algorithm]));
    unsigned carriers = 0;
    for (unsigned op = 0; op < ops; ++op) carriers += (r.carriers >> op) & 1;
    float volume = p[Volume] / 100.0f * 0.5f * output_trim::fm6 / std::sqrt(float(std::max(carriers, 1u)));
    for (unsigned start = 0; start < frames;) {
        // A control step on its own count of frames (engine.h ControlStep), not from this call's start.
        if (step.starts(control_frames)) {
            unsigned moved = step.take();
            lfo_value = lfo.next(rate_hertz(p[LfoRate]) * std::exp2(expression::lfo_rate_octaves(mods, wheel, lfo_value)), moved);
            for (auto& v : voices) {
                if (v.key < 0) continue;
                expression::follow(v.touch, v.pressure, moved);
                auto m = expression::sums(mods, {lfo_value, v.velocity, v.touch, wheel, (v.key - 60.0f) / 60.0f, v.random, v.env[0].level});
                // Pitch: a slide's way in, the bend, the matrix and pressure's vibrato; carriers and modulators can part.
                v.slide = expression::slide_settle(v.slide, moved);
                float key = v.key + v.slide + bend * 2 + at(m, ModDest::Pitch) * 12 + lfo_value * expression::vibrato_semitones(v.touch, p[AtVib]);
                float hz_carrier = dsp::midi_hz(key + at(m, ModDest::OscAPitch) * 12) / dsp::sample_rate();
                float hz_modulator = dsp::midi_hz(key + at(m, ModDest::OscBPitch) * 12) / dsp::sample_rate();
                // Brightness: pressure and the matrix scale every modulator, as a DX patch's aftertouch into modulator levels.
                float bright = std::min(8.0f, std::exp2(2 * (at(m, ModDest::Cutoff) + v.touch * p[AtBright] / 100.0f)));
                float steps = std::clamp(p[Feedback] + at(m, ModDest::Resonance) * 7, 0.0f, 7.0f);
                v.feedback_amount = steps > 0 ? std::exp2(steps - 7) * 0.8f : 0;
                float amp = std::max(0.0f, 1 + at(m, ModDest::AmpLevel)) * v.accent;
                for (unsigned op = 0; op < ops; ++op) {
                    bool carrier = (r.carriers >> op) & 1;
                    float level = levels[op] * expression::velocity_gain(v.velocity, p[size_t(op_param(op, Velocity))] / 100.0f);
                    if (op == 5) v.self = level; // op 6's level into its own feedback
                    v.gain[op] = level * (carrier ? amp : bright * (2 + wheel * 2)); // a modulator turns in radians (x 2 pi in sine())
                    v.phase_step[op] = (carrier ? hz_carrier : hz_modulator) * ratios[op];
                }
            }
        }
        unsigned n = step.run(frames - start);
        for (auto& v : voices) {
            if (v.key < 0) continue;
            const float feedback = v.feedback_amount, self = v.self;
            for (unsigned f = 0; f < n; ++f) {
                float heard = 0;
                // Operators 6 down to 1, so a modulator is ready before what it modulates.
                for (int op = int(ops) - 1; op >= 0; --op) {
                    float mod = 0;
                    for (unsigned k = 0; k < ops; ++k) if ((r.into[size_t(op)] >> k) & 1) mod += v.out[k];
                    if (op == 5) mod += v.feedback * feedback;
                    float e = v.env[size_t(op)].next(shapes[size_t(op)]);
                    float s = sine(v.phase[size_t(op)] + mod * 0.6f);
                    v.phase[size_t(op)] += v.phase_step[size_t(op)];
                    if (v.phase[size_t(op)] > 1) v.phase[size_t(op)] -= 1;
                    float o = s * e * v.gain[size_t(op)];
                    if (op == 5) v.feedback = (v.feedback + s * e * self) * 0.5f; // averaged, as feedback FM wants
                    v.out[size_t(op)] = o;
                    if ((r.carriers >> op) & 1) heard += o;
                }
                out[start + f] += heard * volume;
            }
        }
        if (step.played(n)) // let go once a step, when every carrier has ended
            for (auto& v : voices) {
                if (v.key < 0) continue;
                bool done = true;
                for (unsigned op = 0; op < ops; ++op) if (((r.carriers >> op) & 1) && v.env[op].active()) done = false;
                if (done && !v.held) v.key = -1;
            }
        start += n;
    }
}

} // namespace melodee
