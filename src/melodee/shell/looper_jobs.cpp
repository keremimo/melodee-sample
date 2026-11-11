// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/looper_jobs.h"
#include "melodee/engine/engine_host.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/sample_mode.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace melodee {
using looper::Do;
using Type = Command::Type;

namespace {
std::string loop_name(unsigned loop) { return "L" + std::to_string(loop % loop_tracks + 1); }

// What the audio thread refused, said once.
const char* refusal(looper::Error e) {
    switch (e) {
    case looper::Error::NoMemory: return "memory still coming";
    case looper::Error::Locked: return "not while it records";
    case looper::Error::TooLong: return "too long at this tempo";
    case looper::Error::Tempo: return "cannot fit this tempo";
    case looper::Error::Busy: return "busy, try again";
    default: return "";
    }
}
// A source in one word, for `status`.
std::string source_word(uint8_t source) {
    std::string name = loop_source_name(source);
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return name;
}
} // namespace

std::string loop_source_name(uint8_t source) {
    if (source == looper::source_input) return "Input";
    if (source == looper::source_mix) return "Mix";
    if (looper::source_is_track(source)) return "Track " + std::to_string(source);
    if (looper::source_is_loop(source)) return loop_name(source - looper::source_loop(0));
    if (looper::source_is_direct(source)) return input_source_name(looper::direct_input(source));
    return "Input";
}

namespace {
// The ADC route a loop's own input needs: 1 line, 2 mic, 0 none.
unsigned adc_of(uint8_t source) {
    if (!looper::source_is_direct(source)) return 0;
    auto in = looper::direct_input(source);
    return from_rear(in) ? InputRoute::need_line : in == InputSource::Mic ? InputRoute::need_mic : 0;
}
} // namespace

bool loop_source_clashes(const LooperSettings& lp, unsigned loop, uint8_t source) {
    unsigned adc = adc_of(source);
    if (!adc) return false;
    for (unsigned n = 0; n < loop_tracks; ++n)
        if (n != loop % loop_tracks && adc_of(lp.loops[n].source) && adc_of(lp.loops[n].source) != adc) return true;
    return false;
}

unsigned loop_input_needs(const LooperSettings& lp) {
    if (!lp.on) return 0;
    unsigned needs = 0;
    for (const auto& l : lp.loops) {
        needs |= adc_of(l.source);
        if (looper::source_is_direct(l.source) && from_usb(looper::direct_input(l.source))) needs |= InputRoute::need_usb;
    }
    if ((needs & InputRoute::need_line) && (needs & InputRoute::need_mic)) needs &= ~InputRoute::need_mic; // never both (a file from elsewhere)
    return needs;
}

std::string loop_length_name(unsigned bars) {
    if (!bars) return "Free";
    return std::to_string(bars) + (bars == 1 ? " bar" : " bars");
}

uint8_t next_loop_source(const Project& project, unsigned loop, uint8_t source) {
    for (unsigned step = 0; step < looper::source_count; ++step) {
        source = uint8_t((source + 1) % looper::source_count);
        if (looper::source_is_track(source) && !project.tracks[source - 1].used()) continue;
        if (source == looper::source_loop(loop)) continue;
        if (loop_source_clashes(project.looper, loop, source)) continue;
        return source;
    }
    return looper::source_input;
}

void looper_do(Context& c, Do what, unsigned loop, int value, uint32_t position) {
    loop %= loop_tracks;
    auto& lp = c.project.looper;
    auto view = c.host.looper().view(loop);
    bool taking = view.state == Looper::State::Recording || view.state == Looper::State::Overdub;
    switch (what) {
    case Do::Enable: lp.on = value != 0; break;
    case Do::Length:
        if (view.state != Looper::State::Empty && view.state != Looper::State::Armed) { c.notice(loop_name(loop) + ": length before a take"); return; }
        lp.loops[loop].bars = uint8_t(std::clamp(value, 0, 16));
        break;
    case Do::Feedback: lp.loops[loop].feedback = uint8_t(std::clamp(value, 0, 100)); break;
    case Do::Source:
        if (taking) { c.notice(loop_name(loop) + ": source between takes"); return; }
        if (value < 0 || value >= int(looper::source_count)) return;
        if (loop_source_clashes(lp, loop, uint8_t(value))) { c.notice("Mic and Rear share one input: one at a time"); return; }
        lp.loops[loop].source = uint8_t(value);
        break;
    case Do::Quantize: lp.quantize = uint8_t(std::clamp(value, 0, int(looper::Quantize::Count) - 1)); break;
    case Do::Then: lp.then = uint8_t(std::clamp(value, 0, int(looper::Then::Count) - 1)); break;
    case Do::Transport: lp.transport = uint8_t(std::clamp(value, 0, int(looper::Transport::Count) - 1)); break;
    case Do::Clear: lp.loops[loop].frames = 0; break; // nothing kept for the file any more
    default:
        if (!lp.on) { c.notice("Looper off: B3 turns it on"); return; }
        break;
    }
    Command cmd {Type::Looper, uint8_t(loop), 0, uint16_t(what), int16_t(value)};
    cmd.position = position;
    c.send(cmd);
}

