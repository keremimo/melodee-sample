// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/melodee_app.h"
#include "melodee/crash_log.h"
#include "drivers/card_share.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "surface/pads.h"
#include "drivers/power_button.h"
#include "drivers/sd_update.h"
#include "melodee/shell/update.h"
#include "melodee/pages/page.h"
#include "melodee/ui/paint.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>

extern char** environ;

namespace {
// Service thread: asks systemd to turn the MPC off. Only on the MPC itself
// (its stock binary is there), never on a computer running a host build. systemd stops this service first (the
// recovery script sees the system stopping and leaves the stock app off), then unmounts the card and powers down.
bool request_power_off() {
    if (access("/usr/bin/MPC Sample", F_OK) != 0) return false;
    sync(); // the project and settings on the card before anything stops
    char systemctl[] = "/bin/systemctl", no_block[] = "--no-block", poweroff[] = "poweroff";
    char* argv[] = {systemctl, no_block, poweroff, nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, systemctl, nullptr, nullptr, argv, environ) != 0) return false;
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0; // --no-block: it only queues the job
}

// Service thread: stops the stock web server (Akai's updater, port 80) so the web page (web/web_app.h) takes port 80.
// Only on the MPC. The base's restore-stock.sh starts it again whenever Melodee stops, a crash too.
void stop_stock_web_server() {
    if (access("/usr/bin/MPC Sample", F_OK) != 0) return;
    char systemctl[] = "/bin/systemctl", stop[] = "stop", unit[] = "az0x-webserver.service";
    char* argv[] = {systemctl, stop, unit, nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, systemctl, nullptr, nullptr, argv, environ) != 0) return;
    int status = 0;
    waitpid(pid, &status, 0);
}
} // namespace


MelodeeApp::MelodeeApp()
    : shell(host), library(host), plugins(host), printer(&library), frame(std::make_unique<melodee::Snapshot>()) {
    melodee::crash_log::install(); // before the threads start, so a crash on any of them leaves a report on the card
    shell.set_library(&library);
    shell.set_plugins(&plugins);
    shell.set_printer(&printer);
    printer.set_pager(&host.streams()); // freeze plan F2b: prints are read through the instruments' pager
    shell.set_store(&store);
    shell.set_input(&input_route);
    // The card's settings now, not first on the service thread: the runtime opens the audio device with
    // audio_format() as it starts, and Settings' rate from that first open spares a reopen moments later
    // (the codec powered down and up again, its mute shut a second, and the USB-C gadget rebound twice).
    const std::string projects = melodee::ProjectStore::projects_root(Platform().storage());
    if (!projects.empty()) melodee::crash_log::set_folder(std::filesystem::path(projects).parent_path().string() + "/Crashes");
    shell.start_settings(melodee::ProjectStore::read_settings_file(projects).settings);
    host.load(shell.project(), shell.patterns());
    // The tracks render on several cores (roadmap P2). Floating: a worker for each core beside the audio thread, every
    // one free to run anywhere. Pinned: the audio thread on core 1 and two workers on cores 2 and 3, the rest of Melodee
    // kept on core 0 (keep_rest_off()). $MELODEE_RENDER_THREADS: "pinned", or how many floating workers.
    unsigned cores = std::max(std::thread::hardware_concurrency(), 1u);
    bool pinned = render_pinned_default && cores >= 4;
    unsigned workers = pinned ? 2u : std::min(cores - 1, 3u);
    if (const char* own = std::getenv("MELODEE_RENDER_THREADS")) {
        pinned = std::string(own) == "pinned" && cores >= 4;
        workers = pinned ? 2u : unsigned(std::clamp(std::atoi(own), 0, 7));
    }
    host.set_render_threads(workers, 150000, pinned);
    host.set_overload_guard(true); // near a block's time, voices go before a dropout comes (roadmap P2)
    host.set_parking(true);        // a project load fades out and builds off the audio thread (Context::send_reload)
}

// A dropout (roadmap P2): the busiest track as it came, its share of a block (both held at their recent peak).
void MelodeeApp::note_dropout() {
    unsigned xruns = session().audio.xruns.load(std::memory_order_relaxed);
    if (xruns == xruns_seen) return;
    xruns_seen = xruns;
    int t = host.busiest_track();
    dropout_track.store(t >= 0 ? unsigned(t) + 1 : 0, std::memory_order_relaxed);
    dropout_load.store(t >= 0 ? unsigned(std::lround(host.track_load(unsigned(t)) * 1000)) : 0, std::memory_order_relaxed);
}

