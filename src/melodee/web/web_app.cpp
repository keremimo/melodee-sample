// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/web/web_app.h"
#include "melodee/web/web_bundle.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/shell/update.h"
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace melodee::web {
namespace fs = std::filesystem;
namespace {
constexpr uint64_t update_limit = uint64_t(1) << 30;       // sd_update::zips_at() looks at nothing bigger
constexpr uint64_t plugin_limit = (uint64_t(4) << 30) - 1; // a FAT32 card's largest file
constexpr uint64_t room_margin = uint64_t(8) << 20;

Reply fail(int status, const std::string& words) { return {status, "application/json; charset=utf-8", "{\"error\":" + json_text(words) + "}"}; }

// `name` in `folder`, else "<stem> 2.zip", "<stem> 3.zip"... so nothing already there is written over.
fs::path free_path(const fs::path& folder, const std::string& name) {
    std::error_code error;
    fs::path path = folder / name;
    for (int n = 2; fs::exists(path, error) && n < 1000; ++n)
        path = folder / (fs::path(name).stem().string() + " " + std::to_string(n) + fs::path(name).extension().string());
    return path;
}

// The request's body into `path`, flushed to the card. False, with nothing left behind, when it stopped short.
bool receive(const Body& body, uint64_t length, const fs::path& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::vector<char> buffer(256 * 1024);
    uint64_t written = 0;
    bool ok = true;
    while (ok && written < length) {
        long got = body(buffer.data(), buffer.size());
        if (got <= 0) { ok = false; break; }
        for (long at = 0; at < got;) {
            ssize_t n = ::write(fd, buffer.data() + at, size_t(got - at));
            if (n <= 0) { ok = false; break; }
            at += n;
        }
        written += uint64_t(got);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    std::error_code error;
    if (!ok) fs::remove(path, error);
    return ok;
}

// Room on the card for `length` bytes, kept back from the end.
bool room_for(const fs::path& folder, uint64_t length) {
    std::error_code error;
    auto space = fs::space(folder, error);
    return !error && space.available >= length + room_margin;
}

const char* phase_word(uint8_t phase) {
    switch (UpdatePhase(phase)) {
    case UpdatePhase::Saving: return "saving";
    case UpdatePhase::Checking: return "checking";
    case UpdatePhase::Writing: return "writing";
    case UpdatePhase::Starting: return "starting";
    case UpdatePhase::Failed: return "failed";
    default: return "none";
    }
}
} // namespace

bool WebApp::start(std::string& error) {
    {
        std::lock_guard<std::mutex> held(lock);
        closing = false;
    }
    auto serve = [this](const Request& request, const Body& body) { return handle(request, body); };
    if (!server.start(port, serve, error) && !server.start(spare_port, serve, error)) return false;
    std::string name_error; // without the name the page still opens at the address
    if (!mdns.start(name_error)) std::fprintf(stderr, "melodee: %s not answered: %s\n", mdns_name, name_error.c_str());
    return true;
}

void WebApp::stop() {
    {
        std::lock_guard<std::mutex> held(lock);
        closing = true;
    }
    published.notify_all();
    mdns.stop();
    server.stop();
}

void WebApp::publish(const Facts& now) {
    {
        std::lock_guard<std::mutex> held(lock);
        facts = now;
    }
    published.notify_all();
}

bool WebApp::take_note(std::string& text) {
    std::lock_guard<std::mutex> held(lock);
    if (note.empty()) return false;
    text.swap(note);
    note.clear();
    return true;
}

Facts WebApp::current() {
    std::lock_guard<std::mutex> held(lock);
    return facts;
}

Reply WebApp::handle(const Request& request, const Body& body) {
    const bool get = request.method == "GET" || request.method == "HEAD";
    if (get && request.path == "/status") return status();
    if (get && request.path == "/plugins") return plugins();
    if (request.method == "POST" && request.path == "/update") return upload_update(request, body);
    if (request.method == "POST" && request.path == "/plugin") return upload_plugin(request, body);
    if (get)
        if (auto file = bundled(request.path)) return {200, file.type, std::string(file.data, file.size)};
    if (get && request.path == "/favicon.ico") return {204, "image/x-icon", ""}; // the page names its own icon
    return fail(404, "Not found");
}

Reply WebApp::status() {
    Facts f = current();
    std::string out = "{\"version\":" + json_text(f.version) + ",\"card\":" + (f.card.empty() ? "false" : "true")
        + ",\"offer\":" + (f.offer_version.empty() ? std::string("null") : json_text(f.offer_version))
        + ",\"phase\":" + json_text(phase_word(f.phase))
        + ",\"error\":" + json_text(update_error_text(UpdateError(f.error)))
        + ",\"card_free\":" + std::to_string(f.card_free) + ",\"card_total\":" + std::to_string(f.card_total) + "}";
    return {200, "application/json; charset=utf-8", out};
}

Reply WebApp::plugins() {
    Facts f = current();
    if (f.card.empty()) return {200, "application/json; charset=utf-8", "{\"card\":false,\"plugins\":[]}"};
    std::string out = "{\"card\":true,\"plugins\":[";
    bool first = true;
    for (const auto& p : list_plugins(plugin_folder(f.card))) {
        out += first ? "" : ",";
        first = false;
        out += "{\"name\":" + json_text(p.name) + ",\"file\":" + json_text(fs::path(p.path).filename().string())
            + ",\"format\":" + json_text(p.fixed_rate ? "OPEN MPC" : format_tag(p.format))
            + ",\"folder\":" + (p.folder ? "true" : "false") + ",\"loadable\":" + (p.loadable() ? "true" : "false")
            + ",\"why\":" + json_text(p.why) + "}";
    }
    return {200, "application/json; charset=utf-8", out + "]}"};
}

Reply WebApp::upload_update(const Request& request, const Body& body) {
    Facts f = current();
    if (f.card.empty()) return fail(409, "No SD card in the MPC, or it is lent to a computer");
    if (update_running(f.phase)) return fail(409, "An update is installing");
    auto name = request.query.count("name") ? safe_name(request.query.at("name"), ".zip") : std::string();
    if (name.empty()) return fail(400, "Choose the Melodee release .zip");
    if (request.length == 0) return fail(400, "The file is empty");
    if (request.length > update_limit) return fail(413, "Too big for a Melodee release");
    const fs::path card = f.card;
    if (!room_for(card, request.length)) return fail(507, "The SD card is full");
    const fs::path temp = card / ".melodee-upload.part";
    if (!receive(body, request.length, temp)) return fail(400, "The upload stopped before the end");
    const fs::path zip = card / name; // the same release sent again takes the place of the copy already there
    std::error_code error;
    fs::rename(temp, zip, error);
    if (error) { fs::remove(temp, error); return fail(500, "Could not write to the SD card"); }
    // The service thread checks the card again: wait for that check, at most 30 s (sd-update.sh's check reads every zip).
    std::unique_lock<std::mutex> held(lock);
    const uint64_t before = facts.scans;
    rescan.store(true);
    bool checked = published.wait_for(held, std::chrono::seconds(30), [&] { return facts.scans > before || closing; }) && !closing;
    Facts after = facts;
    held.unlock();
    if (checked && after.offer_zip == zip.string())
        return {200, "application/json; charset=utf-8", "{\"offered\":" + json_text(after.offer_version) + "}"};
    fs::remove(zip, error);
    if (!checked) return fail(500, "The MPC did not check the card");
    return fail(400, "Not a newer signed Melodee release (this MPC runs " + after.version + ")");
}

Reply WebApp::upload_plugin(const Request& request, const Body& body) {
    Facts f = current();
    if (f.card.empty()) return fail(409, "No SD card in the MPC, or it is lent to a computer");
    auto name = request.query.count("name") ? safe_name(request.query.at("name"), ".zip") : std::string();
    if (name.empty()) return fail(400, "Choose a plug-in .zip");
    if (request.length == 0) return fail(400, "The file is empty");
    if (request.length > plugin_limit) return fail(413, "Too big for the SD card (4 GB at most)");
    const fs::path folder = plugin_folder(f.card);
    std::error_code error;
    fs::create_directories(folder, error);
    if (!room_for(folder, request.length)) return fail(507, "The SD card is full");
    const fs::path temp = folder / ".melodee-upload.part"; // hidden: the picker and unzip_plugins() pass it by
    if (!receive(body, request.length, temp)) return fail(400, "The upload stopped before the end");
    const fs::path zip = free_path(folder, name);
    fs::rename(temp, zip, error);
    if (error) { fs::remove(temp, error); return fail(500, "Could not write to the SD card"); }
    auto placed = unzip_plugins(folder.string());
    if (fs::exists(zip, error)) { // it stayed: say why, as the picker would, and take it away again
        std::string why = "Won't unzip";
        for (const auto& p : list_plugins(folder.string()))
            if (p.path == zip.string() && !p.why.empty()) why = p.why;
        fs::remove(zip, error);
        return fail(why == "Card full" ? 507 : 400, why == "Card full" ? "The SD card is full" : "This zip will not unzip");
    }
    {
        std::lock_guard<std::mutex> held(lock);
        note = "Plug-in installed";
    }
    std::string out = "{\"installed\":[";
    for (size_t n = 0; n < placed.size(); ++n) out += (n ? "," : "") + json_text(placed[n]);
    return {200, "application/json; charset=utf-8", out + "]}"};
}

} // namespace melodee::web