void set_looper(Context& c, bool on) {
    looper_do(c, Do::Enable, 0, on);
    if (!on) for (auto& l : c.project.looper.loops) l.frames = 0;
    c.notice(on ? "Looper on" : "Looper off");
}

void set_loop_mix(Context& c, unsigned loop, Binding::Kind kind, int value) {
    loop %= loop_tracks;
    auto& m = c.project.loop_mix[loop];
    uint8_t track = uint8_t(track_count + loop);
    switch (kind) {
    case Binding::Kind::LoopLevel: m.level = uint8_t(std::clamp(value, 0, 127)); c.send({Type::Level, track, 0, 0, m.level}); break;
    case Binding::Kind::LoopPan: m.pan = int8_t(std::clamp(value, -50, 50)); c.send({Type::Pan, track, 0, 0, m.pan}); break;
    case Binding::Kind::LoopSend: m.send = uint8_t(std::clamp(value, 0, 100)); c.send({Type::Send, track, 0, 0, m.send}); break;
    case Binding::Kind::LoopBus: m.bus = uint8_t(std::clamp(value, 0, int(bus_count) - 1)); c.send({Type::SendBus, track, 0, 0, m.bus}); break;
    default: break;
    }
}

void set_loop_mute(Context& c, unsigned loop, bool on) {
    loop %= loop_tracks;
    c.project.loop_mix[loop].mute = on;
    c.send({Type::Mute, uint8_t(track_count + loop), 0, 0, on});
}

void set_loop_solo(Context& c, unsigned loop, bool on) {
    loop %= loop_tracks;
    c.project.loop_mix[loop].solo = on;
    c.send({Type::Solo, uint8_t(track_count + loop), 0, 0, on});
}

// The Looper was meant, not the tap's loop: undo that tap, silently.
static void take_back_loop(Context& c) {
    if (c.ui.loop_on_song) song_loop_back(c); else c.toggle_loop();
    c.ui.notice[0] = 0;
}

// Loop is a hold, not a tap: the tap's loop taken back, and no double tap after it.
static void grip_loop(Context& c) {
    auto& ui = c.ui;
    if (ui.loop_grip) return;
    ui.loop_grip = true; ui.loop_tap_at = 0;
    if (ui.loop_toggled) take_back_loop(c);
    ui.loop_toggled = false;
}

// Shift + Loop's printed job, whether Shift is still down or not: the last pad reversed.
static void reverse_last_pad(Context& c) {
    if (c.track().engine != EngineKind::Sampler) c.notice("Loop: a kit pad's");
    else c.toggle_reverse();
}

// Loop pressed a moment before Shift: Shift + Loop after all, the tap's loop taken back.
static void loop_to_shift(Context& c) {
    auto& ui = c.ui;
    if (ui.loop_toggled) take_back_loop(c);
    ui.loop_toggled = false; ui.loop_held = false; ui.loop_tap_at = 0;
    ui.loop_shift_held = true; ui.shift_used = true;
}

void loop_press(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (!pressed) {
        if (ui.loop_held && !ui.loop_grip && ui.shift) loop_to_shift(c); // Shift came down during the tap
        if (ui.loop_shift_held) reverse_last_pad(c); // let go before the hold: Reverse, as printed
        if (ui.loop_held && ui.page != PageId::Looper) ui.loop_clear = -1; // a clear pad still down: no clear
        ui.loop_held = false; ui.loop_grip = false; ui.loop_shift_held = false;
        return;
    }
    if (ui.shift) { // Shift + Loop: Reverse on release; held half a second, the Looper on or off (looper_tick)
        ui.shift_used = true;
        ui.loop_down = c.now; ui.loop_shift_held = true;
        return;
    }
    if (ui.page == PageId::Looper) { ui.loop_tap_at = 0; c.back(); return; }
    if (ui.loop_tap_at && c.now - ui.loop_tap_at <= loop_double_ms) { // tapped twice: the Looper
        if (ui.loop_toggled) take_back_loop(c); // the first tap's loop
        ui.loop_tap_at = 0; ui.loop_toggled = false; ui.loop_held = false;
        c.open(PageId::Looper);
        return;
    }
    ui.loop_tap_at = std::max<uint32_t>(c.now, 1);
    ui.loop_down = c.now; ui.loop_held = true; ui.loop_grip = false;
    ui.loop_on_song = ui.page == PageId::Song && !ui.song_export;
    if (ui.loop_on_song) ui.loop_toggled = song_loop(c); // on Song: the brace on the clip's bars
    else {
        ui.loop_toggled = c.track().engine == EngineKind::Sampler && c.pad().used();
        if (ui.loop_toggled) loop_button(c);             // the MPC's job: the last pad's loop
    }
}

