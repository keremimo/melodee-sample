// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/engine_host.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/engine_info.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
// Flush denormals to zero on this thread: decaying filters and envelopes
// would otherwise crawl through slow subnormal arithmetic.
void flush_denormals() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("msr fpcr, %0" ::"r"(fpcr | (uint64_t(1) << 24)));
#endif
}
float level_gain(int level) { float x = std::clamp(level, 0, 127) / 100.0f; return x * x; }
} // namespace

EngineHost::EngineHost() : patterns(std::make_unique<PatternBank>()) {
    // Build every engine's static tables now, not on the audio thread.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) (void)engine_info(EngineKind(k));
}

void EngineHost::load(const Project& project, const PatternBank& bank) {
    *patterns = bank;
    tempo = project.tempo; swing = project.swing;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = project.tracks[t];
        auto& slot = slots[t];
        select_engine(slot, track.engine);
        const auto& info = engine_info(track.engine);
        if (slot.engine) for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), track.params[p]);
        apply({Command::Type::Level, uint8_t(t), 0, 0, track.mix.level});
        apply({Command::Type::Pan, uint8_t(t), 0, 0, track.mix.pan});
        slot.send = track.mix.send / 100.0f;
        slot.mute = track.mix.mute; slot.solo = track.mix.solo;
        slot.pattern = track.pattern;
    }
}

void EngineHost::select_engine(Slot& slot, EngineKind kind) {
    if (slot.engine) slot.engine->silence();
    slot.kind = kind;
    slot.engine = kind == EngineKind::Prophet ? static_cast<Engine*>(&slot.prophet) : nullptr;
    for (auto& off : slot.offs) off.key = -1;
}

void EngineHost::apply(const Command& c) {
    using Type = Command::Type;
    auto& slot = slots[c.track % track_count];
    switch (c.type) {
    case Type::NoteOn: if (slot.engine) slot.engine->note_on(c.index, c.value); break;
    case Type::NoteOff: if (slot.engine) slot.engine->note_off(c.index); break;
    case Type::AllNotesOff:
        for (unsigned t = 0; t < track_count; ++t)
            if ((c.track == 255 || t == c.track) && slots[t].engine) {
                slots[t].engine->all_notes_off();
                for (auto& off : slots[t].offs) off.key = -1;
            }
        break;
    case Type::Engine: select_engine(slot, EngineKind(c.index)); break;
    case Type::Sound:
        if (slot.engine) {
            std::array<int16_t, max_params> values;
            const auto& info = engine_info(slot.kind);
            sound_values(info, c.index, values);
            for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), values[p]);
        }
        break;
    case Type::Param: if (slot.engine) slot.engine->set_param(c.index, c.value); break;
    case Type::Wheel: if (slot.engine) slot.engine->set_wheel(c.value / 1000.0f); break;
    case Type::Level: slot.gain = level_gain(c.value); break;
    case Type::Pan: {
        float angle = (std::clamp<int>(c.value, -50, 50) + 50) / 100.0f * dsp::pi / 2;
        slot.pan_left = std::cos(angle); slot.pan_right = std::sin(angle);
        break;
    }
    case Type::Send: slot.send = c.value / 100.0f; break;
    case Type::Mute: slot.mute = c.value != 0; break;
    case Type::Solo: slot.solo = c.value != 0; break;
    case Type::SetStep:
        if (c.index < max_steps) patterns->at[c.track % track_count][c.pattern % pattern_count].steps[c.index] = c.step;
        break;
    case Type::Length:
        patterns->at[c.track % track_count][c.pattern % pattern_count].length = uint16_t(std::clamp<int>(c.value, 1, max_steps));
        break;
    case Type::Pattern: slot.pattern = uint8_t(c.value % pattern_count); break;
    case Type::Play:
        if (!running) { running = true; position_now = 0; until_step = 0; }
        break;
    case Type::Stop:
        running = false;
        for (auto& s : slots) {
            for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
        }
        break;
    case Type::Tempo: tempo = uint16_t(std::clamp<int>(c.value, 300, 3000)); break;
    case Type::Swing: swing = uint8_t(std::clamp<int>(c.value, 50, 75)); break;
    }
}

