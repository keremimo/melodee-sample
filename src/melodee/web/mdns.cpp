// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/web/mdns.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace melodee::web {
namespace {
constexpr uint16_t mdns_port = 5353;
constexpr const char* mdns_group = "224.0.0.251";
constexpr uint16_t type_a = 1, type_aaaa = 28, type_nsec = 47, type_any = 255;

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void put16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& out, uint32_t v) { put16(out, uint16_t(v >> 16)); put16(out, uint16_t(v)); }

// A name at `at` in the packet, its labels joined by dots, following compression pointers. `at` moves past the
// name where it starts. False when the packet is cut short or loops.
bool read_name(const uint8_t* p, size_t size, size_t& at, std::string& name) {
    name.clear();
    size_t pos = at;
    bool jumped = false;
    for (int hops = 0; hops < 32; ++hops) {
        if (pos >= size) return false;
        uint8_t len = p[pos];
        if (len == 0) {
            if (!jumped) at = pos + 1;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= size) return false;
            if (!jumped) at = pos + 2;
            jumped = true;
            pos = size_t((len & 0x3F) << 8 | p[pos + 1]);
            continue;
        }
        if (len > 63 || pos + 1 + len > size) return false;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(p + pos + 1), len);
        pos += 1 + len;
    }
    return false;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

void put_name(std::vector<uint8_t>& out, const std::string& name) {
    size_t from = 0;
    while (from < name.size()) {
        size_t dot = name.find('.', from);
        if (dot == std::string::npos) dot = name.size();
        out.push_back(uint8_t(dot - from));
        out.insert(out.end(), name.begin() + long(from), name.begin() + long(dot));
        from = dot + 1;
    }
    out.push_back(0);
}

// The record: `name` A `address` (network order), the cache-flush bit set unless `legacy`.
void put_record(std::vector<uint8_t>& out, const std::string& name, uint32_t address, bool legacy) {
    put_name(out, name);
    put16(out, type_a);
    put16(out, legacy ? 0x0001 : 0x8001);
    put32(out, legacy ? 10 : 120);
    put16(out, 4);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&address);
    out.insert(out.end(), bytes, bytes + 4);
}

// The record that says `name` has an A record and nothing else (RFC 6762 6.1): a computer asking for AAAA is told
// at once there is none, instead of waiting seconds for an answer that never comes.
void put_nsec(std::vector<uint8_t>& out, const std::string& name) {
    put_name(out, name);
    put16(out, type_nsec);
    put16(out, 0x8001);
    put32(out, 120);
    put16(out, uint16_t(name.size() + 2 + 3)); // the next name (this one), then window 0 with one byte of bits
    put_name(out, name);
    out.push_back(0);
    out.push_back(1);
    out.push_back(0x40); // type 1, A
}

// The MPC's IPv4 address: the first interface up that is not loopback (usb0; the radios are off). 0 when none.
uint32_t own_address() {
    ifaddrs* all = nullptr;
    if (getifaddrs(&all) != 0) return 0;
    uint32_t found = 0;
    for (ifaddrs* i = all; i && !found; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK))
            found = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
    freeifaddrs(all);
    return found;
}
} // namespace

std::vector<uint8_t> mdns_answer(const uint8_t* p, size_t size, const std::string& name, uint32_t address, bool legacy) {
    if (size < 12 || (p[2] & 0x80) || !address) return {}; // a response, or too short
    unsigned questions = be16(p + 4);
    size_t at = 12;
    bool asked_a = false, asked_aaaa = false;
    for (unsigned q = 0; q < questions; ++q) {
        std::string qname;
        if (!read_name(p, size, at, qname) || at + 4 > size) return {};
        uint16_t type = be16(p + at), klass = be16(p + at + 2) & 0x7FFF; // the top bit asks for a unicast answer
        at += 4;
        if (!same_name(qname, name) || (klass != 1 && klass != 255)) continue;
        asked_a = asked_a || type == type_a || type == type_any;
        asked_aaaa = asked_aaaa || type == type_aaaa;
    }
    if (!asked_a && !asked_aaaa) return {};
    // Asked for A: the address, and beside it that there is nothing else. Asked only for AAAA: that there is none,
    // and beside it the address.
    std::vector<uint8_t> out;
    put16(out, legacy ? be16(p) : 0); // id: a legacy query's own, else 0
    put16(out, 0x8400);               // a response, authoritative
    put16(out, legacy ? 1 : 0);
    put16(out, 1);
    put16(out, 0);
    put16(out, 1);
    if (legacy) { put_name(out, name); put16(out, asked_a ? type_a : type_aaaa); put16(out, 1); }
    if (asked_a) {
        put_record(out, name, address, legacy);
        put_nsec(out, name);
    } else {
        put_nsec(out, name);
        put_record(out, name, address, legacy);
    }
    return out;
}

std::vector<uint8_t> mdns_announce(const std::string& name, uint32_t address) {
    std::vector<uint8_t> out;
    put16(out, 0);
    put16(out, 0x8400);
    put16(out, 0);
    put16(out, 1);
    put16(out, 0);
    put16(out, 0);
    put_record(out, name, address, false);
    return out;
}

bool Mdns::start(std::string& error) {
    stop();
    socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) { error = std::strerror(errno); return false; }
    int on = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)); // beside systemd-resolved, which holds 5353 too
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(mdns_port);
    // The group joined, and sent to, on usb0 by its address: the MPC has no default route, so the kernel picks no
    // interface by itself (ENODEV).
    in_addr here {};
    here.s_addr = own_address();
    if (!here.s_addr) { error = "no network"; ::close(socket_fd); socket_fd = -1; return false; }
    ip_mreq group {};
    inet_pton(AF_INET, mdns_group, &group.imr_multiaddr);
    group.imr_interface = here;
    int ttl = 255;
    if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0
        || setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0
        || setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &here, sizeof(here)) != 0
        || setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
        error = std::strerror(errno);
        ::close(socket_fd);
        socket_fd = -1;
        return false;
    }
    stopping.store(false);
    thread = std::thread([this] { run(); });
    return true;
}

void Mdns::stop() {
    stopping.store(true);
    if (thread.joinable()) thread.join();
    if (socket_fd >= 0) ::close(socket_fd);
    socket_fd = -1;
}

void Mdns::run() {
    sockaddr_in group {};
    group.sin_family = AF_INET;
    group.sin_port = htons(mdns_port);
    inet_pton(AF_INET, mdns_group, &group.sin_addr);
    auto send_to = [&](const std::vector<uint8_t>& packet, const sockaddr_in& to) {
        if (!packet.empty()) ::sendto(socket_fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    };
    // Two announcements a second apart (RFC 6762 8.3), so a computer that cached nothing yet sees the name at once.
    auto next_announce = std::chrono::steady_clock::now();
    int announcements = 2;
    uint8_t packet[1500];
    while (!stopping.load()) {
        if (announcements && std::chrono::steady_clock::now() >= next_announce) {
            send_to(mdns_announce(mdns_name, own_address()), group);
            --announcements;
            next_announce += std::chrono::seconds(1);
        }
        pollfd wait {socket_fd, POLLIN, 0};
        if (::poll(&wait, 1, 250) <= 0) continue;
        sockaddr_in from {};
        socklen_t from_size = sizeof(from);
        ssize_t got = ::recvfrom(socket_fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (got <= 0) continue;
        const bool legacy = ntohs(from.sin_port) != mdns_port;
        auto reply = mdns_answer(packet, size_t(got), mdns_name, own_address(), legacy);
        send_to(reply, legacy ? from : group);
    }
}

} // namespace melodee::web