// Pinned (roadmap P2): every thread of Melodee's but the audio thread and the render workers onto core 0, those
// already running and, every two seconds, any started since (a thread starts where the one that made it was, so
// most are born there). Service thread.
void MelodeeApp::keep_rest_off() {
#if defined(__linux__)
    if (!host.render_pinned()) return;
    uint32_t now = now_ms();
    if (pinned_at && now - pinned_at < 2000) return;
    pinned_at = now ? now : 1;
    cpu_set_t first;
    CPU_ZERO(&first);
    CPU_SET(0, &first);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        int tid = std::atoi(entry.path().filename().c_str());
        if (tid > 0 && !host.rendering_thread(tid)) sched_setaffinity(pid_t(tid), sizeof first, &first);
    }
#endif
}

// MIDI out and sync are timed on the device's own clock: its rate and the frames it keeps queued. A new rate
// silences the engine until the shell has sent the project again; USB-C in is converted to the rate.
void MelodeeApp::audio_opened(const AudioFormat& format) {
    host.set_audio_format(format.rate, format.latency_frames());
    input_route.set_device_rate(format.rate);
}

uint32_t MelodeeApp::now_ms() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

static_assert(usb_stem_count == melodee::track_count && usb_stem_frames == melodee::EngineHost::block && audio_frames <= usb_stem_frames,
              "one USB-C stem a track, a block at a time");

void MelodeeApp::render(const AudioBlock& input, AudioBlock& output) {
    host.render(output.data(), audio_frames, shell.volume(), input.data(), &stem_block.track[0][0], input_route.usb_block(audio_frames));
}

void MelodeeApp::surface(const SurfaceEvent& event) {
    // A remote tool's prompt owns the controls while it is shown.
    if (session().prompting.load() && event.input.kind == SurfaceInput::Kind::Button) return;
    // A press during the boot animation fades it out, and still counts.
    bool pressed = (event.input.kind == SurfaceInput::Kind::Button && event.input.pressed) ||
                   (event.pad >= 0 && (event.midi.status & 0xf0) == 0x90 && event.midi.value > 0);
    if (pressed && !event.syncing) boot_skip.store(true, std::memory_order_relaxed);
    shell.surface(event, now_ms());
}

void MelodeeApp::midi(const MidiPortInfo& port, const MidiPacket& packet) { shell.midi(port, packet, now_ms()); }
void MelodeeApp::midi_lost() { shell.midi_lost(); }
void MelodeeApp::midi_output(MidiOutput& out, Clock::time_point now) {
    RemoteMidi injected;
    while (remote_midi.pop(injected)) {
        MidiPortInfo port;
        port.address = "remote"; port.name = "melodee-midi"; port.input = true;
        port.driver = injected.usb ? "MIDI Gadget" : "SerialMIDI";
        injected.packet.timestamp_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        shell.midi(port, injected.packet, now_ms());
    }
    if (panic_asked.exchange(false)) shell.midi_panic(now_ms());
    shell.midi_output(out, now, now_ms());
}

void MelodeeApp::lights(SurfaceLights& lights, Clock::time_point) {
    auto remote_ns = [] { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); };
    melodee::LooperAsk looper_ask;
    while (remote_looper.pop(looper_ask)) shell.looper_ask(looper_ask, now_ms());
    melodee::Context::PrintAsk print_ask;
    while (remote_print.pop(print_ask)) shell.print(print_ask);
    Remote remote;
    while (presses.pop(remote)) {
        if (remote.kind == 1) { // the wheel
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control; event.input.index = uint8_t(SurfaceControl::Wheel); event.input.delta = remote.delta;
            shell.surface(event, now_ms());
            continue;
        }
        auto press = [&](uint8_t index, bool down) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button; event.input.index = index; event.input.pressed = down;
            shell.surface(event, now_ms());
        };
        if (remote.kind == 3) { // a pot moved to a position; the top bit holds Shift around it
            bool shifted = remote.index & 0x80;
            if (shifted) press(uint8_t(SurfaceButton::Shift), true);
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control; event.input.index = remote.index & 0x7f; event.input.value = uint8_t(remote.delta);
            shell.surface(event, now_ms());
            if (shifted) press(uint8_t(SurfaceButton::Shift), false);
            continue;
        }
        if (remote.kind == 4) { press(remote.index, remote.delta != 0); continue; } // hold-BUTTON, release-BUTTON
        if (remote.kind == 5) { // hold-padN, release-padN
            SurfaceEvent event;
            bool down = remote.delta != 0;
            event.pad = remote.index;
            event.timestamp_ns = remote_ns(); // timed as a hand's, to the frame
            event.midi = {uint8_t(down ? 0x99 : 0x89), surface_pad_notes[remote.index % 16], uint8_t(down ? 100 : 0)};
            shell.surface(event, now_ms());
            continue;
        }
        if (remote.kind == 2) { // a pad, struck and let go; delta - 1 is a button held around it (Shift, Erase, ...)
            int held = remote.delta - 1;
            auto hold = [&](bool down) {
                SurfaceEvent event;
                event.input.kind = SurfaceInput::Kind::Button; event.input.index = uint8_t(held); event.input.pressed = down;
                shell.surface(event, now_ms());
            };
            bool holding = held >= 0;
            if (holding) hold(true);
            for (bool down : {true, false}) {
                SurfaceEvent event;
                event.pad = remote.index;
                event.midi = {uint8_t(down ? 0x99 : 0x89), surface_pad_notes[remote.index % 16], uint8_t(down ? 100 : 0)};
                event.timestamp_ns = remote_ns();
                shell.surface(event, now_ms());
            }
            if (holding) hold(false);
            continue;
        }
        uint8_t button = remote.index;
        // A remote press, as if by hand: down, then up; the top bit holds Shift around it.
        auto send = [&](uint8_t index, bool down) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button; event.input.index = index; event.input.pressed = down;
            shell.surface(event, now_ms());
        };
        bool shifted = button & 0x80;
        if (shifted) send(uint8_t(SurfaceButton::Shift), true);
        send(button & 0x7f, true); send(button & 0x7f, false);
        if (shifted) send(uint8_t(SurfaceButton::Shift), false);
    }
    shell.lights(lights, now_ms());
}