void EngineHost::schedule_off(Slot& slot, int key, int32_t samples) {
    for (auto& off : slot.offs)
        if (off.key == key) { off.samples = samples; return; } // a retrigger moves its end
    for (auto& off : slot.offs)
        if (off.key < 0) { off.key = int16_t(key); off.samples = samples; return; }
    slot.engine->note_off(key); // no room: end it now rather than hang
}

void EngineHost::run_offs(Slot& slot, unsigned frames) {
    for (auto& off : slot.offs) {
        if (off.key < 0) continue;
        off.samples -= int32_t(frames);
        if (off.samples <= 0) { if (slot.engine) slot.engine->note_off(off.key); off.key = -1; }
    }
}

void EngineHost::step() {
    double length = samples_per_step();
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        if (!slot.engine) continue;
        const auto& pattern = patterns->at[t][slot.pattern];
        const auto& s = pattern.steps[position_now % pattern.length];
        if (s.empty()) continue;
        random = random * 1664525u + 1013904223u;
        if (s.chance < 100 && (random >> 8) % 100 >= s.chance) continue;
        // Held for its length, a touch short of the next step unless tied.
        auto samples = int32_t(length * s.length - (s.flags & step_tie ? 0 : length * 0.1));
        for (const auto& note : s.notes) {
            if (!note.velocity) continue;
            int velocity = s.flags & step_accent ? std::min(127, note.velocity + 24) : note.velocity;
            slot.engine->note_on(note.key, velocity);
            schedule_off(slot, note.key, std::max<int32_t>(samples, 64));
        }
    }
    ++position_now;
    // Swing delays every second sixteenth by up to half a step.
    double late = (swing - 50) / 50.0 * length * 0.5;
    until_step += position_now % 2 ? length + late : length - late;
    step_position.store(position_now - 1, std::memory_order_relaxed);
}

void EngineHost::render(int32_t* out, unsigned frames, float volume) {
    static thread_local bool flushed = false;
    if (!flushed) { flush_denormals(); flushed = true; }
    Command command;
    for (unsigned n = 0; n < 4096 && queue.pop(command); ++n) apply(command);
    frames = std::min(frames, block);
    bool solo = false;
    for (auto& slot : slots) { solo |= slot.solo; std::fill(slot.buffer, slot.buffer + frames, 0.0f); }

    // Render up to each step, play the step, carry on: steps land on the sample.
    unsigned done = 0;
    while (done < frames) {
        unsigned n = frames - done;
        if (running) {
            if (until_step <= 0) { step(); continue; }
            n = std::min<unsigned>(n, unsigned(std::ceil(until_step)));
        }
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            run_offs(slot, n);
            slot.engine->render(slot.buffer + done, n);
        }
        if (running) until_step -= n;
        done += n;
    }
    is_playing.store(running, std::memory_order_relaxed);

    float peak = 0;
    for (unsigned f = 0; f < frames; ++f) {
        float left = 0, right = 0;
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            float x = slot.buffer[f] * slot.gain;
            if (slot.mute || (solo && !slot.solo)) x = 0;
            slot.buffer[f] = x;
            left += x * slot.pan_left; right += x * slot.pan_right;
        }
        left = dsp::soft_clip(left * volume); right = dsp::soft_clip(right * volume);
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        out[2 * f] = int32_t(std::clamp(left, -0.999f, 0.999f) * 2147483647.0f);
        out[2 * f + 1] = int32_t(std::clamp(right, -0.999f, 0.999f) * 2147483647.0f);
    }
    for (unsigned t = 0; t < track_count; ++t) {
        float level = 0;
        for (unsigned f = 0; f < frames; ++f) level = std::max(level, std::fabs(slots[t].buffer[f]));
        float shown = meters[t].load(std::memory_order_relaxed) * 0.97f; // a short fall
        meters[t].store(std::max(level, shown), std::memory_order_relaxed);
    }
    output_peak.store(peak, std::memory_order_relaxed);
}

} // namespace melodee
