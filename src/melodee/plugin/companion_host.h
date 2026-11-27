// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The process companion screens run in (sdk/melodee-screen/melodee_companion.h): Melodee's own binary started again
// as `melodee --melodee-companion-helper`, which loads one companion library at a time and draws one screen at a time
// (plugin/companion_protocol.h). CompanionHost is Melodee's end: it starts the helper when a screen first needs it,
// asks it to draw with a time limit, and kills it when it crashes, hangs, floods or answers what it should not. Only
// one thread uses it at a time (the rack's screen thread, or destroy() while that thread is kept out). Linux only.
#pragma once
#include "melodee/plugin/companion_protocol.h"
#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

namespace melodee {

class CompanionHost {
public:
    // `program`: the binary to start (Melodee's own, /proc/self/exe; a test's build/melodee).
    explicit CompanionHost(std::string program = "/proc/self/exe");
    ~CompanionHost();
    CompanionHost(const CompanionHost&) = delete;
    CompanionHost& operator=(const CompanionHost&) = delete;

    // Started, if it is not, with the shared block mapped. False with `why`.
    bool start(std::string& why);
    bool running() const { return pid > 0; }
    companion::Block* block() { return shared; }
    // The screen for `session`, made from `library` with `params` handles, unless it is the one open now (its
    // parameters' copies must be in block() already). False with `why`: the helper is stopped then.
    bool open(uint32_t session, const std::string& library, uint32_t params, std::string& why);
    uint32_t open_session() const { return session_open; }
    // Sends `request` and waits up to `limit` for the answer `answer` (its session's). The screen's own requests that
    // come meanwhile go to requests(). False with `why` (no answer in time, the helper gone, a message out of place):
    // the helper is stopped then.
    bool ask(const companion::Message& request, companion::Op answer, companion::Message& out,
             std::chrono::milliseconds limit, std::string& why);
    // The screen's requests sent between asks (an animation's redraw), without waiting. False: stopped (why).
    bool pump(std::string& why);
    // Edit, Program and Redraw messages received, oldest first, for whoever reads them; cleared by the reader.
    std::vector<companion::Message>& requests() { return got; }
    void close_session(uint32_t session); // its screen destroyed in the helper (no answer)
    void stop();                          // killed and waited for
    // How many datagrams the helper may send for one ask before it counts as a flood.
    static constexpr unsigned flood = 192;

private:
    bool receive(companion::Message& m, int wait_ms, bool& none, std::string& why);
    bool accept(const companion::Message& m, std::string& why);
    std::string program;
    pid_t pid = -1;
    int socket = -1;
    companion::Block* shared = nullptr;
    uint32_t session_open = 0;
    uint32_t params_open = 0;
    std::vector<companion::Message> got;
};

// The helper's own main (tools.cpp's --melodee-companion-helper): the socket on fd 3, the shared block on fd 4.
int companion_helper_main();

} // namespace melodee