std::string MelodeeApp::command(const std::string& action, const std::string& argument, Hardware&) {
    if (action == "melodee-render") { // melodee-render serial|parallel: the render workers left asleep or used again
        if (argument == "serial" || argument == "parallel") host.set_render_parallel(argument == "parallel");
        else if (!argument.empty()) return "ERR melodee-render serial|parallel";
        return std::string("OK melodee-render ") + (host.render_parallel() ? "parallel" : "serial");
    }
    if (action == "melodee-unfreeze") { // freeze plan F2b: the track plays live again at once (its prints are kept)
        int track = std::atoi(argument.c_str());
        if (track < 1 || track > int(melodee::track_count)) return "ERR melodee-unfreeze TRACK";
        melodee::Context::PrintAsk ask;
        ask.track = uint8_t(track - 1);
        ask.mode = melodee::Context::PrintAsk::Unfreeze;
        return remote_print.push(ask) ? "OK melodee-unfreeze " + argument : "ERR busy";
    }
    if (action == "melodee-freeze" && (argument.empty() || argument == "status")) { // what is frozen, playing and asleep now
        auto bits = [](uint32_t b) {
            std::string out;
            for (unsigned t = 0; t < melodee::track_count; ++t) if ((b >> t) & 1) out += (out.empty() ? "" : ",") + std::to_string(t + 1);
            return out.empty() ? std::string("-") : out;
        };
        auto& ctx = shell.context(); // freeze plan F2d: its atomics only (this is not the MIDI thread)
        std::string s = "OK frozen=" + bits(host.frozen_tracks()) + " deep=" + bits(ctx.deep_now.load(std::memory_order_relaxed))
                      + " thawing=" + bits(ctx.thawing_now.load(std::memory_order_relaxed))
                      + " thaw_ms=" + std::to_string(ctx.thaw_took_now.load(std::memory_order_relaxed)) + " plugins=" + plugins.summary()
                      + " print_playing=" + bits(host.printing_tracks())
                      + " engine_asleep=" + bits(host.sleeping_tracks()) + " print_underruns=" + std::to_string(host.print_underruns())
                      + " print_waits=" + std::to_string(host.print_waits())
                      + " stream_underruns=" + std::to_string(const_cast<melodee::EngineHost&>(host).streams().underruns())
                      + " render_load=" + std::to_string(int(std::lround(host.render_load() * 1000))) + " track_load=";
        for (unsigned t = 0; t < melodee::track_count; ++t) s += (t ? "," : "") + std::to_string(int(std::lround(host.track_load(t) * 1000)));
        return s + " | " + printer.status();
    }
    if (action == "melodee-print" || action == "melodee-freeze") { // freeze plan F2a / F2b: prints of one track's patterns, or the track frozen on them
        const bool freezing = action == "melodee-freeze";
        if (!freezing && (argument.empty() || argument == "status")) return "OK " + printer.status();
        if (!freezing && argument == "cancel") { printer.cancel(); return "OK melodee-print cancel"; }
        melodee::Context::PrintAsk ask;
        if (freezing) ask.mode = melodee::Context::PrintAsk::Freeze;
        std::istringstream words(argument);
        std::string word;
        int track = 0;
        if (!(words >> track) || track < 1 || track > int(melodee::track_count))
            return "ERR " + action + " TRACK [A1,B2..|1..128|all] [pre|post] [loop|ring|cut]" + (freezing ? " [deep]" : "");
        ask.track = uint8_t(track - 1);
        while (words >> word) {
            if (word == "deep" && freezing) ask.deep = true; // freeze plan F2d: every pattern with notes, its plug-ins let go
            else if (word == "pre" || word == "post") ask.tap = word == "pre" ? melodee::PrintTap::PreFx : melodee::PrintTap::PostFx;
            else if (word == "loop") ask.tail = melodee::PrintTail::Loop;
            else if (word == "ring") ask.tail = melodee::PrintTail::Ring;
            else if (word == "cut") ask.tail = melodee::PrintTail::Cut;
            else if (word == "all") ask.all = true;
            else {
                std::istringstream list(word);
                std::string one;
                while (std::getline(list, one, ',')) {
                    int p = -1;
                    if (one.size() >= 2 && std::toupper(static_cast<unsigned char>(one[0])) >= 'A' &&
                        std::toupper(static_cast<unsigned char>(one[0])) < 'A' + int(melodee::pattern_banks)) {
                        int pad = std::atoi(one.c_str() + 1);
                        if (pad >= 1 && pad <= int(melodee::pattern_pads)) p = (std::toupper(static_cast<unsigned char>(one[0])) - 'A') * int(melodee::pattern_pads) + pad - 1;
                    } else if (!one.empty() && std::isdigit(static_cast<unsigned char>(one[0]))) {
                        int n = std::atoi(one.c_str());
                        if (n >= 1 && n <= int(melodee::pattern_count)) p = n - 1;
                    }
                    if (p < 0) return "ERR " + action + ": no pattern " + one;
                    ask.patterns[unsigned(p) / 64] |= uint64_t(1) << (unsigned(p) % 64);
                }
            }
        }
        return remote_print.push(ask) ? "OK " + action + " " + argument : "ERR busy";
    }
    if (action == "melodee-update") { // melodee-update [DIR|card]: check for an update now, in DIR instead of the card
        update_override = argument == "card" ? std::string() : argument;
        update_rescan = true;
        update_skip.clear();
        return "OK melodee-update " + (update_override.empty() ? std::string("card") : update_override);
    }
    if (action == "melodee-midi" && argument == "panic") { // MIDI Config's MIDI Panic (docs/MIDI_TRACKS.md)
        panic_asked.store(true);
        return "OK melodee-midi panic";
    }
    if (action == "melodee-midi") { // melodee-midi [usb] 90 3c 64
        RemoteMidi m;
        std::string rest = argument;
        if (rest.rfind("usb ", 0) == 0) { m.usb = true; rest = rest.substr(4); }
        size_t at = 0;
        while (at < rest.size() && m.packet.size < 32) {
            while (at < rest.size() && rest[at] == ' ') ++at;
            if (at >= rest.size()) break;
            char* end = nullptr;
            long byte = std::strtol(rest.c_str() + at, &end, 16);
            if (end == rest.c_str() + at || byte < 0 || byte > 255) return "ERR melodee-midi takes hex bytes: [usb] 90 3c 64";
            m.packet.bytes[m.packet.size++] = uint8_t(byte);
            at = size_t(end - rest.c_str());
        }
        if (!m.packet.size) return "ERR melodee-midi takes hex bytes: [usb] 90 3c 64";
        return remote_midi.push(m) ? "OK melodee-midi " + argument : "ERR busy";
    }
    if (action == "melodee-looper") { // docs/LOOPER.md: status straight from the engine; the rest on the MIDI thread
        if (argument.empty() || argument == "status") return "OK " + melodee::looper_status(host.looper());
        melodee::LooperAsk ask;
        std::string error;
        if (!melodee::parse_looper(argument, ask, error)) return "ERR " + error;
        return remote_looper.push(ask) ? "OK melodee-looper " + argument : "ERR busy";
    }
    if (action != "melodee-press") return {};
    if (argument.rfind("wheel", 0) == 0 && argument.size() > 6 && (argument[5] == '+' || argument[5] == '-') &&
        std::isdigit(static_cast<unsigned char>(argument[6]))) { // wheel+N, wheel-N (not wheel-push)
        int delta = std::atoi(argument.c_str() + 5);
        return presses.push({1, 0, int8_t(std::clamp(delta, -64, 63))}) ? "OK melodee-press " + argument : "ERR busy";
    }
    {   // k1=64, or shift+k3=64 (Shift held around the move: the card's second parameter)
        bool shift_pot = argument.rfind("shift+", 0) == 0;
        std::string pot_text = shift_pot ? argument.substr(6) : argument;
        if (pot_text.size() > 3 && pot_text[0] == 'k' && pot_text[1] >= '1' && pot_text[1] <= '3' && pot_text[2] == '=') {
            int value = std::clamp(std::atoi(pot_text.c_str() + 3), 0, 127);
            auto pot = unsigned(SurfaceControl::K1) + unsigned(pot_text[1] - '1');
            return presses.push({3, uint8_t(pot | (shift_pot ? 0x80 : 0)), int8_t(value)}) ? "OK melodee-press " + argument : "ERR busy";
        }
    }
    if (argument.rfind("fader=", 0) == 0 && argument.size() > 6) { // fader=V: the fader to V (0..127), as if moved there
        int value = std::clamp(std::atoi(argument.c_str() + 6), 0, 127);
        return presses.push({3, uint8_t(SurfaceControl::Fader), int8_t(value)}) ? "OK melodee-press " + argument : "ERR busy";
    }
    // power: a short press of Power (not on the surface port): the Exit sheet opens, nothing more
    if (argument == "power") { shell.power_pressed(); return "OK melodee-press power"; }
    auto button_named = [](const std::string& wanted) {
        for (unsigned n = 0; n < surface_buttons.size(); ++n) {
            std::string name = surface_buttons[n].name;
            for (auto& c : name) c = char(c == ' ' ? '-' : std::tolower(static_cast<unsigned char>(c)));
            if (name == wanted) return int(n);
        }
        return -1;
    };
    // pad5, or a button held around it: shift+pad5, erase+pad5, pad-bank+pad5 (not pad-fx)
    auto plus = argument.rfind("+pad");
    size_t at = plus == std::string::npos ? 3 : plus + 4;
    bool pad_press = (plus == std::string::npos ? argument.rfind("pad", 0) == 0 : true) &&
                     argument.size() > at && std::isdigit(static_cast<unsigned char>(argument[at]));
    if (pad_press) {
        int held = plus == std::string::npos ? -1 : button_named(argument.substr(0, plus));
        if (plus != std::string::npos && held < 0) return "ERR no button " + argument.substr(0, plus);
        int pad = std::atoi(argument.c_str() + at);
        if (pad < 1 || pad > 16) return "ERR pads are pad1 to pad16";
        return presses.push({2, uint8_t(pad - 1), int8_t(held + 1)}) ? "OK melodee-press " + argument : "ERR busy";
    }
    // hold-shift ... release-shift: a button (or hold-pad5: a pad) held down across other presses
    for (const char* verb : {"hold-", "release-"}) {
        std::string prefix = verb;
        if (argument.rfind(prefix, 0) != 0) continue;
        std::string what = argument.substr(prefix.size());
        if (what.rfind("pad", 0) == 0 && what.size() > 3 && std::isdigit(static_cast<unsigned char>(what[3]))) {
            int pad = std::atoi(what.c_str() + 3);
            if (pad < 1 || pad > 16) return "ERR pads are pad1 to pad16";
            return presses.push({5, uint8_t(pad - 1), int8_t(prefix == "hold-")}) ? "OK melodee-press " + argument : "ERR busy";
        }
        int n = button_named(what);
        if (n < 0) return "ERR no button " + argument.substr(prefix.size());
        return presses.push({4, uint8_t(n), int8_t(prefix == "hold-")}) ? "OK melodee-press " + argument : "ERR busy";
    }
    bool shifted = argument.rfind("shift+", 0) == 0;
    std::string wanted = shifted ? argument.substr(6) : argument;
    if (int n = button_named(wanted); n >= 0) return presses.push({0, uint8_t(n | (shifted ? 0x80 : 0)), 0}) ? "OK melodee-press " + argument : "ERR busy";
    return "ERR melodee-press takes a button, optionally shift+: sample, seq, play, stop, top-left, wheel-push, ...";
}