bool loop_held_pad(Context& c, unsigned pad, bool down) {
    if (!c.ui.loop_held || c.ui.page == PageId::Looper) return false;
    if (down) grip_loop(c);
    if (down && c.ui.shift) c.ui.shift_used = true; // Shift + a play / stop pad: at once
    loop_grid_pad(c, pad, down);
    return true;
}

void loop_grid_pad(Context& c, unsigned pad, bool down) {
    auto& ui = c.ui;
    unsigned column = pad % 4, row = pad / 4 % 4; // row 0 is the bottom one (pads 1-4)
    if (!down) { if (row == 0 && ui.loop_clear == int(column)) ui.loop_clear = -1; return; }
    if (!c.project.looper.on) { c.notice(ui.page == PageId::Looper ? "Looper off: B3 turns it on" : "Looper off: Shift + hold Loop"); return; }
    ui.loop = uint8_t(column);
    switch (row) {
    case 3: looper_do(c, Do::Primary, column); break;
    case 2: looper_do(c, ui.shift ? Do::Now : Do::Toggle, column); break;
    case 1: looper_do(c, Do::Undo, column); break;
    default:
        if (c.host.looper().view(column).state != Looper::State::Empty) { ui.loop_clear = int8_t(column); ui.loop_clear_at = std::max<uint32_t>(c.now, 1); }
        break;
    }
}

void loop_mute_pad(Context& c, unsigned pad) {
    unsigned loop = pad % 4;
    auto& m = c.project.loop_mix[loop];
    c.ui.loop = uint8_t(loop);
    if (c.ui.shift) { c.ui.shift_used = true; set_loop_solo(c, loop, !m.solo); }
    else set_loop_mute(c, loop, !m.mute);
}

void loop_sheet_push(Context& c) {
    auto& ui = c.ui;
    unsigned loop = ui.loop % loop_tracks;
    auto v = c.host.looper().view(loop);
    bool audio = v.length > 0;
    switch (LoopRow(ui.loop_row % unsigned(LoopRow::Count))) {
    case LoopRow::Source: // steps; the sheet stays
        looper_do(c, Do::Source, loop, next_loop_source(c.project, loop, c.project.looper.loops[loop].source));
        return;
    case LoopRow::Toggle: if (!audio) return; looper_do(c, Do::Toggle, loop); break;
    case LoopRow::Stop: looper_do(c, Do::Halt, loop); break;
    case LoopRow::Undo: if (v.undo == looper::Undo::None) return; looper_do(c, Do::Undo, loop); break;
    case LoopRow::Commit: case LoopRow::CommitNote:
        if (!audio) return;
        commit_loop(c, loop, LoopRow(ui.loop_row) == LoopRow::CommitNote);
        break;
    case LoopRow::Clear: if (v.state == Looper::State::Empty) return; ask(c, UiState::Ask::ClearLoop, int(loop)); break;
    case LoopRow::Off: ask(c, UiState::Ask::LooperOff, 0); break;
    case LoopRow::Count: break;
    }
    ui.loop_sheet = false;
}

