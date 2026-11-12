// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/smf.h"
#include "melodee/model/song_flatten.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace melodee {
namespace {
void be32(std::vector<uint8_t>& out, uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); }
void be16(std::vector<uint8_t>& out, uint32_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void vlq(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t bytes[5];
    unsigned n = 0;
    do { bytes[n++] = uint8_t(v & 0x7f); v >>= 7; } while (v && n < 5);
    while (n--) out.push_back(uint8_t(bytes[n] | (n ? 0x80 : 0)));
}
void chunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& body) {
    out.insert(out.end(), tag, tag + 4);
    be32(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}
void meta(std::vector<uint8_t>& body, uint8_t type, const std::vector<uint8_t>& data) {
    vlq(body, 0); body.push_back(0xff); body.push_back(type); vlq(body, uint32_t(data.size()));
    body.insert(body.end(), data.begin(), data.end());
}
} // namespace

std::vector<uint8_t> write_smf(const std::vector<SmfTrack>& tracks, unsigned tempo_tenths, unsigned beats, unsigned unit) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> header;
    be16(header, 1); be16(header, uint32_t(tracks.size() + 1)); be16(header, smf_ppq);
    chunk(out, "MThd", header);
    { // the conductor: tempo and time signature
        std::vector<uint8_t> body;
        uint32_t micros = uint32_t(std::lround(60000000.0 * 10.0 / std::max(tempo_tenths, 10u)));
        meta(body, 0x51, {uint8_t(micros >> 16), uint8_t(micros >> 8), uint8_t(micros)});
        unsigned power = 0;
        while ((1u << power) < std::max(unit, 1u) && power < 6) ++power;
        meta(body, 0x58, {uint8_t(std::clamp(beats, 1u, 64u)), uint8_t(power), 24, 8});
        meta(body, 0x2f, {});
        chunk(out, "MTrk", body);
    }
    for (const auto& track : tracks) {
        struct Message { uint32_t tick; bool on; uint8_t key, velocity; };
        std::vector<Message> messages;
        for (const auto& n : track.notes) {
            messages.push_back({n.tick, true, n.key, std::max<uint8_t>(n.velocity, 1)});
            messages.push_back({n.tick + std::max<uint32_t>(n.length, 1), false, n.key, 0});
        }
        // In time; at the same tick an off before an on, so a note ended and played again on one tick both sound.
        std::stable_sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) { return a.tick != b.tick ? a.tick < b.tick : !a.on && b.on; });
        std::vector<uint8_t> body;
        if (!track.name.empty()) meta(body, 0x03, std::vector<uint8_t>(track.name.begin(), track.name.end()));
        uint8_t channel = uint8_t((std::clamp<unsigned>(track.channel, 1, 16) - 1) & 15);
        uint32_t last = 0;
        for (const auto& m : messages) {
            vlq(body, m.tick - last);
            last = m.tick;
            body.push_back(uint8_t((m.on ? 0x90 : 0x80) | channel));
            body.push_back(uint8_t(m.key & 0x7f));
            body.push_back(uint8_t(m.on ? (m.velocity & 0x7f) : 64));
        }
        vlq(body, 0); body.push_back(0xff); body.push_back(0x2f); body.push_back(0);
        chunk(out, "MTrk", body);
    }
    return out;
}