void MelodeeApp::draw(juce::Graphics& g) {
    using namespace melodee;
    uint32_t now = now_ms();
    if (boot_skip.exchange(false, std::memory_order_relaxed)) boot.skip(now);
    if (boot.covers(now)) { boot.draw(g, now); return; } // the app starts drawing as the animation fades out
    shell.snapshot(*frame, now);
    frame->audio_rate = session().audio.rate.load(std::memory_order_relaxed);
    frame->audio_periods = session().audio.periods.load(std::memory_order_relaxed);
    frame->audio_xruns = session().audio.xruns.load(std::memory_order_relaxed);
    { // About: how the machine is doing
        auto& m = frame->machine;
        m.cores = machine.cores.load(std::memory_order_relaxed);
        for (unsigned c = 0; c < m.core_load.size(); ++c) m.core_load[c] = machine.core_load[c].load(std::memory_order_relaxed);
        m.render_load = host.render_load();
        for (unsigned t = 0; t < melodee::track_count; ++t) m.track_load[t] = host.track_load(t);
        m.pdc_frames = host.compensation();
        for (unsigned n = 0; n < melodee::modulator_count; ++n) frame->mod_values[n] = host.modulator_value(n); // roadmap P4
        m.card = machine.card.load(std::memory_order_relaxed);
        m.card_free = machine.card_free.load(std::memory_order_relaxed);
        m.card_total = machine.card_total.load(std::memory_order_relaxed);
        m.stream_late = host.streams().underruns();
        m.plugins = machine.plugins.load(std::memory_order_relaxed);
        m.plugins_load = machine.plugins_load.load(std::memory_order_relaxed);
    }
    ui::Paint p(g);
    g.fillAll(ui::colour::bg);
    page_spec(frame->ui.page).draw(p, *frame);
    draw_overlays(p, *frame);
    boot.draw(g, now); // its fade out, over the app; nothing once it has ended
}