void looper_tick(Context& c, uint32_t now) {
    auto& ui = c.ui;
    const auto& looper = c.host.looper();
    // Shift + Loop held: the Looper on, or off (asked first while a loop holds audio); no Reverse.
    if (ui.loop_held && !ui.loop_grip && ui.shift && now - ui.loop_down <= loop_chord_ms) loop_to_shift(c); // Shift a moment after Loop
    if (ui.loop_shift_held && now - ui.loop_down >= loop_hold_ms) {
        ui.loop_shift_held = false;
        bool audio = false;
        for (unsigned n = 0; n < loop_tracks; ++n) audio |= looper.view(n).state != Looper::State::Empty;
        if (!c.project.looper.on) set_looper(c, true);
        else if (audio) ask(c, UiState::Ask::LooperOff, 0);
        else set_looper(c, false);
    }
    // Loop held: the pads are the loop grid (they were from the press), the press's loop taken back.
    if (ui.loop_held && !ui.loop_grip && now - ui.loop_down >= loop_hold_ms && ui.page != PageId::Looper) {
        grip_loop(c);
        if (!c.project.looper.on) c.notice("Looper off: Shift + hold Loop"); // on, the lit grid says it
    }
    if (ui.loop_tap_at && now - ui.loop_tap_at > loop_double_ms && !ui.loop_held) { ui.loop_tap_at = 0; ui.loop_toggled = false; }
    // A clear pad held long enough.
    if (ui.loop_clear >= 0 && now - ui.loop_clear_at >= loop_clear_ms) {
        unsigned loop = unsigned(ui.loop_clear);
        ui.loop_clear = -1;
        looper_do(c, Do::Clear, loop);
        c.notice("L" + std::to_string(loop + 1) + " cleared");
    }
    // What the audio thread refused, said once.
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto v = looper.view(n);
        if (v.errors != c.loop_errors[n]) {
            c.loop_errors[n] = v.errors;
            if (*refusal(v.error)) c.notice("L" + std::to_string(n + 1) + ": " + refusal(v.error));
        }
    }
    // The metronome's Rec setting clicks while a loop records or overdubs, and for one armed while stopped.
    bool click = looper.recording() || (looper.armed() && (!c.host.playing() || c.host.count_in()));
    if (click != c.loop_click) { c.loop_click = click; c.send_click(); }
}

bool parse_looper(const std::string& argument, LooperAsk& out, std::string& error) {
    std::istringstream in(argument);
    std::string verb, a, b, d;
    in >> verb >> a >> b >> d;
    auto loop_of = [&](const std::string& text) {
        int n = std::atoi(text.c_str());
        if (n < 1 || n > int(loop_tracks)) { error = "loops are 1 to 4"; return false; }
        out.loop = uint8_t(n - 1);
        return true;
    };
    out = LooperAsk {};
    using Op = LooperAsk::Op;
    if (verb == "on" || verb == "off") { out.what = Do::Enable; out.value = verb == "on"; return true; }
    struct Simple { const char* name; Do what; };
    static const Simple simple[] {{"rec", Do::Primary}, {"toggle", Do::Toggle}, {"halt", Do::Halt}, {"now", Do::Now}, {"undo", Do::Undo}, {"clear", Do::Clear}};
    for (const auto& s : simple)
        if (verb == s.name) { out.what = s.what; return loop_of(a); }
    if (verb == "length") {
        if (!loop_of(a)) return false;
        out.what = Do::Length;
        out.value = b == "free" ? 0 : std::atoi(b.c_str());
        if (b != "free" && std::find(looper::lengths.begin(), looper::lengths.end(), out.value) == looper::lengths.end()) { error = "length 1, 2, 4, 8, 16 or free"; return false; }
        return true;
    }
    if (verb == "feedback") { if (!loop_of(a)) return false; out.what = Do::Feedback; out.value = std::clamp(int(std::atof(b.c_str()) * 100 + 0.5), 0, 100); return true; }
    if (verb == "level" || verb == "pan" || verb == "send" || verb == "bus") {
        if (!loop_of(a)) return false;
        out.op = Op::Mix;
        out.mix = verb == "level" ? Binding::Kind::LoopLevel : verb == "pan" ? Binding::Kind::LoopPan : verb == "send" ? Binding::Kind::LoopSend : Binding::Kind::LoopBus;
        out.value = verb == "bus" && !b.empty() && std::isalpha(static_cast<unsigned char>(b[0])) ? std::toupper(static_cast<unsigned char>(b[0])) - 'A' : std::atoi(b.c_str());
        return true;
    }
    if (verb == "mute" || verb == "solo") { if (!loop_of(a)) return false; out.op = verb == "mute" ? Op::Mute : Op::Solo; out.value = b != "0" && b != "off"; return true; }
    if (verb == "quantize") {
        out.what = Do::Quantize;
        out.value = a == "bar" ? 0 : a == "beat" ? 1 : a == "off" ? 2 : -1;
        if (out.value < 0) { error = "quantize bar, beat or off"; return false; }
        return true;
    }
    if (verb == "then") {
        out.what = Do::Then;
        out.value = a == "play" ? 0 : a == "overdub" ? 1 : a == "stop" ? 2 : -1;
        if (out.value < 0) { error = "then play, overdub or stop"; return false; }
        return true;
    }
    if (verb == "transport") {
        out.what = Do::Transport;
        out.value = a == "follow" ? 0 : a == "free" ? 1 : -1;
        if (out.value < 0) { error = "transport follow or free"; return false; }
        return true;
    }
    if (verb == "latency") {
        out.what = Do::Latency;
        if (a == "auto") { out.value = -1; return true; }
        out.value = 0; out.position = uint32_t(std::max(0, std::atoi(a.c_str())));
        return true;
    }
    if (verb == "source") {
        if (!loop_of(a)) return false;
        out.what = Do::Source;
        int m = std::atoi(d.c_str());
        if (b == "input") out.value = looper::source_input;
        else if (b == "mix") out.value = looper::source_mix;
        else if (b == "track" && m >= 1 && m <= int(track_count)) out.value = looper::source_track(unsigned(m - 1));
        else if (b == "loop" && m >= 1 && m <= int(loop_tracks) && m - 1 != out.loop) out.value = looper::source_loop(unsigned(m - 1));
        else {
            // A hardware input of its own: rear, rear l, rear r, usb, usb l, usb r, mic (or run together: rearl).
            std::string asked = b + d;
            out.value = -1;
            for (unsigned i = 0; i < looper::direct_inputs.size(); ++i)
                if (asked == source_word(looper::source_direct(i))) out.value = looper::source_direct(i);
            if (out.value < 0) { error = "source input, track 1-16, loop 1-4 (another), mix, rear [l|r], usb [l|r] or mic"; return false; }
        }
        return true;
    }
    if (verb == "commit") { if (!loop_of(a)) return false; out.op = Op::Commit; out.value = b == "note"; return true; }
    error = "melodee-looper status | on | off | rec|toggle|halt|now|undo|clear N | length N 1-16|free | feedback N 0-1 | "
            "level|pan|send|bus N V | mute|solo N 0|1 | quantize bar|beat|off | then play|overdub|stop | source N input|track M|loop M|mix|rear [l|r]|usb [l|r]|mic | "
            "latency FRAMES|auto | transport follow|free | commit N [note]";
    return false;
}

