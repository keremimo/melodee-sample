// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/web/web_server.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace melodee::web {
namespace {
constexpr size_t head_limit = 16 * 1024;

const char* status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 421: return "Misdirected Request";
    case 507: return "Insufficient Storage";
    default: return status >= 500 ? "Internal Server Error" : "Error";
    }
}

bool send_all(int fd, const char* data, size_t size) {
    while (size) {
        ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        size -= size_t(sent);
    }
    return true;
}

Reply error_reply(int status, const std::string& words);

// A name for Content-Disposition: plain ASCII with quotes and controls taken out, and the whole name UTF-8 encoded.
std::string disposition(const std::string& name) {
    std::string plain, encoded;
    for (unsigned char c : name) {
        plain += (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') ? '_' : char(c);
        if (std::isalnum(c) || std::strchr("-._~", c)) encoded += char(c);
        else { char hex[4]; std::snprintf(hex, sizeof(hex), "%%%02X", c); encoded += hex; }
    }
    return "attachment; filename=\"" + plain + "\"; filename*=UTF-8''" + encoded;
}

void send_reply(int fd, const Reply& reply, bool head_only) {
    int file = -1;
    uint64_t length = reply.body.size();
    if (!reply.file.empty()) {
        file = ::open(reply.file.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat info {};
        if (file < 0 || ::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
            if (file >= 0) ::close(file);
            return send_reply(fd, error_reply(404, "Not found"), head_only);
        }
        length = uint64_t(info.st_size);
    }
    std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " " + status_text(reply.status) + "\r\n"
        + "Content-Type: " + reply.type + "\r\n"
        + "Content-Length: " + std::to_string(length) + "\r\n"
        + (reply.download.empty() ? std::string() : "Content-Disposition: " + disposition(reply.download) + "\r\n")
        + "Cache-Control: no-store\r\n"
        + "X-Content-Type-Options: nosniff\r\n"
        + "X-Frame-Options: DENY\r\n"
        + "Connection: close\r\n\r\n";
    if (!head_only && file < 0) out += reply.body;
    bool ok = send_all(fd, out.data(), out.size());
    if (file >= 0) {
        for (off_t at = 0; ok && !head_only && uint64_t(at) < length;) {
            ssize_t sent = ::sendfile(fd, file, &at, size_t(std::min<uint64_t>(length - uint64_t(at), 1 << 20)));
            if (sent < 0 && errno == EINTR) continue;
            ok = sent > 0;
        }
        ::close(file);
    }
}

Reply error_reply(int status, const std::string& words) { return {status, "application/json; charset=utf-8", "{\"error\":" + json_text(words) + "}"}; }

std::string lowered(std::string text) {
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trimmed(const std::string& text) {
    size_t from = text.find_first_not_of(" \t"), to = text.find_last_not_of(" \t");
    return from == std::string::npos ? std::string() : text.substr(from, to - from + 1);
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at <= query.size() && !query.empty()) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        std::string pair = query.substr(at, end - at);
        size_t eq = pair.find('=');
        if (!pair.empty()) out[url_decode(pair.substr(0, eq))] = eq == std::string::npos ? std::string() : url_decode(pair.substr(eq + 1));
        at = end + 1;
    }
    return out;
}
} // namespace

std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20 || c == 0x7f) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); out += code; }
        else out += char(c);
    }
    return out + "\"";
}

std::string url_decode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') out += ' ';
        else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1]))
                 && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += char(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else out += text[i];
    }
    return out;
}

bool host_allowed(const std::string& host) {
    if (host.empty()) return false;
    if (host[0] == '[') { // [::1]:8080
        size_t close = host.find(']');
        if (close == std::string::npos) return false;
        in6_addr address;
        return inet_pton(AF_INET6, host.substr(1, close - 1).c_str(), &address) == 1;
    }
    std::string name = lowered(host.substr(0, host.find(':')));
    if (name == "localhost") return true;
    // A .local name is answered by multicast DNS on the cable (melodee.local, mpc-sample.local), never by a web
    // site's own DNS.
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".local") == 0) return true;
    in_addr address;
    return inet_pton(AF_INET, name.c_str(), &address) == 1;
}

std::string safe_name(const std::string& name, const char* extension) {
    const size_t n = std::strlen(extension);
    if (name.size() <= n || name.size() > 200 || name[0] == '.' || name[0] == ' ') return {};
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f || std::strchr("/\\:*?\"<>|", c)) return {};
    if (lowered(name.substr(name.size() - n)) != lowered(extension)) return {};
    return name;
}