// The speaker plays while neither headphones nor line out is plugged in (and
// not while the mic is monitored). The runtime leaves it as the stock app
// did, which is often off.
void MelodeeApp::jack(const JackEvent&, Hardware& hardware) {
    bool plugged = session().plugged(Jack::Headphones) == 1 || session().plugged(Jack::LineOut) == 1;
    input_route.service(hardware, plugged);
}

// About (round 2): each core's load and the plug-ins' every half second, the card's space every five (statvfs()
// can wait on a slow card, so not more often, and only while the card is Melodee's).
void MelodeeApp::read_machine(Hardware& hardware) {
    uint32_t now = now_ms();
    if (!machine_at || now - machine_at >= 500) {
        machine_at = now ? now : 1;
        auto times = hardware.platform.cpu_times(); // the whole machine first, then each core
        unsigned cores = times.size() > 1 ? unsigned(std::min<size_t>(times.size() - 1, machine.core_load.size())) : 0;
        if (cpu_before.size() == times.size())
            for (unsigned c = 0; c < cores; ++c)
                machine.core_load[c].store(uint8_t(std::lround(cpu_load(cpu_before[c + 1], times[c + 1]) * 100)), std::memory_order_relaxed);
        machine.cores.store(uint8_t(cores), std::memory_order_relaxed);
        cpu_before = std::move(times);
        auto use = plugins.usage();
        machine.plugins.store(uint8_t(std::min(255u, use.loaded)), std::memory_order_relaxed);
        machine.plugins_load.store(use.load, std::memory_order_relaxed);
    }
    if (hardware.card.status().state != CardShareState::Home) { machine.card.store(false, std::memory_order_relaxed); card_at = 0; return; }
    if (card_at && now - card_at < 5000) return;
    card_at = now ? now : 1;
    const StorageSpace* card = nullptr; // the first card among the mounts, as ProjectStore picks it
    auto storage = hardware.platform.storage();
    for (const auto& space : storage) if (space.path.rfind("/media/", 0) == 0) { card = &space; break; }
    machine.card.store(card != nullptr, std::memory_order_relaxed);
    machine.card_free.store(card ? card->free_bytes : 0, std::memory_order_relaxed);
    machine.card_total.store(card ? card->total_bytes : 0, std::memory_order_relaxed);
}

