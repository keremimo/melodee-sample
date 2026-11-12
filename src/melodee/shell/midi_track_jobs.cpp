// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/midi_track_jobs.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/shell/input_route.h"
#include "melodee/shell/midi_ports.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace melodee {
namespace {
namespace fs = std::filesystem;
using Kind = Binding::Kind;
using Type = Command::Type;
using namespace midi_track;

std::string sysex_folder(const Context& c) { return c.library ? c.library->card() + "/SysEx" : std::string(); }
} // namespace

void set_midi_port(Context& c, unsigned t, int choice) {
    auto& track = c.project.tracks[t % track_count];
    int last = int(midi_port_count(c.ui, track)) - 1;
    int port = choice <= 0 ? External : choice == 1 ? UsbC : choice >= last ? AllUsb : UsbDevice;
    if (port == UsbDevice) {
        unsigned n = unsigned(choice - 2);
        if (n < c.ui.usb_device_count) std::snprintf(track.midi.device, sizeof(track.midi.device), "%s", c.ui.usb_devices[n].data());
    }
    c.set_param(t, Port, port);
    c.dirty = true;
}

void list_sysex(Context& c) {
    c.ui.sysex_count = 0;
    c.ui.sysex_listed = true;
    std::string folder = sysex_folder(c);
    std::error_code e;
    if (folder.empty() || !fs::is_directory(folder, e)) return;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder, e)) {
        auto name = entry.path().filename().string();
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (name.empty() || name[0] == '.' || ext != ".syx" || name.size() >= 32) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (c.ui.sysex_count == c.ui.sysex_names.size()) break;
        std::snprintf(c.ui.sysex_names[c.ui.sysex_count++].data(), 32, "%s", name.c_str());
    }
}

void set_sysex(Context& c, unsigned t, int choice) {
    auto& track = c.project.tracks[t % track_count];
    if (choice <= 0 || unsigned(choice) > c.ui.sysex_count) track.midi.sysex[0] = 0;
    else std::snprintf(track.midi.sysex, sizeof(track.midi.sysex), "%s", c.ui.sysex_names[unsigned(choice - 1)].data());
    c.dirty = true;
}

void send_sysex(Context& c, unsigned t) {
    const auto& track = c.project.tracks[t % track_count];
    if (track.engine != EngineKind::Midi || !track.midi.sysex[0]) { c.notice("No SysEx file"); return; }
    std::string path = sysex_folder(c) + "/" + track.midi.sysex;
    std::ifstream in(path, std::ios::binary);
    if (!in) { c.notice("SysEx: no file"); return; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > 65536) { c.notice("SysEx: over 64 KB"); return; }
    if (bytes.empty() || bytes.front() != 0xf0) { c.notice("SysEx: not a .syx"); return; }
    c.sysex_queue.push_back({uint8_t(t % track_count), std::move(bytes)});
    c.ui.sysex_percent[t % track_count] = 1;
}

void sysex_on_load(Context& c) {
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = c.project.tracks[t];
        if (track.engine == EngineKind::Midi && track.params[SysexOnLoad] && track.midi.sysex[0]) send_sysex(c, t);
    }
}

unsigned midi_input_needs(const Project& project) {
    unsigned needs = 0;
    for (const auto& track : project.tracks) {
        if (track.engine != EngineKind::Midi) continue;
        switch (track.params[AudioIn]) {
        case AudioRearL: case AudioRearR: case AudioRear: needs |= InputRoute::need_line; break;
        case AudioMic: needs |= InputRoute::need_mic; break;
        case AudioUsb: needs |= InputRoute::need_usb; break;
        default: break;
        }
    }
    return needs;
}

void midi_audio_tick(Context& c, unsigned needs, bool source_mic) {
    // The ADC as InputRoute routes it: the rear inputs when anything needs them, else the mic when anything does, else
    // as Input Config's source says.
    bool line = (needs & InputRoute::need_line) || (!(needs & InputRoute::need_mic) && !source_mic);
    bool again = c.now - c.midi_audio_at >= 1000; // a track reloaded forgets: told again a second on
    if (again) c.midi_audio_at = c.now;
    uint16_t in_use = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = c.project.tracks[t];
        int want = track.engine == EngineKind::Midi ? int(track.params[AudioIn]) : int(AudioNone), give = want;
        if ((want == AudioMic && line) || ((want == AudioRearL || want == AudioRearR || want == AudioRear) && !line)) {
            give = AudioNone; in_use = uint16_t(in_use | 1u << t);
        }
        if (give == c.midi_audio_sent[t] && !(again && give)) continue;
        c.send({Type::MidiAudio, uint8_t(t), 0, 0, int16_t(give)});
        c.midi_audio_sent[t] = int8_t(give);
    }
    c.ui.midi_in_use = in_use;
}

bool midi_binding(Context& c, const Binding& b, int value) {
    switch (b.kind) {
    case Kind::MidiPort: set_midi_port(c, c.ui.track, value); return true;
    case Kind::MidiSysexFile:
        if (!c.ui.sysex_listed) list_sysex(c);
        set_sysex(c, c.ui.track, value);
        return true;
    default: return false;
    }
}

} // namespace melodee