bool Server::start(uint16_t port, Handler serve_with, std::string& error) {
    stop();
    listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) { error = std::strerror(errno); return false; }
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    socklen_t size = sizeof(address);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 4) != 0
        || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        error = std::strerror(errno);
        ::close(listener);
        listener = -1;
        return false;
    }
    bound = ntohs(address.sin_port);
    handler = std::move(serve_with);
    stopping.store(false);
    thread = std::thread([this] { run(); });
    return true;
}

void Server::stop() {
    stopping.store(true);
    if (thread.joinable()) thread.join();
    if (listener >= 0) ::close(listener);
    listener = -1;
}

void Server::run() {
    while (!stopping.load()) {
        pollfd wait {listener, POLLIN, 0};
        if (::poll(&wait, 1, 250) <= 0) continue; // a quarter second at most before stop() is seen
        int client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        timeval limit {15, 0}; // a computer that goes quiet mid-upload lets the next one in after 15 s
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        serve(client);
        // Done sending. Read what the computer still sends for a moment (a body the reply came before, the rest of a
        // head too big), so it reads the reply instead of a reset; it closes its side once it has the reply.
        ::shutdown(client, SHUT_WR);
        timeval brief {2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &brief, sizeof(brief));
        char rest[4096];
        for (int n = 0; n < 256 && ::recv(client, rest, sizeof(rest), 0) > 0; ++n) {}
        ::close(client);
    }
}

void Server::serve(int client) {
    // The head: the request line and headers, up to the blank line. What came after it is the body's start.
    std::string head;
    size_t end = std::string::npos;
    char chunk[4096];
    while (end == std::string::npos) {
        if (head.size() > head_limit) return send_reply(client, error_reply(400, "Request too large"), false);
        ssize_t got = ::recv(client, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return;
        head.append(chunk, size_t(got));
        end = head.find("\r\n\r\n");
    }
    if (end > head_limit) return send_reply(client, error_reply(400, "Request too large"), false);
    std::string extra = head.substr(end + 4);
    head.resize(end);

    Request request;
    size_t line_end = head.find("\r\n");
    std::string line = head.substr(0, line_end);
    size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string::npos || b == a || line.compare(b + 1, 5, "HTTP/") != 0)
        return send_reply(client, error_reply(400, "Bad request"), false);
    request.method = line.substr(0, a);
    std::string target = line.substr(a + 1, b - a - 1);
    size_t mark = target.find('?');
    request.path = url_decode(target.substr(0, mark));
    if (mark != std::string::npos) request.query = parse_query(target.substr(mark + 1));
    size_t at = line_end == std::string::npos ? head.size() : line_end + 2;
    while (at < head.size()) {
        size_t next = head.find("\r\n", at);
        if (next == std::string::npos) next = head.size();
        std::string field = head.substr(at, next - at);
        size_t colon = field.find(':');
        if (colon != std::string::npos) request.headers[lowered(trimmed(field.substr(0, colon)))] = trimmed(field.substr(colon + 1));
        at = next + 2;
    }

    const bool head_only = request.method == "HEAD";
    if (!host_allowed(request.headers["host"])) return send_reply(client, error_reply(421, "Open this page by the MPC's address"), head_only);
    if (request.method != "GET" && request.method != "HEAD" && request.method != "POST")
        return send_reply(client, error_reply(405, "Not allowed"), head_only);
    if (request.method == "POST") {
        if (!request.headers.count("x-melodee")) return send_reply(client, error_reply(403, "Not from Melodee's page"), false);
        if (request.headers.count("transfer-encoding")) return send_reply(client, error_reply(411, "Send a length"), false);
        auto length = request.headers.find("content-length");
        if (length == request.headers.end() || length->second.empty()
            || !std::all_of(length->second.begin(), length->second.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return send_reply(client, error_reply(411, "Send a length"), false);
        request.length = std::strtoull(length->second.c_str(), nullptr, 10);
    }

    uint64_t left = request.length;
    Body body = [&](char* into, size_t size) -> long {
        if (left == 0) return 0;
        size = size_t(std::min<uint64_t>(std::min<size_t>(size, 1 << 20), left));
        if (!extra.empty()) {
            size_t n = std::min(size, extra.size());
            std::memcpy(into, extra.data(), n);
            extra.erase(0, n);
            left -= n;
            return long(n);
        }
        for (;;) {
            ssize_t got = ::recv(client, into, size, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return -1;
            left -= uint64_t(got);
            return long(got);
        }
    };
    Reply reply;
    try {
        reply = handler(request, body);
    } catch (const std::exception& e) {
        reply = error_reply(500, e.what());
    }
    send_reply(client, reply, head_only);
}

} // namespace melodee::web