void MelodeeApp::service(Hardware& hardware) {
    keep_rest_off();
    note_dropout();
    read_machine(hardware);
    input_route.service(hardware, session().plugged(Jack::Headphones) == 1 || session().plugged(Jack::LineOut) == 1);
    host.looper().service(); // the Looper's memory in and out, its fits (docs/LOOPER.md)
    card_access(hardware);
    card_update(hardware);
    web_service(hardware);
    // The card is Melodee's only while the base has it home: nothing reads or writes it meanwhile.
    if (hardware.card.status().state == CardShareState::Home) {
        library.service(hardware);
        store.service(hardware);
    }
    // POWER OFF on the Exit sheet, once the project is saved; if the MPC will not turn off, the stock app instead.
    if (shell.powering_off() && !power_off_asked) {
        power_off_asked = true;
        if (!request_power_off()) power_off_failed.store(true);
    }
    // A short press of Power opens the Exit sheet.
    uint64_t presses = hardware.power.presses();
    if (power_known && presses > power_presses && !hardware.power.pressed()) shell.power_pressed();
    if (!hardware.power.pressed()) { power_presses = presses; power_known = true; }
    // Settings' Screen row: the backlight, as a share of its range.
    unsigned screen = shell.screen();
    if (screen && screen != screen_applied) {
        auto backlight = hardware.platform.backlight();
        if (backlight.maximum > 0) hardware.platform.brightness(unsigned(std::max(1L, backlight.maximum * long(screen) / 100)));
        screen_applied = screen;
    }
}

