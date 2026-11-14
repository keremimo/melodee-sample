// SPDX-License-Identifier: AGPL-3.0-only
// The web page's server (src/melodee/web/web_server.h) on a free port: what it refuses, and a body read whole.
#include "melodee/web/mdns.h"
#include "melodee/web/web_server.h"
#include <cstring>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace melodee::web;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

// Sends `request` raw, then `body_bytes` bytes of 'x', and returns everything the server answered.
std::string ask(uint16_t port, const std::string& request, size_t body_bytes = 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return {}; }
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string chunk(8192, 'x');
    for (size_t sent = 0; sent < body_bytes;) {
        ssize_t n = ::send(fd, chunk.data(), std::min(chunk.size(), body_bytes - sent), MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += size_t(n);
    }
    std::string out;
    char buffer[4096];
    for (ssize_t n; (n = ::recv(fd, buffer, sizeof(buffer), 0)) > 0;) out.append(buffer, size_t(n));
    ::close(fd);
    return out;
}

bool status_is(const std::string& reply, int status) { return reply.rfind("HTTP/1.1 " + std::to_string(status) + " ", 0) == 0; }
std::string body_of(const std::string& reply) { auto at = reply.find("\r\n\r\n"); return at == std::string::npos ? "" : reply.substr(at + 4); }
} // namespace

