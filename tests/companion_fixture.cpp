// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A companion screen that misbehaves on purpose, for tests/test_companions.py: the environment's MELODEE_FIXTURE says
// how, at its second draw. ok: draws a band and asks for nothing; crash; exit; hang; flood (thousands of datagrams);
// garbage (a datagram of the wrong size); badedit (an edit for a handle it does not list); abi (refuses the host).
#include "melodee/plugin/companion_protocol.h"
#include <melodee_companion.h>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct Fixture { const melodee_companion_host_t* host; int draws = 0; };
const char* mode() { const char* m = std::getenv("MELODEE_FIXTURE"); return m ? m : "ok"; }
bool is(const char* m) { return !std::strcmp(mode(), m); }
void raw(melodee::companion::Op op, int32_t a) {
    melodee::companion::Message m;
    m.op = uint32_t(op);
    m.session = 0; // filled below from what the host's last message was is not known here: use a likely one
    m.a = a;
    for (uint32_t session = 1; session <= 2; ++session) {
        m.session = session;
        send(melodee::companion::helper_socket_fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

void* create(const melodee_companion_host_t* host) { return new (std::nothrow) Fixture {host}; }
void destroy(void* s) { delete static_cast<Fixture*>(s); }
bool draw(void* s, const melodee_screen_frame_t* f) {
    auto& x = *static_cast<Fixture*>(s);
    for (uint32_t y = 0; y < f->height; ++y)
        for (uint32_t c = 0; c < f->width; ++c) f->pixels[y * f->stride + c] = y < 20 ? 0x3060a0 : 0x101010;
    if (++x.draws < 2) return true;
    if (is("crash")) { volatile int* nowhere = nullptr; *nowhere = 1; }
    if (is("exit")) _exit(0);
    if (is("hang")) for (;;) pause();
    if (is("flood")) for (int n = 0; n < 5000; ++n) raw(melodee::companion::Op::Redraw, 0);
    if (is("garbage")) send(melodee::companion::helper_socket_fd, "xy", 2, MSG_NOSIGNAL);
    if (is("badedit")) raw(melodee::companion::Op::Edit, 9999);
    return true;
}
bool input(void*, const melodee_screen_input_t*) { return true; }
void controls(void*, uint32_t handles[3]) { handles[0] = 13; handles[1] = 14; handles[2] = MELODEE_SCREEN_NO_CONTROL; }
const melodee_companion_t table {MELODEE_COMPANION_ABI, sizeof(melodee_companion_t), create, destroy, draw, input, controls, nullptr};
} // namespace

extern "C" __attribute__((visibility("default"))) const melodee_companion_t* melodee_companion_entry(uint32_t) {
    return is("abi") ? nullptr : &table;
}