// SD card access (shell/card_access.h): what the shell asked, done through the
// base's CardShare, and what came of it copied back for the shell.
void MelodeeApp::card_access(Hardware& hardware) {
    using melodee::CardNote;
    using melodee::CardRequest;
    auto& link = shell.card();
    auto& card = hardware.card;
    switch (CardRequest(link.request.exchange(0))) {
    case CardRequest::Lend:
        card_note = CardNote::None;
        if (library.busy()) card_note = CardNote::Writing; // a take or a render still writes to it
        else if (!library.release_instruments()) card_note = CardNote::Failed;
        else if (!card.lend()) card_note = card.status().message == "no card" ? CardNote::NoCard : CardNote::Failed;
        break;
    case CardRequest::Return: card.take_back(false); break;
    case CardRequest::Force: card.take_back(true); break;
    default: break;
    }
    const auto& now = card.status();
    if (now.state == CardShareState::Home && library.instruments_away()) library.restore_instruments(); // back, or never lent
    if (now.returns != card_returns) { // back: its projects and samples read again
        card_returns = now.returns;
        library.rescan();
        store.rescan();
    }
    CardNote note = card_note;
    if (note == CardNote::None && !now.message.empty()) {
        const auto& m = now.message;
        note = m == "no card" ? CardNote::NoCard : m == "the card is in use" || m == "the card is still mounted" ? CardNote::InUse
             : m == "the card came back with nothing to mount" ? CardNote::Empty : now.locked ? CardNote::Locked : CardNote::Failed;
    }
    if (now.state != CardShareState::Home) card_note = CardNote::None; // a note of Melodee's own lasts until the next try
    link.state.store(uint8_t(now.state));
    link.connected.store(now.connected);
    link.ejected.store(now.ejected);
    link.locked.store(now.locked);
    link.note.store(uint8_t(note));
    link.returns.store(now.returns);
}

std::string MelodeeApp::status() const {
    return "melodee_playing=" + std::to_string(host.playing()) + " melodee_dropped=" + std::to_string(host.dropped())
         + " melodee_stream_underruns=" + std::to_string(const_cast<melodee::EngineHost&>(host).streams().underruns())
         + " melodee_render_threads=" + std::to_string(host.render_threads())
         + " melodee_render_load=" + std::to_string(int(std::lround(host.render_load() * 1000))) // tenths of a percent, the recent peak
         + " melodee_render_parallel=" + std::to_string(host.render_parallel_blocks())
         + " melodee_dropout_track=" + std::to_string(dropout_track.load(std::memory_order_relaxed)) // the busiest at the last dropout
         + " melodee_dropout_load=" + std::to_string(dropout_load.load(std::memory_order_relaxed))   // its share then, tenths of a percent
         + " melodee_guard=" + std::to_string(host.guard_steals()) + "/" + std::to_string(host.guard_blocks()) // voices stolen / blocks eased
         + " melodee_update=" + std::to_string(int(const_cast<melodee::Shell&>(shell).updates().phase())) // UpdatePhase
         + " melodee_update_offer=" + std::to_string(int(update_offered.load()))
         + " " + shell.summary();
}