int main() {
    check(json_text("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "json_text escapes");
    check(url_decode("Melodee%20Poly+v1.zip") == "Melodee Poly v1.zip", "url_decode");
    check(host_allowed("192.168.155.1:8080") && host_allowed("192.168.155.1") && host_allowed("localhost:8080")
          && host_allowed("[::1]:8080") && host_allowed("melodee.local") && host_allowed("MPC-Sample.local:80"),
          "host_allowed takes addresses and .local names");
    check(!host_allowed("") && !host_allowed("evil.example") && !host_allowed("evil.example:8080")
          && !host_allowed("192.168.155.1.evil.example") && !host_allowed("melodee.local.evil.example")
          && !host_allowed(".local"), "host_allowed refuses names");

    // melodee.local's answers: an A question gets the address, anything else nothing.
    const uint32_t address = htonl(0xC0A89B01); // 192.168.155.1
    auto query = [](const char* name, uint16_t type, uint16_t id) {
        std::vector<uint8_t> q = {uint8_t(id >> 8), uint8_t(id), 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
        std::string n = name;
        for (size_t from = 0; from < n.size();) {
            size_t dot = n.find('.', from);
            if (dot == std::string::npos) dot = n.size();
            q.push_back(uint8_t(dot - from));
            q.insert(q.end(), n.begin() + long(from), n.begin() + long(dot));
            from = dot + 1;
        }
        q.push_back(0);
        q.push_back(0); q.push_back(uint8_t(type)); q.push_back(0x80); q.push_back(1); // the unicast-answer bit set
        return q;
    };
    auto a = query("Melodee.LOCAL", 1, 0x1234);
    auto reply = mdns_answer(a.data(), a.size(), mdns_name, address, false);
    const size_t a_record = 15 + 10 + 4, nsec = 15 + 10 + 15 + 3;
    check(reply.size() == 12 + a_record + nsec && reply[2] == 0x84 && reply[7] == 1 && reply[11] == 1 && reply[4] == 0 && reply[5] == 0
          && std::memcmp(reply.data() + 12 + a_record - 4, &address, 4) == 0 && reply[0] == 0 && reply[1] == 0
          && reply[12 + a_record + 15 + 1] == 47 && reply.back() == 0x40, "mDNS answers A, with NSEC beside it");
    auto legacy = mdns_answer(a.data(), a.size(), mdns_name, address, true);
    check(legacy.size() > reply.size() && legacy[0] == 0x12 && legacy[1] == 0x34 && legacy[5] == 1, "mDNS legacy answer keeps id and question");
    auto aaaa = query("melodee.local", 28, 0);
    auto none = mdns_answer(aaaa.data(), aaaa.size(), mdns_name, address, false);
    check(none.size() == 12 + nsec + a_record && none[12 + 15 + 1] == 47
          && std::memcmp(none.data() + none.size() - 4, &address, 4) == 0, "mDNS answers AAAA with NSEC: there is none");
    auto other = query("mpc-sample.local", 1, 0);
    check(mdns_answer(other.data(), other.size(), mdns_name, address, false).empty(), "mDNS leaves other names alone");
    a[2] = 0x84;
    check(mdns_answer(a.data(), a.size(), mdns_name, address, false).empty(), "mDNS ignores responses");
    check(mdns_answer(a.data(), 5, mdns_name, address, false).empty(), "mDNS ignores a cut packet");
    check(safe_name("Melodee-MPC-Sample-v0.3.0.zip", ".zip") == "Melodee-MPC-Sample-v0.3.0.zip", "safe_name keeps a name");
    check(safe_name("Poly.ZIP", ".zip") == "Poly.ZIP", "safe_name any case");
    check(safe_name("../autorun.zip", ".zip").empty() && safe_name("a/b.zip", ".zip").empty()
          && safe_name(".hidden.zip", ".zip").empty() && safe_name("a.zip.exe", ".zip").empty()
          && safe_name(".zip", ".zip").empty() && safe_name("a\nb.zip", ".zip").empty()
          && safe_name(std::string(300, 'a') + ".zip", ".zip").empty(), "safe_name refuses");

    Server server;
    std::string error;
    bool started = server.start(0, [](const Request& request, const Body& body) {
        if (request.path == "/early") return Reply {409, "text/plain", "early"}; // the body left unread
        uint64_t got = 0;
        bool all_x = true;
        char buffer[3000];
        for (long n; (n = body(buffer, sizeof(buffer))) > 0;) {
            for (long i = 0; i < n; ++i) all_x = all_x && buffer[i] == 'x';
            got += uint64_t(n);
        }
        auto name = request.query.count("name") ? request.query.at("name") : std::string("-");
        return Reply {200, "text/plain", request.method + " " + request.path + " " + name + " " + std::to_string(got) + (all_x ? " x" : " ?")};
    }, error);
    check(started, "server starts on a free port");
    if (!started) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    const uint16_t port = server.port();
    const std::string host = "Host: 192.168.155.1:8080\r\n";

    auto r = ask(port, "GET /status HTTP/1.1\r\n" + host + "\r\n");
    check(status_is(r, 200) && body_of(r) == "GET /status - 0 x", "GET answered");
    r = ask(port, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n");
    check(status_is(r, 421), "a named Host is refused");
    r = ask(port, "GET / HTTP/1.1\r\n\r\n");
    check(status_is(r, 421), "no Host is refused");
    r = ask(port, "OPTIONS /plugin HTTP/1.1\r\n" + host + "\r\n");
    check(status_is(r, 405), "OPTIONS is refused");
    r = ask(port, "POST /plugin?name=a.zip HTTP/1.1\r\n" + host + "Content-Length: 10\r\n\r\n", 10);
    check(status_is(r, 403), "POST without X-Melodee is refused");
    r = ask(port, "POST /plugin?name=a.zip HTTP/1.1\r\n" + host + "X-Melodee: 1\r\nTransfer-Encoding: chunked\r\n\r\n");
    check(status_is(r, 411), "chunked is refused");
    r = ask(port, "POST /plugin?name=a.zip HTTP/1.1\r\n" + host + "X-Melodee: 1\r\n\r\n");
    check(status_is(r, 411), "POST without a length is refused");
    r = ask(port, "POST /plugin?name=Melodee%20Poly.zip HTTP/1.1\r\n" + host + "X-Melodee: 1\r\nContent-Length: 1000000\r\n\r\n", 1000000);
    check(status_is(r, 200) && body_of(r) == "POST /plugin Melodee Poly.zip 1000000 x", "a body read whole");
    r = ask(port, "POST /early HTTP/1.1\r\n" + host + "X-Melodee: 1\r\nContent-Length: 200000\r\n\r\n", 200000);
    check(status_is(r, 409) && body_of(r) == "early", "a reply before the body still arrives");
    r = ask(port, "HEAD /status HTTP/1.1\r\n" + host + "\r\n");
    check(status_is(r, 200) && body_of(r).empty() && r.find("Content-Length: 18") != std::string::npos, "HEAD has no body");
    r = ask(port, "GARBAGE\r\n\r\n");
    check(status_is(r, 400), "a bad request line");
    r = ask(port, "GET / HTTP/1.1\r\n" + host + "X-Big: " + std::string(20000, 'a') + "\r\n\r\n");
    check(status_is(r, 400), "a head over 16 KB");
    server.stop();
    if (failures) return 1;
    std::printf("web server ok\n");
    return 0;
}
