// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/tools.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/pad_params.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <vector>

// --melodee-dsp-bench (roadmap P2): a busy project played as the audio thread plays it, a block every period at the
// device's rate, at the audio thread's real-time priority, with the render workers; what each block took and what
// the whole process spent, to compare ways of scheduling on the MPC. Nothing opens the audio device.
namespace melodee {
namespace {

// Sixteen tracks of every engine: four-note chords and lines on sixteenths, drums with ratchets, kits of samples,
// Motion on the synths, two effects on every track, sends into buses A-D (reverbs and echoes), the compressor on.
void busy_project(Project& p, PatternBank& b) {
    const EngineKind kinds[track_count] {EngineKind::Prophet, EngineKind::Fm6, EngineKind::Cz, EngineKind::Sid,
                                         EngineKind::Kit909, EngineKind::Sampler, EngineKind::Prophet, EngineKind::Fm6,
                                         EngineKind::Sampler, EngineKind::Cz, EngineKind::Sid, EngineKind::Prophet,
                                         EngineKind::Sampler, EngineKind::Fm6, EngineKind::Kit909, EngineKind::Sampler};
    const EffectKind chain[][2] {{EffectKind::Chorus, EffectKind::Drive}, {EffectKind::Phaser, EffectKind::Filter},
                                 {EffectKind::Crush, EffectKind::Echo}, {EffectKind::Compressor, EffectKind::Width},
                                 {EffectKind::Flanger, EffectKind::Tape}, {EffectKind::TapeEcho, EffectKind::Filter},
                                 {EffectKind::Drive, EffectKind::PingPong}, {EffectKind::Chorus, EffectKind::Compressor}};
    for (unsigned t = 0; t < track_count; ++t) {
        auto& track = p.tracks[t];
        load_sound(track, kinds[t], kinds[t] == EngineKind::Sampler || kinds[t] == EngineKind::Kit909 ? 0 : t % 4);
        track.mix.pan = int8_t(int(t % 8) * 12 - 42);
        track.mix.sends[t % bus_count] = uint8_t(20 + t * 3);
        for (unsigned k = 0; k < 2; ++k) track.fx.slots[k] = {uint8_t(chain[t % 8][k]), false, {60, 50, 40, 50, 30, 0}};
        if (kinds[t] == EngineKind::Sampler)
            for (unsigned pad = 0; pad < 8; ++pad) { track.pads[pad].sample = uint16_t(pad % 4); track.pads[pad].play = PadPlay::OneShot; }
        auto& q = b.at[t][0];
        bool drums = kinds[t] == EngineKind::Kit909 || kinds[t] == EngineKind::Sampler;
        for (unsigned s = 0; s < 16; ++s) {
            Step step;
            if (drums) {
                unsigned first = kinds[t] == EngineKind::Kit909 ? 36u : sampler_first_note;
                for (unsigned n = 0; n < 3; ++n) step.notes[n] = {uint8_t(first + (s + n * 3 + t) % 8), uint8_t(70 + 3 * s)};
            } else {
                for (unsigned n = 0; n < notes_per_step; ++n) step.notes[n] = {uint8_t(48 + (s * 5 + t) % 12 + n * 4), uint8_t(60 + 4 * n + s)};
                step.length = uint8_t(1 + (s + t) % 3);
            }
            if (s % 4 == 2) { step.ratchet = uint8_t(2 + s % 3); step.ramp = -30; }
            q.set_step(s, step);
        }
        if (!drums)
            for (unsigned s = 0; s < 16; s += 3) {
                MotionPoint m;
                m.tick = s * sixteenth_ticks; m.param = uint8_t((s / 3) % 4); m.value = int16_t(20 + s * 5);
                q.write_motion(m);
            }
    }
    const EffectKind buses[bus_count] {EffectKind::Reverb, EffectKind::Plate, EffectKind::Echo, EffectKind::Shimmer};
    for (unsigned b2 = 0; b2 < bus_count; ++b2) p.buses[b2].fx.slots[0] = {uint8_t(buses[b2]), false, {70, 50, 40, 50, 0, 0}};
    p.tempo = 1240;
}

double cpu_seconds(int who) {
    rusage r {};
    getrusage(who, &r);
    return double(r.ru_utime.tv_sec + r.ru_stime.tv_sec) + double(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
// The calling thread's own (Linux); 0 where it cannot be told apart.
double thread_seconds() {
#if defined(RUSAGE_THREAD)
    return cpu_seconds(RUSAGE_THREAD);
#else
    return 0;
#endif
}

} // namespace

// Arguments as key=value: seconds (10), rate (48000), threads (3 workers), mode (whole: one dispatch a block; split:
// the old way), spin (µs the workers wait awake for a run that follows within the block), linger (1: after every
// run; with mode=split spin=200, the way it was before roadmap P2), pin (1: this thread on core 1, the workers on
// cores 2, 3, ...; give threads=2), tracks (16).
int dsp_bench(int argc, char** argv) {
    double seconds = 10;
    unsigned rate = 48000, threads = 3, spin = 200, tracks = track_count;
    bool whole = true, linger = false, pinned = false;
    for (int n = 2; n < argc; ++n) {
        std::string arg = argv[n];
        auto eq = arg.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "dsp-bench: key=value, not %s\n", argv[n]); return 1; }
        std::string key = arg.substr(0, eq), value = arg.substr(eq + 1);
        if (key == "seconds") seconds = std::atof(value.c_str());
        else if (key == "rate") rate = unsigned(std::atoi(value.c_str()));
        else if (key == "threads") threads = unsigned(std::atoi(value.c_str()));
        else if (key == "spin") spin = unsigned(std::atoi(value.c_str()));
        else if (key == "mode") whole = value != "split";
        else if (key == "linger") linger = value == "1";
        else if (key == "pin") pinned = value == "1";
        else if (key == "tracks") tracks = std::min<unsigned>(unsigned(std::atoi(value.c_str())), track_count);
        else { std::fprintf(stderr, "dsp-bench: no key %s\n", key.c_str()); return 1; }
    }
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    busy_project(*project, *bank);
    for (unsigned t = tracks; t < track_count; ++t) project->tracks[t] = Track {};
    // Four samples for the kits: a tone, a noise burst, a falling sweep, a low thump, two seconds each.
    std::vector<SampleData> samples(4);
    for (unsigned s = 0; s < 4; ++s) {
        auto& d = samples[s];
        d.frames = 88200; d.left.resize(d.frames); d.right.resize(d.frames);
        uint32_t seed = 12345 + s;
        for (uint32_t f = 0; f < d.frames; ++f) {
            double t = f / 44100.0, fall = std::exp(-t * (3 + s));
            seed = seed * 1664525u + 1013904223u;
            double noise = int32_t(seed) / 2147483648.0;
            double x = s == 0 ? std::sin(2 * M_PI * 440 * t) : s == 1 ? noise : s == 2 ? std::sin(2 * M_PI * (900 - 600 * t) * t) : std::sin(2 * M_PI * 55 * t);
            d.left[f] = float(x * fall * 0.5); d.right[f] = float(x * fall * 0.45);
        }
    }
    EngineHost host;
    host.set_render_threads(threads, 150000, pinned);
    host.set_one_dispatch(whole);
    host.set_render_spin(spin, linger);
    host.set_overload_guard(true); // as Melodee runs
    host.load(*project, *bank);
    host.set_audio_format(rate, 384);
    {
        Command c {Command::Type::Reload};
        c.pack = new ProjectPack(*project, *bank);
        host.send(c);
    }
    for (unsigned s = 0; s < 4; ++s) host.offer_sample(s, &samples[s]);
    host.send({Command::Type::Compressor, 0, 0, 0, 1});
    host.send({Command::Type::Play});
    // As the audio thread: real-time, a block every period.
    sched_param priority {};
    priority.sched_priority = 30;
    bool realtime = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority) == 0;
    const unsigned frames = EngineHost::block;
    const long period_ns = long(1e9 * frames / rate);
    std::vector<int32_t> out(frames * 2);
    std::vector<float> took;
    auto blocks = unsigned(seconds * rate / frames);
    took.reserve(blocks);
    // A second of warming up (caches, the workers' first wake), then the count.
    for (unsigned n = 0; n < rate / frames; ++n) host.render(out.data(), frames, 0.7f);
    timespec next {};
    clock_gettime(CLOCK_MONOTONIC, &next);
    double cpu0 = cpu_seconds(RUSAGE_SELF), own0 = thread_seconds();
    auto wall0 = std::chrono::steady_clock::now();
    unsigned over = 0;
    for (unsigned n = 0; n < blocks; ++n) {
        auto t0 = std::chrono::steady_clock::now();
        host.render(out.data(), frames, 0.7f);
        float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
        took.push_back(us);
        if (us * 1000 > period_ns) ++over;
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
#if defined(__linux__)
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
#else
        timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        long wait = (next.tv_sec - now.tv_sec) * 1000000000L + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) { timespec rest {wait / 1000000000L, wait % 1000000000L}; nanosleep(&rest, nullptr); }
#endif
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    double cpu = cpu_seconds(RUSAGE_SELF) - cpu0, own = thread_seconds() - own0;
    host.send({Command::Type::Stop});
    host.render(out.data(), frames, 0.7f);
    const ProjectPack* pack = nullptr;
    while (host.take_done_pack(pack)) delete pack;
    std::vector<float> sorted = took;
    std::sort(sorted.begin(), sorted.end());
    double mean = 0;
    for (float us : took) mean += us;
    mean /= std::max<size_t>(took.size(), 1);
    auto at = [&](double q) { return sorted.empty() ? 0.0f : sorted[std::min(sorted.size() - 1, size_t(q * sorted.size()))]; };
    std::printf("MELODEE_DSP_BENCH guard=%u/%u mode=%s linger=%d pin=%d threads=%u spin=%u tracks=%u rate=%u realtime=%d blocks=%u period_us=%.0f "
                "mean_us=%.0f p50_us=%.0f p99_us=%.0f max_us=%.0f over=%u cpu_pct=%.1f audio_thread_pct=%.1f workers_pct=%.1f "
                "parallel_blocks=%llu calls_lost=%u\n",
                host.guard_steals(), host.guard_blocks(), whole ? "whole" : "split", int(linger), int(pinned), threads, spin, tracks, rate, int(realtime), blocks, period_ns / 1000.0, mean, at(0.5),
                at(0.99), sorted.empty() ? 0.0f : sorted.back(), over, 100 * cpu / wall, 100 * own / wall, 100 * (cpu - own) / wall,
                (unsigned long long)host.render_parallel_blocks(), host.calls_lost());
    return 0;
}

} // namespace melodee
