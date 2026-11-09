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
    for (auto& s : steps_now) s.store(-1);
    // Build every engine's static tables now, not on the audio thread.
    for (unsigned k = 0; k < unsigned(EngineKind::Count); ++k) (void)engine_info(EngineKind(k));
}

void EngineHost::load(const Project& project, const PatternBank& bank) {
    *patterns = bank;
    for (unsigned n = 0; n < chain_count; ++n) {
        const auto& chain = project.chain(n);
        for (unsigned k = 0; k < max_slots; ++k) {
            const auto& s = chain.slots[k];
            fx[n][k].set_kind(EffectKind(s.kind), pool);
            for (unsigned q = 0; q < effect_params; ++q) fx[n][k].set_param(q, s.params[q]);
            fx[n][k].set_bypass(s.bypass);
        }
    }
    for (unsigned b = 0; b < bus_count; ++b) returns[b] = project.buses[b].level / 100.0f;
    click = project.click; click_level = project.click_level / 100.0f;
    motion = project.motion;
    song = project.song;
    tempo = project.tempo; swing = project.swing;
    for (auto& s : slots) s.sampler.set_tempo(tempo);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = project.tracks[t];
        auto& slot = slots[t];
        select_engine(slot, track.engine);
        const auto& info = engine_info(track.engine);
        slot.base = track.params;
        if (slot.engine) for (unsigned r = 0; r < mod_routes; ++r) slot.engine->set_mod(r, track.mods[r]);
        for (unsigned pad = 0; pad < sampler_pads; ++pad) slot.sampler.set_pad(pad, track.pads[pad]);
        if (slot.engine) for (unsigned p = 0; p < info.param_count; ++p) slot.engine->set_param(int(p), track.params[p]);
        apply({Command::Type::Level, uint8_t(t), 0, 0, track.mix.level});
        apply({Command::Type::Pan, uint8_t(t), 0, 0, track.mix.pan});
        slot.send = track.mix.send / 100.0f;
        slot.bus = track.mix.bus;
        slot.mute = track.mix.mute; slot.solo = track.mix.solo;
        slot.pattern = track.pattern;
        patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
    }
}

