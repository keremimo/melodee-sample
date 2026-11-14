// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Answers multicast DNS (RFC 6762) for one name, "melodee.local", so a
// computer on the MPC's USB-C network opens the web page (web/web_app.h) at
// http://melodee.local. Only A questions for that name are answered, with
// the MPC's IPv4 address on the interface the question came in on.
// systemd-resolved keeps answering for the MPC's own name (mpc-sample.local)
// on the same port: both sockets share it (SO_REUSEADDR), and each multicast
// packet reaches both. It stops with Melodee, so the name goes with it.
namespace melodee::web {

constexpr const char* mdns_name = "melodee.local";

class Mdns {
public:
    ~Mdns() { stop(); }
    bool start(std::string& error); // its own thread; announces the name twice, a second apart
    void stop();

private:
    void run();
    int socket_fd = -1;
    std::atomic<bool> stopping {false};
    std::thread thread;
};

// The answer to one mDNS packet `query` (`size` bytes) for `name` at
// `address` (network order), or empty when it asks nothing of `name`.
// `legacy`: a one-shot query from a port other than 5353, answered with
// its id and question, as RFC 6762 6.7 says. Plain C++, for the tests.
std::vector<uint8_t> mdns_answer(const uint8_t* query, size_t size, const std::string& name, uint32_t address, bool legacy);
// An unsolicited announcement of `name` at `address`.
std::vector<uint8_t> mdns_announce(const std::string& name, uint32_t address);

} // namespace melodee::web
