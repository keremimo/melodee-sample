// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/melodee_app.h"
#include "drivers/card_share.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "surface/pads.h"
#include "drivers/power_button.h"
#include "melodee/pages/page.h"
#include "melodee/ui/paint.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sched.h>
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
} // namespace


MelodeeApp::MelodeeApp()
    : shell(host), library(host), plugins(host), frame(std::make_unique<melodee::Snapshot>()) {
    shell.set_library(&library);
    shell.set_plugins(&plugins);
    shell.set_store(&store);
    shell.set_input(&input_route);
    // The card's settings now, not first on the service thread: the runtime opens the audio device with
    // audio_format() as it starts, and Settings' rate from that first open spares a reopen moments later
    // (the codec powered down and up again, its mute shut a second, and the USB-C gadget rebound twice).
    shell.start_settings(melodee::ProjectStore::read_settings_file(melodee::ProjectStore::projects_root(Platform().storage())).settings);
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
         + " " + shell.summary();
}