void EngineHost::select_engine(Slot& slot, EngineKind kind) {
    if (slot.engine) slot.engine->silence();
    slot.kind = kind;
    slot.engine = kind == EngineKind::Prophet ? static_cast<Engine*>(&slot.prophet)
                : kind == EngineKind::Sampler ? static_cast<Engine*>(&slot.sampler)
                : kind == EngineKind::Kit909 ? static_cast<Engine*>(&slot.kit) : nullptr;
    slot.sampler.set_samples(&samples);
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
            slot.base = values;
        }
        break;
    case Type::Param:
        if (c.index < max_params) slot.base[c.index] = c.value;
        if (slot.engine) slot.engine->set_param(c.index, c.value);
        break;
    case Type::MotionWrite: motion.write(c.motion); break;
    case Type::Mod: if (slot.engine && c.index < mod_routes) slot.engine->set_mod(c.index, c.route); break;
    case Type::FxKind: if (c.track < chain_count && c.index < max_slots) fx[c.track][c.index].set_kind(EffectKind(c.value), pool); break;
    case Type::FxParam: if (c.track < chain_count && c.index < max_slots) fx[c.track][c.index].set_param(c.pattern, c.value); break;
    case Type::FxBypass: if (c.track < chain_count && c.index < max_slots) fx[c.track][c.index].set_bypass(c.value != 0); break;
    case Type::SendBus: slot.bus = uint8_t(c.value % bus_count); break;
    case Type::Return: if (c.index < bus_count) returns[c.index] = std::clamp<int>(c.value, 0, 100) / 100.0f; break;
    case Type::Pad: slot.sampler.set_pad(c.index, c.pad); break;
    case Type::Reload:
        if (c.pack) {
            apply({Type::Stop});
            for (auto& s : slots) if (s.engine) s.engine->silence();
            load(c.pack->project, c.pack->patterns); // a one-off copy, while stopped
            if (!done_packs.push(c.pack)) {}         // the MIDI thread frees it; a full queue only leaks one
        }
        break;
    case Type::Click:
        click = c.value != 0;
        click_level = std::clamp<int>(c.index, 0, 100) / 100.0f;
        break;
    case Type::Arp: {
        auto& a = slot.arp;
        switch (c.index) {
        case 0:
            a.on = c.value != 0;
            if (!a.on) { a.clear(); if (a.sounding >= 0 && slot.engine) slot.engine->note_off(a.sounding); a.sounding = -1; }
            break;
        case 1: a.mode = ArpMode(c.value % int(ArpMode::Count)); break;
        case 2: a.rate = unsigned(c.value) % 6; break;
        case 3: a.octaves = unsigned(std::clamp<int>(c.value, 1, 4)); break;
        case 4: a.gate = unsigned(std::clamp<int>(c.value, 5, 100)); break;
        }
        break;
    }
    case Type::ArpKey:
        if (c.value) slot.arp.press(c.index, c.value); else slot.arp.release(c.index);
        if (!slot.arp.count) slot.arp.position = 0;
        break;
    case Type::RecordArm: {
        source_now.store(c.track % 3, std::memory_order_relaxed);
        uint32_t length = c.pattern ? uint32_t(c.pattern * 4 * 60.0 / (tempo / 10.0) * 44100) : 0;
        take.arm(float(c.value), length, record_gain, c.index);
        break;
    }
    case Type::RecordGain: record_gain = float(c.value); take.set_gain(record_gain); break;
    case Type::RecordNow: take.start_now(); break;
    case Type::RecordStop: take.stop(); break;
    case Type::PadNote:
        if (slot.engine == &slot.sampler) slot.sampler.play_pad(c.index, c.value, int(c.pattern) - 64);
        break;
    case Type::Audition:
        if (slot.engine == &slot.sampler) slot.sampler.audition(c.pad.sample, c.pad.start, c.pad.end, c.value);
        break;
    case Type::Pressure: if (slot.engine) slot.engine->set_pressure(c.index, c.value / 127.0f); break;
    case Type::MotionClear: motion.clear(c.motion.track, c.motion.pattern, c.motion.param); break;
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
    case Type::Pattern:
        if (running && !song_mode) slot.queued = uint8_t(c.value % pattern_count);
        else if (!song_mode) { slot.pattern = uint8_t(c.value % pattern_count); slot.queued = 255; }
        patterns_now[c.track % track_count].store(slot.pattern, std::memory_order_relaxed);
        break;
    case Type::Division:
        patterns->at[c.track % track_count][c.pattern % pattern_count].division = Division(c.value % int(Division::Count));
        break;
    case Type::SongClip: if (c.index < song_clips) song.clips[c.index] = c.clip; break;
    case Type::SongCount: song.count = uint16_t(std::clamp<int>(c.value, 0, song_clips)); break;
    case Type::SongLoop: song.loop_start = c.index; song.loop_end = uint16_t(c.value); break;
    case Type::Play:
        if (running) break;
        running = true; tick_now = 0; until_tick = 0;
        song_mode = c.value == 1; song_bar_now = c.index;
        for (auto& s : slots) {
            s.start = 0; s.clip = -1; s.silent = song_mode;
            if (s.queued != 255) { s.pattern = s.queued; s.queued = 255; }
        }
        in_song.store(song_mode, std::memory_order_relaxed);
        break;
    case Type::Stop:
        running = false;
        song_mode = false;
        in_song.store(false, std::memory_order_relaxed);
        for (unsigned t = 0; t < track_count; ++t) {
            steps_now[t].store(-1, std::memory_order_relaxed);
            if (slots[t].queued != 255) { slots[t].pattern = slots[t].queued; slots[t].queued = 255; }
            slots[t].silent = false; slots[t].clip = -1;
            patterns_now[t].store(slots[t].pattern, std::memory_order_relaxed);
        }
        for (auto& s : slots) restore(s);
        for (auto& s : slots) {
            for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
        }
        break;
    case Type::Tempo:
        tempo = uint16_t(std::clamp<int>(c.value, 300, 3000));
        for (auto& s : slots) s.sampler.set_tempo(tempo);
        break;
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

void EngineHost::run_chain(unsigned chain, float* left, float* right, unsigned frames) {
    for (auto& effect : fx[chain]) {
        if (effect.kind() == EffectKind::None) break; // slots are packed from the front
        effect.process(left, right, frames);
    }
}

// Back to the track's own values once motion has moved them.
void EngineHost::restore(Slot& slot) {
    if (!slot.moved || !slot.engine) return;
    for (unsigned p = 0; p < max_params; ++p) slot.engine->set_param(int(p), slot.base[p]);
    slot.moved = false;
}

// Motion at step `at` of the track's pattern: each lane starts the pattern
// at the track's own value, then follows its events.
void EngineHost::play_motion(unsigned t, Slot& slot, unsigned at) {
    if (at == 0) restore(slot);
    for (unsigned n = 0; n < motion.count; ++n) {
        const auto& e = motion.events[n];
        if (e.track != t || e.pattern != slot.pattern || e.step != at) continue;
        slot.engine->set_param(e.param, e.value);
        slot.moved = true;
    }
}

// Swing stretches the first sixteenth of every eighth and shortens the second.
double EngineHost::tick_samples(uint32_t at) const {
    double base = quarter_tick();
    return (at % 12) < 6 ? base * swing / 50.0 : base * (100 - swing) / 50.0;
}

