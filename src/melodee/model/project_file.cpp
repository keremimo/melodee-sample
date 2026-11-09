// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/project_file.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace melodee {
namespace {
constexpr char magic[8] {'M', 'E', 'L', 'O', 'D', 'E', 'E', '1'};

class Writer {
public:
    std::vector<uint8_t> out;
    void u8(unsigned v) { out.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void i16(int v) { u16(uint16_t(int16_t(v))); }
    void text(const char* s) { size_t n = std::strlen(s); u8(unsigned(n)); out.insert(out.end(), s, s + n); }
    // A section: its tag, then a length filled in when it ends.
    size_t begin(const char* tag) { out.insert(out.end(), tag, tag + 4); u32(0); return out.size(); }
    void end(size_t at) { uint32_t n = uint32_t(out.size() - at); for (int k = 0; k < 4; ++k) out[at - 4 + k] = uint8_t(n >> (8 * k)); }
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p(data), end_(data + size) {}
    bool ok = true;
    bool done() const { return p >= end_; }
    unsigned u8() { if (p >= end_) { ok = false; return 0; } return *p++; }
    unsigned u16() { unsigned a = u8(); return a | u8() << 8; }
    uint32_t u32() { uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
    int i16() { return int16_t(uint16_t(u16())); }
    void text(char* out, size_t size) {
        unsigned n = u8();
        for (unsigned k = 0; k < n; ++k) { char c = char(u8()); if (k + 1 < size) out[k] = c; }
        out[std::min<size_t>(n, size - 1)] = 0;
    }
    Reader section(char tag[5]) {
        for (int k = 0; k < 4; ++k) tag[k] = char(u8());
        tag[4] = 0;
        uint32_t n = u32();
        if (!ok || size_t(end_ - p) < n) { ok = false; return Reader(p, 0); }
        Reader inner(p, n);
        p += n;
        return inner;
    }
private:
    const uint8_t* p;
    const uint8_t* end_;
};

void write_chain(Writer& w, const Chain& chain) {
    w.u8(chain.size);
    unsigned count = chain.count();
    w.u8(count);
    for (unsigned k = 0; k < count; ++k) {
        const auto& s = chain.slots[k];
        w.u8(s.kind); w.u8(s.bypass);
        for (auto v : s.params) w.i16(v);
    }
}
void read_chain(Reader& r, Chain& chain) {
    chain = Chain {{}, uint8_t(std::clamp<unsigned>(r.u8(), 1, max_slots))};
    unsigned count = std::min<unsigned>(r.u8(), max_slots);
    for (unsigned k = 0; k < count; ++k) {
        auto& s = chain.slots[k];
        s.kind = uint8_t(r.u8()); s.bypass = r.u8() != 0;
        for (auto& v : s.params) v = int16_t(r.i16());
    }
}
void write_step(Writer& w, const Step& s) {
    unsigned notes = 0;
    for (auto n : s.notes) notes += n.velocity != 0;
    w.u8(notes);
    for (auto n : s.notes) if (n.velocity) { w.u8(n.key); w.u8(n.velocity); }
    w.u8(s.length); w.u8(s.chance); w.u8(s.flags);
}
void read_step(Reader& r, Step& s) {
    s = Step {};
    unsigned notes = std::min<unsigned>(r.u8(), notes_per_step);
    for (unsigned k = 0; k < notes; ++k) { s.notes[k].key = uint8_t(r.u8() & 127); s.notes[k].velocity = uint8_t(r.u8() & 127); }
    s.length = uint8_t(std::max(1u, r.u8())); s.chance = uint8_t(std::min(100u, r.u8())); s.flags = uint8_t(r.u8());
}
} // namespace

std::vector<uint8_t> save_project(const Project& project, const PatternBank& patterns) {
    Writer w;
    w.out.insert(w.out.end(), magic, magic + 8);
    auto at = w.begin("PROJ");
    w.text(project.name); w.u16(project.tempo); w.u8(project.swing); w.u8(project.root); w.u8(project.scale);
    w.end(at);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = project.tracks[t];
        at = w.begin("TRAK");
        w.u8(t); w.u8(unsigned(tr.engine)); w.u16(tr.preset); w.text(tr.name);
        w.u8(max_params); for (auto v : tr.params) w.i16(v);
        w.u8(tr.mix.level); w.i16(tr.mix.pan); w.u8(tr.mix.send); w.u8(tr.mix.bus); w.u8(tr.mix.mute); w.u8(tr.mix.solo);
        w.u8(tr.pattern); w.u8(unsigned(tr.layout)); w.i16(tr.octave); w.u8(tr.last_note);
        w.u8(mod_routes); for (auto m : tr.mods) { w.u8(m.source); w.u8(m.destination); w.i16(m.amount); }
        write_chain(w, tr.fx);
        w.u8(tr.bank); w.u8(tr.last_pad); w.u8(tr.levels); w.u8(tr.levels_pad); w.u8(tr.levels_kind);
        unsigned used = 0;
        for (const auto& pad : tr.pads) used += pad.used();
        w.u8(used);
        for (unsigned n = 0; n < sampler_pads; ++n) {
            const auto& pad = tr.pads[n];
            if (!pad.used()) continue;
            w.u8(n); w.u16(pad.sample); w.u32(pad.start); w.u32(pad.end); w.i16(pad.pitch); w.i16(pad.fine); w.u8(pad.level);
            w.i16(pad.pan); w.u8(unsigned(pad.play)); w.u8(pad.reverse); w.u8(pad.group);
            w.u8(pad.attack); w.u8(pad.decay); w.u8(pad.sustain); w.u8(pad.release); w.u16(pad.own_tempo);
        }
        w.end(at);
        for (unsigned p = 0; p < pattern_count; ++p) {
            const auto& pattern = patterns.at[t][p];
            bool plain = pattern.length == 16 && pattern.division == Division::Sixteenth && pattern.empty();
            if (plain) continue;
            at = w.begin("PATT");
            w.u8(t); w.u8(p); w.u16(pattern.length); w.u8(unsigned(pattern.division));
            unsigned steps = 0;
            for (const auto& s : pattern.steps) steps += !s.empty();
            w.u16(steps);
            for (unsigned s = 0; s < max_steps; ++s)
                if (!pattern.steps[s].empty()) { w.u16(s); write_step(w, pattern.steps[s]); }
            w.end(at);
        }
    }
    at = w.begin("MOTN");
    w.u16(project.motion.count);
    for (unsigned n = 0; n < project.motion.count; ++n) {
        const auto& e = project.motion.events[n];
        w.u8(e.track); w.u8(e.pattern); w.u16(e.step); w.u8(e.param); w.i16(e.value);
    }
    w.end(at);
    at = w.begin("SONG");
    const auto& song = project.song;
    w.u16(song.count);
    for (unsigned n = 0; n < song.count; ++n) {
        const auto& c = song.clips[n];
        w.u8(c.track); w.u8(c.pattern); w.u16(c.bar); w.u16(c.bars); w.u8(c.muted);
    }
    w.u8(song.marker_count);
    for (unsigned n = 0; n < song.marker_count; ++n) { w.u16(song.markers[n].bar); w.text(song.markers[n].name); }
    w.u16(song.loop_start); w.u16(song.loop_end);
    w.end(at);
    for (unsigned b = 0; b < bus_count; ++b) {
        at = w.begin("BUSS");
        w.u8(b); w.u8(project.buses[b].level); write_chain(w, project.buses[b].fx);
        w.end(at);
    }
    at = w.begin("MSTR");
    write_chain(w, project.master);
    w.end(at);
    for (unsigned n = 0; n < sample_slots; ++n) {
        const auto& s = project.samples[n];
        if (!s.used()) continue;
        at = w.begin("SMPL");
        w.u8(n); w.text(s.path); w.text(s.name);
        w.end(at);
    }
    return w.out;
}

std::string load_project(const std::vector<uint8_t>& data, Project& target, PatternBank& target_patterns) {
    if (data.size() < 8 || std::memcmp(data.data(), magic, 8)) return "not a Melodee project";
    auto project = std::make_unique<Project>();
    auto patterns = std::make_unique<PatternBank>();
    Reader file(data.data() + 8, data.size() - 8);
    while (!file.done() && file.ok) {
        char tag[5];
        Reader r = file.section(tag);
        std::string name = tag;
        if (name == "PROJ") {
            r.text(project->name, sizeof(project->name));
            project->tempo = uint16_t(std::clamp<unsigned>(r.u16(), 300, 3000));
            project->swing = uint8_t(std::clamp<unsigned>(r.u8(), 50, 75));
            project->root = uint8_t(r.u8() % 12); project->scale = uint8_t(r.u8());
        } else if (name == "TRAK") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            auto& tr = project->tracks[t];
            tr.engine = EngineKind(std::min<unsigned>(r.u8(), unsigned(EngineKind::Count) - 1));
            tr.preset = uint16_t(r.u16()); r.text(tr.name, sizeof(tr.name));
            unsigned params = r.u8();
            for (unsigned k = 0; k < params; ++k) { int v = r.i16(); if (k < max_params) tr.params[k] = int16_t(v); }
            tr.mix.level = uint8_t(std::min(127u, r.u8())); tr.mix.pan = int8_t(std::clamp(r.i16(), -50, 50));
            tr.mix.send = uint8_t(std::min(100u, r.u8())); tr.mix.bus = uint8_t(r.u8() % bus_count);
            tr.mix.mute = r.u8() != 0; tr.mix.solo = r.u8() != 0;
            tr.pattern = uint8_t(r.u8() % pattern_count); tr.layout = PadLayout(std::min<unsigned>(r.u8(), unsigned(PadLayout::Count) - 1));
            tr.octave = int8_t(std::clamp(r.i16(), 0, 7)); tr.last_note = uint8_t(r.u8() & 127);
            unsigned routes = r.u8();
            for (unsigned k = 0; k < routes; ++k) {
                ModRoute m {uint8_t(r.u8()), uint8_t(r.u8()), int8_t(std::clamp(r.i16(), -100, 100))};
                if (k < mod_routes) tr.mods[k] = m;
            }
            read_chain(r, tr.fx);
            tr.bank = uint8_t(r.u8() % sampler_banks); tr.last_pad = uint8_t(r.u8() % sampler_pads);
            tr.levels = r.u8() != 0; tr.levels_pad = uint8_t(r.u8() % sampler_pads); tr.levels_kind = uint8_t(r.u8() % 3);
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) {
                unsigned n = r.u8() % sampler_pads;
                auto& pad = tr.pads[n];
                pad.sample = uint16_t(r.u16()); pad.start = r.u32(); pad.end = r.u32();
                pad.pitch = int8_t(std::clamp(r.i16(), -24, 24)); pad.fine = int8_t(std::clamp(r.i16(), -50, 50));
                pad.level = uint8_t(std::min(127u, r.u8())); pad.pan = int8_t(std::clamp(r.i16(), -50, 50));
                pad.play = PadPlay(r.u8() & 1); pad.reverse = r.u8() != 0; pad.group = uint8_t(r.u8() % 9);
                pad.attack = uint8_t(r.u8()); pad.decay = uint8_t(r.u8()); pad.sustain = uint8_t(r.u8()); pad.release = uint8_t(r.u8());
                pad.own_tempo = uint16_t(r.u16());
                if (pad.sample >= sample_slots) pad.sample = no_sample;
            }
        } else if (name == "PATT") {
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            pattern.length = uint16_t(std::clamp<unsigned>(r.u16(), 1, max_steps));
            pattern.division = Division(std::min<unsigned>(r.u8(), unsigned(Division::Count) - 1));
            unsigned steps = r.u16();
            for (unsigned k = 0; k < steps && r.ok; ++k) {
                unsigned s = r.u16();
                Step step;
                read_step(r, step);
                if (s < max_steps) pattern.steps[s] = step;
            }
        } else if (name == "MOTN") {
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                MotionEvent e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16() % max_steps);
                e.param = uint8_t(r.u8()); e.value = int16_t(r.i16());
                project->motion.write(e);
            }
        } else if (name == "SONG") {
            auto& song = project->song;
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                Clip c;
                c.track = uint8_t(r.u8() % track_count); c.pattern = uint8_t(r.u8() % pattern_count);
                c.bar = uint16_t(std::min<unsigned>(r.u16(), song_bars)); c.bars = uint16_t(std::clamp<unsigned>(r.u16(), 1, 16)); c.muted = r.u8() != 0;
                if (song.count < song.clips.size()) song.clips[song.count++] = c;
            }
            unsigned markers = r.u8();
            for (unsigned k = 0; k < markers && r.ok; ++k) {
                Marker m;
                m.bar = uint16_t(r.u16()); r.text(m.name, sizeof(m.name));
                if (song.marker_count < song.markers.size()) song.markers[song.marker_count++] = m;
            }
            song.loop_start = uint16_t(r.u16()); song.loop_end = uint16_t(r.u16());
        } else if (name == "BUSS") {
            unsigned b = r.u8();
            if (b >= bus_count) continue;
            project->buses[b].level = uint8_t(std::min(100u, r.u8()));
            read_chain(r, project->buses[b].fx);
        } else if (name == "MSTR") {
            read_chain(r, project->master);
        } else if (name == "SMPL") {
            unsigned n = r.u8();
            if (n >= sample_slots) continue;
            r.text(project->samples[n].path, sizeof(project->samples[n].path));
            r.text(project->samples[n].name, sizeof(project->samples[n].name));
        }
        if (!r.ok) return "damaged section " + name;
    }
    if (!file.ok) return "damaged file";
    target = *project;
    target_patterns = *patterns;
    return {};
}

} // namespace melodee
