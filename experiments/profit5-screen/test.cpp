// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/vst2_abi.h"
#include <melodee_screen.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
int edits = 0, runs = 0;
intptr_t host(vst2::Effect*, int32_t op, int32_t, intptr_t value, void* p, float) {
    if (op == vst2::host::version) return 2400;
    if (op == vst2::host::canDo && p && !std::strcmp(static_cast<char*>(p), MELODEE_SCREEN_VST2_CANDO)) return 1;
    if (op == vst2::host::automate) ++edits;
    if (op == vst2::host::vendorSpecific && (value & MELODEE_SCREEN_RUN)) ++runs;
    return 0;
}
void close(vst2::Effect* e) { e->dispatcher(e, vst2::op::close, 0, 0, nullptr, 0); }
void note(vst2::Effect* e, bool on) {
    vst2::MidiEvent midi {};
    midi.type = 1; midi.byteSize = sizeof(midi); midi.data[0] = on ? char(0x90) : char(0x80);
    midi.data[1] = 60; midi.data[2] = 100;
    vst2::Events<2> events {}; events.count = 1; events.events[0] = &midi;
    e->dispatcher(e, vst2::op::processEvents, 0, 0, &events, 0);
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto entry = reinterpret_cast<vst2::EntryPoint>(dlsym(lib, "VSTPluginMain"));
    auto original = reinterpret_cast<vst2::EntryPoint>(dlsym(lib, "profit5_original_main"));
    assert(entry && original);
    auto* e = entry(host); auto* second = entry(host);
    assert(e && second && e->uniqueId == second->uniqueId);
    assert(e->dispatcher(e, vst2::op::canDo, 0, 0, const_cast<char*>(MELODEE_SCREEN_VST2_CANDO), 0) == 1);
    auto* api = reinterpret_cast<const melodee_screen_t*>(e->dispatcher(e, vst2::op::vendorSpecific, MELODEE_SCREEN_VST2, 0, nullptr, 0));
    assert(api && api->draw && api->input && api->controls);
    // Every synth parameter and Program is reachable; row navigation and UI state are per instance.
    std::array<bool, 59> reachable {};
    uint32_t first_ids[3], ids[3]; api->controls(second, first_ids);
    constexpr int stride = 327, height = 218;
    std::vector<uint32_t> pixels(stride * height + 64, 0xdeadbeef);
    melodee_screen_frame_t frame {};
    frame.pixels = pixels.data(); frame.width = 320; frame.height = height; frame.stride = stride;
    frame.colour = 0xff0000; for (auto& pot : frame.pots) pot = -1;
    double slowest = 0, total = 0;
    const auto captures = std::filesystem::path(argv[1]).parent_path() / "panels";
    std::filesystem::create_directories(captures);
    for (int row = 0; row < 22; ++row) {
        api->controls(e, ids);
        for (auto id : ids) { assert(id < reachable.size()); reachable[id] = true; }
        frame.redraw_all = 1;
        const auto start = std::chrono::steady_clock::now();
        assert(api->draw(e, &frame));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        total += ms; slowest = std::max(slowest, ms);
        std::ofstream ppm(captures / (std::to_string(row) + ".ppm"), std::ios::binary);
        ppm << "P6\n320 218\n255\n";
        for (int y = 0; y < height; ++y) for (int x = 0; x < 320; ++x) {
            uint32_t colour = pixels[y * stride + x];
            const char rgb[] {char(colour >> 16), char(colour >> 8), char(colour)};
            ppm.write(rgb, 3);
        }
        frame.redraw_all = 0; assert(!api->draw(e, &frame));
        melodee_screen_input_t next {MELODEE_SCREEN_STEP, 1, 0}; assert(api->input(e, &next));
    }
    assert(std::all_of(reachable.begin(), reachable.end(), [](bool x) { return x; }));
    api->controls(second, ids); assert(std::equal(ids, ids + 3, first_ids));
    api->controls(e, ids); assert(std::equal(ids, ids + 3, first_ids));
    // Shift navigation jumps sections; a binary switch moves by exactly one choice.
    const uint32_t section_first[] {13, 16, 0, 46, 58, 2};
    melodee_screen_input_t section {MELODEE_SCREEN_STEP, 1, 1};
    for (auto expected : section_first) {
        assert(api->input(e, &section)); api->controls(e, ids); assert(ids[0] == expected);
    }
    melodee_screen_input_t next {MELODEE_SCREEN_STEP, 1, 0};
    assert(api->input(e, &next));
    melodee_screen_input_t toggle {MELODEE_SCREEN_PUSH, 1, 0}, down {MELODEE_SCREEN_WHEEL, -1, 0};
    assert(api->input(e, &toggle)); assert(api->input(e, &down)); assert(e->getParameter(e, 27) == 0.0f);
    melodee_screen_input_t back {MELODEE_SCREEN_STEP, -1, 0}; assert(api->input(e, &back));
    for (int y = 0; y < height; ++y) for (int x = 320; x < stride; ++x) assert(pixels[y * stride + x] == 0xdeadbeef);
    for (size_t i = stride * height; i < pixels.size(); ++i) assert(pixels[i] == 0xdeadbeef);
    // Selecting doesn't edit. Push enters edit. Fine changes, pickup redraw, external automation and state restore work.
    const float before = e->getParameter(e, 2);
    melodee_screen_input_t wheel {MELODEE_SCREEN_WHEEL, 1, 0}, push {MELODEE_SCREEN_PUSH, 1, 0};
    assert(api->input(e, &wheel)); assert(e->getParameter(e, 2) == before);
    assert(api->input(e, &push)); assert(api->input(e, &wheel));
    assert(edits > 0 && runs > 0);
    frame.redraw_all = 1; assert(api->draw(e, &frame)); frame.redraw_all = 0;
    frame.pots[1] = 127; assert(api->draw(e, &frame)); assert(!api->draw(e, &frame));
    e->setParameter(e, 3, 0.33f); assert(api->draw(e, &frame));
    const int runs_before = runs;
    e->setParameter(e, 13, 0.5f); assert(runs > runs_before);
    void* chunk = nullptr; const auto bytes = e->dispatcher(e, vst2::op::getChunk, 0, 0, &chunk, 0);
    assert(bytes > 0 && chunk);
    std::vector<char> saved(static_cast<char*>(chunk), static_cast<char*>(chunk) + bytes);
    const float saved_value = e->getParameter(e, 3);
    e->setParameter(e, 3, 0.9f);
    e->dispatcher(e, vst2::op::setChunk, 0, bytes, saved.data(), 0);
    assert(std::fabs(e->getParameter(e, 3) - saved_value) < 0.0001f);
    frame.area = MELODEE_SCREEN_STRIP; frame.height = 144; assert(api->draw(e, &frame));
    close(e); close(second);
    // The screen extension makes no change to the actual audio, against the untouched wrapper entry in this library.
    auto* plain = original(host); auto* screened = entry(host);
    assert(plain && screened && plain->numParams == screened->numParams);
    for (auto* plugin : {plain, screened}) {
        plugin->dispatcher(plugin, vst2::op::setProgram, 0, 0, nullptr, 0);
        note(plugin, true);
    }
    std::array<float, 128> l1 {}, r1 {}, l2 {}, r2 {};
    float* out1[] {l1.data(), r1.data()}, *out2[] {l2.data(), r2.data()};
    float peak = 0;
    for (int block = 0; block < 350; ++block) {
        if (block == 200) { note(plain, false); note(screened, false); }
        plain->processReplacing(plain, nullptr, out1, 128);
        screened->processReplacing(screened, nullptr, out2, 128);
        for (int i = 0; i < 128; ++i) {
            assert(std::isfinite(l1[i]) && std::isfinite(l2[i]));
            assert(l1[i] == l2[i] && r1[i] == r2[i]); peak = std::max(peak, std::fabs(l1[i]));
        }
    }
    assert(peak > 0.001f);
    close(plain); close(screened); dlclose(lib);
    std::printf("OK: all 59 synth/program controls, per-instance navigation, pickup, automation, state, clipping, idle, strip; audio identical (peak %.3f). Draw avg %.3f ms, max %.3f ms.\n", peak, total / 22, slowest);
}
