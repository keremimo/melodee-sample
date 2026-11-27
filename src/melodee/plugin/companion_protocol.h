// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Between Melodee and the process a companion screen runs in (plugin/companion_screen.h, plugin/companion_helper.cpp):
// one shared block (a memfd both map) with the picture and the parameters' copies, and fixed-size messages over a
// SOCK_SEQPACKET socket pair. Melodee asks (Open, Draw, Input, Close) and waits for the answer with a time limit; the
// screen's own requests (Edit, Program, Redraw) arrive among the answers, each naming the session it belongs to, so
// a late one never reaches a plug-in that replaced the one it was for. No JUCE here.
#pragma once
#include <melodee_companion.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace melodee::companion {

constexpr uint32_t magic = 0x4d4c4350; // 'MLCP'
constexpr uint32_t layout_version = 1;
constexpr unsigned max_params = 512;   // companion_params_most
constexpr unsigned width = MELODEE_SCREEN_WIDTH, height = MELODEE_SCREEN_PAGE_HEIGHT;
constexpr int helper_socket_fd = 3, helper_block_fd = 4; // where the helper finds them

// The shared block. Melodee writes `params`, `program` and `param_count` only while no request is out; the helper
// writes `pixels` only while answering Draw.
struct Block {
    uint32_t magic = companion::magic;
    uint32_t version = layout_version;
    uint32_t size = sizeof(Block);
    uint32_t param_count = 0;
    melodee_companion_program_t program {};
    melodee_companion_param_t params[max_params] {};
    uint32_t pixels[width * height] {};
};

enum class Op : uint32_t {
    // Melodee -> helper
    Open = 1, // text: the library's path; a: param_count. Makes a new screen (the old one destroyed first)
    Draw,     // the frame's fields below
    Input,    // a: kind, b: value, c: shift
    Close,    // destroy the screen; no answer
    // helper -> Melodee
    Opened = 16, // a: 1 made, 0 not (text: why)
    Drawn,       // a: 1 drew anything new; handles: controls()
    Used,        // a: 1 used it; handles: controls()
    Edit,        // a: handle, value: 0..1
    Program,     // a: program
    Redraw,
};

struct Message {
    uint32_t op = 0;
    uint32_t session = 0;  // Melodee's number for the screen the message is about
    int32_t a = 0, b = 0, c = 0;
    float value = 0;
    uint32_t handles[3] {MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL};
    // Draw: the frame, as melodee_screen_frame_t has it.
    uint32_t area = 0, rows = 0, redraw_all = 0, colour = 0;
    uint64_t time_ms = 0;
    uint8_t playing = 0, shift = 0, pots_live[3] {}, reserved = 0;
    int16_t pots[3] {-1, -1, -1};
    char text[256] {};
};
static_assert(std::is_trivially_copyable_v<Message>);
static_assert(sizeof(Message) <= 512, "one datagram");

inline bool from_helper(uint32_t op) { return op >= uint32_t(Op::Opened) && op <= uint32_t(Op::Redraw); }
inline bool to_helper(uint32_t op) { return op >= uint32_t(Op::Open) && op <= uint32_t(Op::Close); }

// What Melodee checks a message from the helper for before acting on it: the op, its session, a handle in range, a
// finite value, a program in range. Anything else stops the screen.
inline bool valid_reply(const Message& m, uint32_t session, uint32_t param_count, uint32_t program_count) {
    if (!from_helper(m.op) || m.session != session) return false;
    switch (Op(m.op)) {
    case Op::Edit: return m.a >= 0 && uint32_t(m.a) < param_count && m.value == m.value && m.value >= -0.001f && m.value <= 1.001f;
    case Op::Program: return m.a >= 0 && uint32_t(m.a) < program_count;
    default: return true;
    }
}

// The three handles K1-K3 turn, as parameter indices through `index_of` (screen.json's), -1 none.
template <class IndexOf> void controls_of(const Message& m, uint32_t param_count, IndexOf index_of, int out[3]) {
    for (unsigned k = 0; k < 3; ++k)
        out[k] = m.handles[k] != MELODEE_SCREEN_NO_CONTROL && m.handles[k] < param_count ? index_of(m.handles[k]) : -1;
}

inline void put_text(char* to, size_t room, const char* text) {
    if (!room) return;
    size_t n = 0;
    for (; text && text[n] && n + 1 < room; ++n) to[n] = text[n];
    to[n] = 0;
}

} // namespace melodee::companion
