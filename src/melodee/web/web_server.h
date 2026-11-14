// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

// A small HTTP/1.1 server for Melodee's web page (web/web_app.h), on its own
// thread, one connection at a time, each closed after its reply. It runs on
// the MPC's USB-C network (usb0, 192.168.155.1): the MPC's radios are off, so
// nothing else reaches it. It takes port 80 from the stock az0x-webserver.
//
// What it refuses before a handler sees the request:
// - a head over 16 KB, a bad request line, or a body sent chunked (411)
// - a Host that is not an IP address, localhost or a .local name (multicast
//   DNS, web/mdns.h): a web site whose name points at the MPC's address
//   (DNS rebinding) gets nothing (421)
// - anything but GET or HEAD without an `X-Melodee` header: a page on
//   another site cannot add it without a CORS preflight, and OPTIONS is
//   refused (405), so only Melodee's own page can upload (403)
namespace melodee::web {

struct Request {
    std::string method, path;                  // "POST", "/plugin"
    std::map<std::string, std::string> query;  // decoded ?name=value pairs
    std::map<std::string, std::string> headers; // names in lower case
    uint64_t length = 0;                       // Content-Length; 0 without one
};

struct Reply {
    int status = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
};

// Reads up to `size` bytes of the request's body into `into`: the bytes read,
// 0 once all `length` are read, -1 when the computer stopped sending.
using Body = std::function<long(char* into, size_t size)>;
using Handler = std::function<Reply(const Request&, const Body&)>;

class Server {
public:
    ~Server() { stop(); }
    // Listens on every address at `port` (0: any free port, for tests) and
    // serves on a new thread. False with `error` when the port will not open.
    bool start(uint16_t port, Handler handler, std::string& error);
    void stop();
    uint16_t port() const { return bound; }

private:
    void run();
    void serve(int client);
    int listener = -1;
    uint16_t bound = 0;
    Handler handler;
    std::atomic<bool> stopping {false};
    std::thread thread;
};

// Helpers the handlers share, kept here so the tests reach them without JUCE.
std::string json_text(const std::string& text);       // "a \"b\"" with its quotes
std::string url_decode(const std::string& text);      // %20 and + as a space
bool host_allowed(const std::string& host);           // an IP literal, localhost or a .local name, any port
// A file name a computer sent, as one safe name in a folder: no folders, no
// leading dot, no control characters, at most 200 bytes, and ending in
// `extension` (any case). Empty when it is none of that.
std::string safe_name(const std::string& name, const char* extension);

} // namespace melodee::web