void EngineHost::bar_start() {
    if (!song_mode) {
        for (unsigned t = 0; t < track_count; ++t) {
            auto& slot = slots[t];
            if (slot.queued == 255) continue;
            slot.pattern = slot.queued; slot.queued = 255; slot.start = tick_now;
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
        }
        return;
    }
    if (tick_now) ++song_bar_now;
    if (song.looping() && song_bar_now == song.loop_end) song_bar_now = song.loop_start;
    if (!song.looping() && song_bar_now >= std::max(song.length(), 1u)) { // the end of the song
        running = false;
        for (auto& s : slots) for (auto& off : s.offs) if (off.key >= 0 && s.engine) { s.engine->note_off(off.key); off.key = -1; }
        return;
    }
    bar_now.store(song_bar_now, std::memory_order_relaxed);
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        int index = song.clip_at(t, song_bar_now);
        slot.silent = index < 0 || song.clips[size_t(index)].muted;
        if (slot.silent) { slot.clip = -1; steps_now[t].store(-1, std::memory_order_relaxed); continue; }
        const auto& clip = song.clips[size_t(index)];
        if (index != slot.clip || song_bar_now == clip.bar) {
            // A clip starts (or the loop came back into it): its pattern from where the clip says.
            slot.clip = index;
            slot.pattern = clip.pattern;
            slot.start = tick_now - (song_bar_now - clip.bar) * ticks_per_bar;
            patterns_now[t].store(slot.pattern, std::memory_order_relaxed);
        }
    }
}

void EngineHost::arp_tick() {
    for (auto& slot : slots) {
        auto& a = slot.arp;
        if (!a.on || !slot.engine) continue;
        unsigned rate = arp_rate_ticks(a.rate);
        if (arp_ticks % rate) continue;
        int velocity = 0, key = a.next(velocity);
        if (key < 0) continue;
        slot.engine->note_on(key, velocity);
        schedule_off(slot, key, std::max<int32_t>(64, int32_t(quarter_tick() * rate * a.gate / 100.0)));
    }
    ++arp_ticks;
}

void EngineHost::tick() {
    // The metronome: every quarter while playing, higher on the bar.
    if (click && tick_now % ticks_per_quarter == 0) {
        click_env = 1; click_phase = 0;
        click_step = (tick_now % ticks_per_bar == 0 ? 1568.0f : 1046.0f) / 44100.0f;
    }
    if (tick_now % ticks_per_bar == 0) bar_start();
    if (!running) return;
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        if (!slot.engine || slot.silent) continue;
        const auto& pattern = patterns->at[t][slot.pattern];
        unsigned division = division_ticks(pattern.division), relative = tick_now - slot.start;
        if (relative % division) continue;
        unsigned at = (relative / division) % pattern.length;
        steps_now[t].store(int(at), std::memory_order_relaxed);
        if (motion.count) play_motion(t, slot, at);
        const auto& s = pattern.steps[at];
        if (s.empty()) continue;
        random = random * 1664525u + 1013904223u;
        if (s.chance < 100 && (random >> 8) % 100 >= s.chance) continue;
        // Held for its length, a touch short of the next step unless tied.
        double length = quarter_tick() * division;
        auto samples = int32_t(length * s.length - (s.flags & step_tie ? 0 : length * 0.1));
        for (const auto& note : s.notes) {
            if (!note.velocity) continue;
            int velocity = s.flags & step_accent ? std::min(127, note.velocity + 24) : note.velocity;
            slot.engine->note_on(note.key, velocity);
            schedule_off(slot, note.key, std::max<int32_t>(samples, 64));
        }
    }
    until_tick += tick_samples(tick_now);
    ++tick_now;
    tick_count.store(tick_now, std::memory_order_relaxed);
}

