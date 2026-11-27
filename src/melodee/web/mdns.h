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
// http://melodee.local. A questions for that name are answered, with the
// MPC's IPv4 address on the interface the question came in on. While File
// sharing serves the card (base drivers/file_share.h), the SMB server is
// offered too (DNS-SD, RFC 6763): "Melodee" of type _smb._tcp, port 445 on
// melodee.local, which a computer's file browser lists by itself.
// systemd-resolved keeps answering for the MPC's own name (mpc-sample.local)
// on the same port: both sockets share it (SO_REUSEADDR), and each multicast
// packet reaches both. It stops with Melodee, so the name goes with it.
namespace melodee::web {

constexpr const char* mdns_name = "melodee.local";
constexpr const char* mdns_smb_type = "_smb._tcp.local";
constexpr const char* mdns_smb_name = "Melodee._smb._tcp.local";

class Mdns {
public:
    ~Mdns() { stop(); }
    bool start(std::string& error); // its own thread; announces the name twice, a second apart
    void stop();
    // Any thread: offer the SMB server or not. On announces it twice; off says goodbye (TTL 0).
    void share_files(bool on) { smb.store(on); }

private:
    void run();
    int socket_fd = -1;
    std::atomic<bool> stopping {false}, smb {false};
    std::thread thread;
};

// The answer to one mDNS packet `query` (`size` bytes) for `name` at
// `address` (network order), or empty when it asks nothing of `name`.
// `legacy`: a one-shot query from a port other than 5353, answered with
// its id and question, as RFC 6762 6.7 says. `smb`: the SMB server is
// offered, so questions for _smb._tcp, for "Melodee" of that type and for
// the list of types (_services._dns-sd._udp) are answered too. Plain C++,
// for the tests.
std::vector<uint8_t> mdns_answer(const uint8_t* query, size_t size, const std::string& name, uint32_t address, bool legacy, bool smb = false);
// An unsolicited announcement of `name` at `address`, with the SMB server's
// records when `smb`; `goodbye`: those records' end (TTL 0) instead.
std::vector<uint8_t> mdns_announce(const std::string& name, uint32_t address, bool smb = false, bool goodbye = false);

} // namespace melodee::web