// Updates from the card (shell/update.h), using the base's drivers/sd_update.h and install/sd-update.sh.
// Checks the card's top folder for a newer release when the card is mounted or returned, handles LATER and
// INSTALL from the shell, and reads the install's status file until the install stops this app or fails. At
// start, reads the status of an earlier install and shows "Updated to <version>" if it installed this one.
void MelodeeApp::card_update(Hardware& hardware) {
    using melodee::UpdateError;
    using melodee::UpdatePhase;
    auto& link = shell.updates();
    const uint32_t now = now_ms();
    if (web.take_rescan()) { // a zip sent from the web page: check now, skipped ones too, and forget an old failure
        update_rescan = true;
        update_skip.clear();
        update_declined.clear();
        if (!update_installing) link.progress(UpdatePhase::None, UpdateError::None);
    }
    // Show the notice after the boot animation (ui/boot.h), with a second for the first frame to appear.
    constexpr uint32_t boot_ms = melodee::ui::BootAnimation::out_at + melodee::ui::BootAnimation::out_ms + 1000;
    if (!update_note.empty() && now >= boot_ms) {
        link.note(update_note);
        update_note.clear();
    }
    std::string zip;
    switch (link.take_request(zip)) {
    case 1: // LATER
        if (!zip.empty()) update_skip.push_back(zip);
        update_declined = zip;
        break;
    case 2: { // INSTALL, after the project was saved
        std::string error = "the offer went away";
        std::remove(sd_update::status_path); // remove an earlier install's status
        sd_update::Offer offer {zip, update_version, 0};
        if (!zip.empty() && zip == update_zip && update_ready() && sd_update::start(update_setup, offer, update_card, error)) {
            update_installing = true;
            update_started = now ? now : 1;
            update_polled = 0;
            link.progress(UpdatePhase::Checking, UpdateError::None);
        } else {
            std::fprintf(stderr, "melodee: update not started: %s\n", error.c_str());
            link.progress(UpdatePhase::Failed, UpdateError::Busy);
        }
        break;
    }
    default: break;
    }
    if ((update_installing || !update_settled) && now - update_polled >= 250) {
        update_polled = now;
        auto status = sd_update::read_status();
        if (update_installing) {
            if (status.state == "checking") link.progress(UpdatePhase::Checking, UpdateError::None);
            else if (status.state == "writing") link.progress(UpdatePhase::Writing, UpdateError::None);
            else if (status.state == "starting") link.progress(UpdatePhase::Starting, UpdateError::None);
            else if (status.state == "failed" || (status.state.empty() && now - update_started > 15000)) {
                link.progress(UpdatePhase::Failed, status.state.empty() ? UpdateError::Other : melodee::update_error(status.error));
                update_installing = false;
                update_skip.push_back(update_zip); // do not offer it again until the card is returned
                update_zip.clear();
                update_version.clear();
                update_offered.store(false);
                link.offer({}, {});
                std::remove(sd_update::status_path);
            }
        } else if (!status.running()) { // no status, done or failed; if still running, it is starting this app
            update_settled = true;
            if (status.state == "done" && sd_update::compare_versions(status.version, melodee::melodee_version()) == 0)
                update_note = "Updated to " + status.version;
            else if (status.state == "failed" && !status.version.empty())
                update_note = "Update to " + status.version + " failed";
            if (!status.state.empty()) std::remove(sd_update::status_path);
        }
    }
    // Every 2 seconds, while the card is mounted here: check again if the card changed or was returned.
    if (update_installing || !update_settled || hardware.card.status().state != CardShareState::Home) return;
    if (update_looked_at && now - update_looked_at < 2000 && !update_rescan) return;
    update_looked_at = now ? now : 1;
    std::string card = update_override;
    if (card.empty())
        for (const auto& space : hardware.platform.storage())
            if (space.path.rfind("/media/", 0) == 0) { card = space.path; break; }
    const uint64_t returns = hardware.card.status().returns;
    if (card == update_card && returns == update_returns && !update_rescan) return;
    if (returns != update_returns) update_skip.clear(); // card returned from a computer: offer skipped zips again
    update_card = card;
    update_returns = returns;
    update_rescan = false;
    sd_update::Offer offer;
    if (!card.empty() && update_ready()) offer = sd_update::find(update_setup, card, melodee::melodee_version(), update_skip);
    ++update_scans;
    update_zip = offer.zip;
    update_version = offer.version;
    update_offered.store(!offer.empty());
    link.offer(offer.zip, offer.version);
}

bool MelodeeApp::update_ready() {
    if (!update_setup_tried) {
        update_setup_tried = true;
        std::error_code error;
        auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error) update_setup.script = (exe.parent_path() / "sd-update.sh").string();
        update_setup.package = "melodee";
        update_setup.app = "melodee";
        update_setup.key = "/run/melodee-release-key.pem";
        std::ofstream out(update_setup.key, std::ios::trunc);
        out << melodee::release_key_pem;
        out.close();
        if (!out) update_setup.key.clear();
    }
    std::error_code error;
    return !update_setup.key.empty() && std::filesystem::is_regular_file(update_setup.script, error);
}

// The web page (web/web_app.h): started with the first pass (every ten seconds again while the port will not open),
// then told each pass what it shows. The card's folder is looked up once a second, and only while the card is home.
void MelodeeApp::web_service(Hardware& hardware) {
    const uint32_t now = now_ms();
    if (!web_running && (!web_tried_at || now - web_tried_at >= 10000)) {
        if (!web_tried_at) stop_stock_web_server();
        web_tried_at = now ? now : 1;
        std::string error;
        web_running = web.start(error);
        if (!web_running) std::fprintf(stderr, "melodee: web page not started on port %u or %u: %s\n", unsigned(melodee::web::port),
                                       unsigned(melodee::web::spare_port), error.c_str());
    }
    if (!web_running) return;
    if (!web_card_at || now - web_card_at >= 1000) {
        web_card_at = now ? now : 1;
        web_card.clear();
        if (hardware.card.status().state == CardShareState::Home)
            for (const auto& space : hardware.platform.storage())
                if (space.path.rfind("/media/", 0) == 0) { web_card = space.path; break; }
    }
    auto& link = shell.updates();
    melodee::web::Facts facts;
    facts.version = melodee::melodee_version();
    facts.card = web_card;
    if (!update_zip.empty() && update_zip != update_declined) {
        facts.offer_zip = update_zip;
        facts.offer_version = update_version;
    }
    facts.phase = uint8_t(link.phase());
    facts.error = uint8_t(link.error());
    facts.scans = update_scans;
    facts.card_free = machine.card_free.load(std::memory_order_relaxed);
    facts.card_total = machine.card_total.load(std::memory_order_relaxed);
    web.publish(facts);
    std::string note;
    if (web.take_note(note)) link.note(note);
}