bool read_smf(const std::vector<uint8_t>& data, std::vector<SmfTrack>& out, std::string& why, unsigned* tempo_tenths) {
    out.clear();
    if (tempo_tenths) *tempo_tenths = 0;
    size_t at = 0;
    auto u32 = [&](size_t p) { return uint32_t(data[p]) << 24 | uint32_t(data[p + 1]) << 16 | uint32_t(data[p + 2]) << 8 | data[p + 3]; };
    auto u16 = [&](size_t p) { return unsigned(data[p]) << 8 | data[p + 1]; };
    if (data.size() < 14 || std::string(data.begin(), data.begin() + 4) != "MThd") { why = "not a MIDI file"; return false; }
    uint32_t header = u32(4);
    unsigned format = u16(8), count = u16(10), division = u16(12);
    if (division & 0x8000) { why = "SMPTE time"; return false; }
    if (format > 1) { why = "format 2"; return false; }
    if (!division) { why = "no division"; return false; }
    at = 8 + size_t(header);
    for (unsigned n = 0; n < count && at + 8 <= data.size(); ++n) {
        uint32_t size = u32(at + 4);
        bool track = std::string(data.begin() + long(at), data.begin() + long(at + 4)) == "MTrk";
        size_t p = at + 8, end = std::min(data.size(), size_t(at + 8 + size));
        at = end;
        if (!track) continue;
        std::string name;
        // Notes held: by channel and key, where they began and how hard.
        std::map<unsigned, std::pair<uint64_t, uint8_t>> held;
        std::array<std::vector<SmfNote>, 16> by_channel;
        uint64_t tick = 0;
        uint8_t status = 0;
        auto read_vlq = [&](uint32_t& v) {
            v = 0;
            for (unsigned k = 0; k < 4 && p < end; ++k) { uint8_t b = data[p++]; v = v << 7 | (b & 0x7f); if (!(b & 0x80)) return true; }
            return false;
        };
        auto scaled = [&](uint64_t t) { return uint32_t(std::min<uint64_t>((t * smf_ppq + division / 2) / division, 0xffffffffu)); };
        while (p < end) {
            uint32_t delta = 0;
            if (!read_vlq(delta)) break;
            tick += delta;
            if (p >= end) break;
            uint8_t b = data[p];
            if (b == 0xff) { // meta
                if (p + 2 > end) break;
                uint8_t type = data[p + 1];
                p += 2;
                uint32_t length = 0;
                if (!read_vlq(length) || p + length > end) break;
                if (type == 0x03 && name.empty()) name.assign(data.begin() + long(p), data.begin() + long(p + length));
                if (type == 0x51 && length == 3 && tempo_tenths && !*tempo_tenths) {
                    uint32_t micros = uint32_t(data[p]) << 16 | uint32_t(data[p + 1]) << 8 | data[p + 2];
                    if (micros) *tempo_tenths = unsigned(std::lround(600000000.0 / micros));
                }
                p += length;
                if (type == 0x2f) break;
                continue;
            }
            if (b == 0xf0 || b == 0xf7) { // SysEx: skipped
                ++p;
                uint32_t length = 0;
                if (!read_vlq(length)) break;
                p += length;
                continue;
            }
            if (b & 0x80) { status = b; ++p; }
            if (!status) break; // running status with none before
            unsigned type = status & 0xf0, ch = status & 15, bytes = type == 0xc0 || type == 0xd0 ? 1 : 2;
            if (p + bytes > end) break;
            uint8_t d1 = data[p], d2 = bytes > 1 ? data[p + 1] : 0;
            p += bytes;
            unsigned which = ch << 7 | (d1 & 0x7f);
            if (type == 0x90 && d2) {
                if (auto h = held.find(which); h != held.end()) // played again while held: the first ends here
                    by_channel[ch].push_back({scaled(h->second.first), std::max(1u, scaled(tick) - scaled(h->second.first)), uint8_t(d1 & 0x7f), h->second.second});
                held[which] = {tick, d2};
            } else if (type == 0x80 || (type == 0x90 && !d2)) {
                auto h = held.find(which);
                if (h == held.end()) continue;
                by_channel[ch].push_back({scaled(h->second.first), std::max(1u, scaled(tick) - scaled(h->second.first)), uint8_t(d1 & 0x7f), h->second.second});
                held.erase(h);
            }
        }
        for (const auto& [which, h] : held) // never let go: to the track's end
            by_channel[which >> 7].push_back({scaled(h.first), std::max(1u, scaled(tick) - scaled(h.first)), uint8_t(which & 0x7f), h.second});
        for (unsigned ch = 0; ch < 16; ++ch) {
            if (by_channel[ch].empty()) continue;
            auto& notes = by_channel[ch];
            std::stable_sort(notes.begin(), notes.end(), [](const SmfNote& a, const SmfNote& b) { return a.tick != b.tick ? a.tick < b.tick : a.key < b.key; });
            out.push_back({name, uint8_t(ch + 1), std::move(notes)});
        }
    }
    if (out.empty()) { why = "no notes"; return false; }
    return true;
}

std::vector<SmfTrack> smf_tracks(const Project& project, const PatternBank& patterns) {
    std::vector<SmfTrack> out;
    const auto& song = project.song;
    unsigned bars = song.length();
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = project.tracks[t];
        if (!tr.used()) continue;
        Pattern lane;
        bool full = false;
        if (bars) { if (!flatten_lane(song, t, patterns.at[t], project.bar_ticks(), bars, lane, full)) continue; }
        else lane = patterns.at[t][tr.pattern % pattern_count];
        SmfTrack track;
        track.name = tr.name[0] ? tr.name : "Track " + std::to_string(t + 1);
        bool kit = tr.engine == EngineKind::Sampler;
        track.channel = uint8_t(tr.engine == EngineKind::Midi ? std::clamp<int>(tr.params[1], 1, 16) : kit ? 10 : tr.midi_channel ? tr.midi_channel : t % 16 + 1);
        for (const auto& e : lane.events) {
            if (e.tick >= lane.length) break;
            int key = e.key;
            if (kit) key = 36 + (int(e.key) - int(sampler_first_note)); // a kit's pad, as MIDI in hears it
            if (key < 0 || key > 127) continue;
            track.notes.push_back({e.tick, std::max<uint32_t>(e.length, 1), uint8_t(key), e.velocity});
        }
        if (!track.notes.empty()) out.push_back(std::move(track));
    }
    return out;
}

bool smf_pattern(const std::vector<SmfTrack>& tracks, const Track& track, uint32_t bar_ticks, Pattern& out) {
    out = Pattern {};
    bool kit = track.engine == EngineKind::Sampler;
    uint32_t end = 0;
    std::vector<Event> events;
    for (const auto& t : tracks)
        for (const auto& n : t.notes) {
            int key = n.key;
            if (kit) { if (key < 36 || key >= 36 + int(sampler_pads)) continue; key = int(sampler_first_note) + key - 36; }
            Event e;
            e.tick = n.tick; e.length = uint16_t(std::clamp<uint32_t>(n.length, 1, 0xffff)); e.key = uint8_t(key & 0xff);
            e.velocity = std::clamp<uint8_t>(n.velocity, 1, 127);
            events.push_back(e);
            end = std::max(end, n.tick + n.length);
        }
    if (events.empty()) return false;
    uint32_t bars = std::clamp<uint32_t>((end + bar_ticks - 1) / std::max(bar_ticks, 1u), 1, max_bars);
    out.length = bars * bar_ticks;
    for (const auto& e : events) if (e.tick < out.length && !out.add(e)) break; // full: the rest left out
    return !out.events.empty();
}

} // namespace melodee