void EngineHost::render(int32_t* out, unsigned frames, float volume, const int32_t* input) {
    static thread_local bool flushed = false;
    if (!flushed) { flush_denormals(); flushed = true; }
    Command command;
    for (unsigned n = 0; n < 4096 && queue.pop(command); ++n) apply(command);
    Offer offer;
    while (offers.pop(offer)) {
        if (offer.slot >= sample_slots) continue;
        for (auto& slot : slots) slot.sampler.sample_gone(offer.slot); // nothing may play the old one past here
        const SampleData* old = samples[offer.slot];
        samples[offer.slot] = offer.sample;
        if (old && !retired.push(old)) samples[offer.slot] = old; // no room to give it back: keep it, try later
    }
    frames = std::min(frames, block);
    // The arps' clock, at the block's start (within 3 ms of the beat).
    until_arp -= frames;
    while (until_arp <= 0) { arp_tick(); until_arp += quarter_tick(); }
    bool solo = false;
    for (auto& slot : slots) {
        solo |= slot.solo;
        std::fill(slot.buffer, slot.buffer + frames, 0.0f);
        std::fill(slot.left, slot.left + frames, 0.0f); std::fill(slot.right, slot.right + frames, 0.0f);
    }

    // Render up to each step, play the step, carry on: steps land on the sample.
    unsigned done = 0;
    while (done < frames) {
        unsigned n = frames - done;
        if (running) {
            if (until_tick <= 0) { tick(); continue; }
            n = std::min<unsigned>(n, unsigned(std::ceil(until_tick)));
        }
        for (auto& slot : slots) {
            if (!slot.engine) continue;
            run_offs(slot, n);
            if (slot.engine->stereo()) slot.engine->render_stereo(slot.left + done, slot.right + done, n);
            else slot.engine->render(slot.buffer + done, n);
        }
        if (running) until_tick -= n;
        done += n;
    }
    is_playing.store(running, std::memory_order_relaxed);

    // The signal: each track panned, through its chain, into the master and
    // its send into one bus; each bus through its chain at its return level
    // into the master; the master through its chain, then main volume.
    for (unsigned b = 0; b < bus_count; ++b) { std::fill(bus_l[b], bus_l[b] + frames, 0.0f); std::fill(bus_r[b], bus_r[b] + frames, 0.0f); }
    std::fill(master_l, master_l + frames, 0.0f); std::fill(master_r, master_r + frames, 0.0f);
    for (unsigned t = 0; t < track_count; ++t) {
        auto& slot = slots[t];
        float level = 0;
        if (slot.engine) {
            if (slot.engine->stereo()) { // its own stereo picture; the track's pan balances it
                float l = slot.gain * slot.pan_left * 1.4142f, r = slot.gain * slot.pan_right * 1.4142f;
                for (unsigned f = 0; f < frames; ++f) { slot.left[f] *= l; slot.right[f] *= r; }
            } else {
                for (unsigned f = 0; f < frames; ++f) {
                    float x = slot.buffer[f] * slot.gain;
                    slot.left[f] = x * slot.pan_left; slot.right[f] = x * slot.pan_right;
                }
            }
            run_chain(t, slot.left, slot.right, frames);
            bool heard = !slot.mute && !(solo && !slot.solo);
            if (heard) {
                for (unsigned f = 0; f < frames; ++f) {
                    master_l[f] += slot.left[f]; master_r[f] += slot.right[f];
                    level = std::max(level, std::max(std::fabs(slot.left[f]), std::fabs(slot.right[f])));
                }
                if (slot.send > 0)
                    for (unsigned f = 0; f < frames; ++f) { bus_l[slot.bus][f] += slot.left[f] * slot.send; bus_r[slot.bus][f] += slot.right[f] * slot.send; }
            }
        }
        float shown = meters[t].load(std::memory_order_relaxed) * 0.97f; // a short fall
        meters[t].store(std::max(level, shown), std::memory_order_relaxed);
    }
    for (unsigned b = 0; b < bus_count; ++b) {
        run_chain(bus_chain(b), bus_l[b], bus_r[b], frames);
        for (unsigned f = 0; f < frames; ++f) { master_l[f] += bus_l[b][f] * returns[b]; master_r[f] += bus_r[b][f] * returns[b]; }
    }
    run_chain(master_chain, master_l, master_r, frames);
    // Record: the mix itself (before main volume), or what comes in.
    if (source_now.load(std::memory_order_relaxed) == 2) take.process(master_l, master_r, frames);
    else {
        for (unsigned f = 0; f < frames; ++f) {
            record_l[f] = input ? input[2 * f] / 2147483648.0f : 0.0f;
            record_r[f] = input ? input[2 * f + 1] / 2147483648.0f : 0.0f;
        }
        if (source_now.load(std::memory_order_relaxed) == 0) std::copy(record_l, record_l + frames, record_r); // the mic is mono
        take.process(record_l, record_r, frames);
    }
    float peak = 0;
    for (unsigned f = 0; f < frames; ++f) {
        if (click_env > 0.0005f) { // a short blip on top of the mix
            float blip = std::sin(2 * dsp::pi * click_phase) * click_env * click_level * 0.5f;
            click_phase += click_step; click_phase -= std::floor(click_phase);
            click_env *= 0.9985f;
            master_l[f] += blip; master_r[f] += blip;
        }
        float left = dsp::soft_clip(master_l[f] * volume), right = dsp::soft_clip(master_r[f] * volume);
        peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
        out[2 * f] = int32_t(std::clamp(left, -0.999f, 0.999f) * 2147483647.0f);
        out[2 * f + 1] = int32_t(std::clamp(right, -0.999f, 0.999f) * 2147483647.0f);
    }
    output_peak.store(peak, std::memory_order_relaxed);
}

} // namespace melodee
