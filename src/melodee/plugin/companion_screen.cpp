// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/companion_screen.h"
#include "melodee/plugin/companion_host.h"
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace melodee {
using companion::Message;
using companion::Op;

namespace {
constexpr auto draw_limit = std::chrono::milliseconds(500); // a draw takes 2 ms; past this the screen counts as hung
constexpr auto refresh_gap = std::chrono::milliseconds(50); // the copies read again twenty times a second
} // namespace

CompanionScreen::CompanionScreen(juce::AudioPluginInstance& instance, CompanionHost& helper, CompanionPackage package, uint32_t id)
    : plugin(instance), host(helper), pack(std::move(package)), session(id) {
    const auto& all = plugin.getParameters();
    const size_t count = std::min<size_t>(pack.manifest.params.size(), companion::max_params);
    params.resize(count, nullptr);
    copies.resize(count);
    for (size_t h = 0; h < count; ++h) {
        const auto& spec = pack.manifest.params[h];
        auto& copy = copies[h];
        copy = {};
        if (spec.index < 0 || spec.index >= all.size()) continue; // the rack matched it: not so
        auto* p = all[spec.index];
        params[h] = p;
        companion::put_text(copy.name, sizeof(copy.name), p->getName(int(sizeof(copy.name)) - 1).toRawUTF8());
        copy.initial = std::clamp(p->getDefaultValue(), 0.0f, 1.0f);
        const int steps = p->getNumSteps();
        // A plug-in that counts its own positions says so; JUCE's "continuous" is a huge count. Else screen.json's.
        copy.steps = steps > 1 && steps < 100000 ? uint32_t(steps - 1) : spec.steps > 0 ? uint32_t(spec.steps) : 0u;
        copy.value = -1.0f; // read at the first refresh
    }
    program_count = std::max(1, plugin.getNumPrograms());
}

CompanionScreen::~CompanionScreen() = default;

std::string CompanionScreen::stop_reason() const {
    std::lock_guard<std::mutex> held(requests_lock);
    return reason;
}

bool CompanionScreen::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> held(requests_lock);
        if (reason.empty()) reason = why.empty() ? "it stopped" : why;
        edits.clear();
        picked = -1;
    }
    stopped.store(true, std::memory_order_release);
    if (host.open_session() == session) host.close_session(session);
    return false;
}

void CompanionScreen::close() {
    if (host.open_session() == session) host.close_session(session);
}

// Screen thread: the copies from the plug-in. `texts`: every text read again, not only the moved ones' (a preset can
// change a value's text without its value). True when any copy changed.
bool CompanionScreen::refresh(bool texts) {
    refreshed = std::chrono::steady_clock::now();
    bool moved = false;
    for (size_t h = 0; h < params.size(); ++h) {
        auto* p = params[h];
        if (!p) continue;
        auto& copy = copies[h];
        float v = p->getValue();
        v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        if (v == copy.value && !texts) continue;
        char text[sizeof(copy.text)] {};
        companion::put_text(text, sizeof(text), p->getText(v, int(sizeof(text)) - 1).trim().toRawUTF8());
        if (v != copy.value || std::strcmp(text, copy.text)) moved = true;
        copy.value = v;
        std::memcpy(copy.text, text, sizeof(text));
    }
    melodee_companion_program_t now {};
    now.count = uint32_t(program_count);
    now.current = plugin.getCurrentProgram();
    if (now.current >= 0 && now.current < program_count)
        companion::put_text(now.name, sizeof(now.name), plugin.getProgramName(now.current).trim().toRawUTF8());
    if (now.current != program.current || std::strcmp(now.name, program.name)) moved = true;
    program = now;
    return moved;
}

void CompanionScreen::write_block() {
    auto* block = host.block();
    if (!block) return;
    block->param_count = uint32_t(copies.size());
    std::copy(copies.begin(), copies.end(), block->params);
    block->program = program;
}

// Its screen made in the helper, unless it is the one there now. `fresh`: just made (its picture is not there).
bool CompanionScreen::ensure_open(bool& fresh) {
    fresh = false;
    if (host.running() && host.open_session() == session) return true;
    std::string why;
    if (!host.start(why)) return fail(why);
    refresh(true);
    write_block();
    if (!host.open(session, pack.library, uint32_t(copies.size()), why)) return fail(why);
    fresh = true;
    return true;
}