void looper_remote(Context& c, const LooperAsk& a) {
    using Op = LooperAsk::Op;
    switch (a.op) {
    case Op::Do:
        if (a.what == Do::Enable) set_looper(c, a.value != 0);
        else looper_do(c, a.what, a.loop, a.value, a.position);
        break;
    case Op::Mix: set_loop_mix(c, a.loop, a.mix, a.value); break;
    case Op::Mute: set_loop_mute(c, a.loop, a.value != 0); break;
    case Op::Solo: set_loop_solo(c, a.loop, a.value != 0); break;
    case Op::Commit: commit_loop(c, a.loop, a.value != 0); break;
    }
}

std::string looper_status(const Looper& looper) {
    static const char* const pend[] {"", "start", "close", "play", "stop"};
    std::string out = "melodee_looper=" + std::string(looper.enabled_now() ? "on" : "off") + " melodee_loop_quantize=" + looper::quantize_name(looper.quantize())
                    + " melodee_loop_then=" + looper::then_name(looper.then()) + " melodee_loop_transport=" + (looper.transport() ? "free" : "follow")
                    + " melodee_loop_latency=" + (looper.latency() < 0 ? std::string("auto") : std::to_string(looper.latency()))
                    + " melodee_loop_memory=" + std::to_string(looper.buffers_out());
    for (unsigned n = 0; n < loop_tracks; ++n) {
        auto v = looper.view(n);
        const char* word = looper::state_word(v.state);
        out += " melodee_loop" + std::to_string(n + 1) + "=" + (*word ? word : "EMPTY") + ",len=" + std::to_string(v.length)
             + ",pos=" + std::to_string(v.position) + ",bars=" + std::to_string(v.bars) + ",layers=" + std::to_string(v.layers)
             + ",undo=" + std::to_string(int(v.undo)) + ",src=" + source_word(v.source) + ",fb=" + std::to_string(v.feedback)
             + ",tempo=" + std::to_string(v.tempo) + ",stretched=" + std::to_string(v.stretched) + ",fitting=" + std::to_string(v.fitting);
        if (v.pending != Looper::Pending::None) out += std::string(",pending=") + pend[unsigned(v.pending) % 5];
    }
    return out;
}

} // namespace melodee