// The screen's own requests that came with the last answers: kept for take(), each checked. One asking for a handle it
// does not have, or a program the plug-in does not have, stops the screen (false).
bool CompanionScreen::collect() {
    auto& got = host.requests();
    bool valid = true;
    {
        std::lock_guard<std::mutex> held(requests_lock);
        for (const auto& m : got) {
            if (m.session != session) continue; // an earlier screen's
            if (!companion::valid_reply(m, session, uint32_t(params.size()), uint32_t(program_count))) { valid = false; break; }
            switch (Op(m.op)) {
            case Op::Edit:
                if (params[size_t(m.a)]) edits[pack.manifest.params[size_t(m.a)].index] = std::clamp(m.value, 0.0f, 1.0f);
                break;
            case Op::Program: picked = m.a; break;
            case Op::Redraw: redraw.store(true, std::memory_order_release); break;
            default: break;
            }
        }
    }
    got.clear();
    return valid || fail("the screen asked for a parameter or program it does not have");
}

void CompanionScreen::take(std::vector<CompanionRequest>& out) {
    std::lock_guard<std::mutex> held(requests_lock);
    for (const auto& [index, value] : edits) out.push_back({false, index, value});
    if (picked >= 0) out.push_back({true, picked, 0.0f});
    edits.clear();
    picked = -1;
}

template <class Fill> bool CompanionScreen::send(Op op, Op answer, Fill fill, Message& reply) {
    Message m;
    m.op = uint32_t(op);
    m.session = session;
    fill(m);
    std::string why;
    if (!host.ask(m, answer, reply, draw_limit, why)) return fail(why);
    if (!collect()) return false;
    int indices[3] {-1, -1, -1};
    companion::controls_of(reply, uint32_t(params.size()), [&](uint32_t h) { return params[h] ? pack.manifest.params[h].index : -1; }, indices);
    for (unsigned k = 0; k < 3; ++k) controls[k] = indices[k];
    return true;
}

bool CompanionScreen::screenDraw(const melodee_screen_frame& frame, bool& changed) {
    changed = false;
    if (stopped.load(std::memory_order_acquire)) return true;
    bool fresh = false;
    if (!ensure_open(fresh)) return true;
    refresh(false);
    write_block();
    const bool all = fresh || frame.redraw_all;
    Message reply;
    const bool asked = send(Op::Draw, Op::Drawn, [&](Message& m) {
        m.area = frame.area;
        m.rows = frame.height;
        m.redraw_all = all ? 1u : 0u;
        m.colour = frame.colour;
        m.time_ms = frame.time_ms;
        m.playing = frame.playing;
        m.shift = frame.shift;
        for (unsigned k = 0; k < 3; ++k) { m.pots[k] = frame.pots[k]; m.pots_live[k] = frame.pots_live[k]; }
    }, reply);
    if (!asked) return true;
    changed = reply.a != 0;
    if (changed || all) {
        const auto* block = host.block();
        const unsigned rows = std::min<unsigned>(frame.height, companion::height), across = std::min<unsigned>(frame.width, companion::width);
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < across; ++x) frame.pixels[size_t(y) * frame.stride + x] = block->pixels[size_t(y) * companion::width + x] & 0xffffffu;
        changed = true;
    }
    return true;
}

bool CompanionScreen::screenInput(const melodee_screen_input& input, bool& used) {
    used = false;
    if (stopped.load(std::memory_order_acquire)) return true;
    bool fresh = false;
    if (!ensure_open(fresh)) return true;
    write_block();
    Message reply;
    if (!send(Op::Input, Op::Used, [&](Message& m) { m.a = int32_t(input.kind); m.b = input.value; m.c = input.shift ? 1 : 0; }, reply)) return true;
    used = reply.a != 0;
    redraw.store(true, std::memory_order_release);
    return true;
}

bool CompanionScreen::screenControls(std::array<int, 3>& indices) {
    indices = controls;
    return true;
}

bool CompanionScreen::takeRedraw() {
    if (stopped.load(std::memory_order_acquire)) return false;
    if (host.running() && host.open_session() == session) {
        std::string why;
        if (!host.pump(why)) { fail(why); return false; }
        if (!collect()) return false;
    }
    if (std::chrono::steady_clock::now() - refreshed >= refresh_gap && refresh(false)) redraw.store(true, std::memory_order_release);
    return redraw.exchange(false, std::memory_order_acq_rel);
}

} // namespace melodee
