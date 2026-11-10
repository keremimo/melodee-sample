// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/project_file.h"
#include "melodee/model/pad_params.h"
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
// One kit pad's sound: the same record in TRAK (banks A-D) and KITX (E-H).
void write_pad(Writer& w, unsigned n, const PadSound& pad) {
    w.u8(n); w.u16(pad.sample); w.u32(pad.start); w.u32(pad.end); w.i16(pad.pitch); w.i16(pad.fine); w.u8(pad.level);
    w.i16(pad.pan); w.u8(unsigned(pad.play)); w.u8(pad.reverse); w.u8(pad.group);
    w.u8(pad.attack); w.u8(pad.decay); w.u8(pad.sustain); w.u8(pad.release); w.u16(pad.own_tempo);
}
void read_pad(Reader& r, Track& tr) {
    unsigned n = r.u8() % sampler_pads;
    auto& pad = tr.pads[n];
    pad.sample = uint16_t(r.u16()); pad.start = r.u32(); pad.end = r.u32();
    pad.pitch = int8_t(std::clamp(r.i16(), -24, 24)); pad.fine = int8_t(std::clamp(r.i16(), -50, 50));
    pad.level = uint8_t(std::min(127u, r.u8())); pad.pan = int8_t(std::clamp(r.i16(), -50, 50));
    pad.play = PadPlay(r.u8() & 1); pad.reverse = r.u8() != 0; pad.group = uint8_t(r.u8() % 17);
    pad.attack = uint8_t(r.u8()); pad.decay = uint8_t(r.u8()); pad.sustain = uint8_t(r.u8()); pad.release = uint8_t(r.u8());
    pad.own_tempo = uint16_t(r.u16());
    if (pad.sample >= sample_slots) pad.sample = no_sample;
}

// A kit pad's Sample Mode values (parity phase 4), in PAD4: older builds
// skip the section and older files load with the MPC's defaults, except
// that their pads decay from the start, as they did.
void write_pad4(Writer& w, unsigned n, const PadSound& pad) {
    w.u8(n);
    w.u8(unsigned(pad.loop) | unsigned(pad.loop_lock) << 1 | unsigned(pad.mono) << 2 | unsigned(pad.decay_from_start) << 3);
    w.u32(pad.loop_start); w.u8(pad.link); w.u8(pad.offset); w.u8(pad.velocity_sense);
    w.u8(unsigned(pad.filter)); w.u8(pad.cutoff); w.u8(pad.resonance);
    w.u8(pad.filter_attack); w.u8(pad.filter_decay); w.u8(pad.filter_sustain); w.u8(pad.filter_release); w.u8(pad.filter_amount);
    w.u8(pad.warp); w.u8(pad.beats); w.i16(pad.gain);
}
void read_pad4(Reader& r, Track& tr) {
    auto& pad = tr.pads[r.u8() % sampler_pads];
    unsigned flags = r.u8();
    pad.loop = flags & 1; pad.loop_lock = (flags >> 1) & 1; pad.mono = (flags >> 2) & 1; pad.decay_from_start = (flags >> 3) & 1;
    pad.loop_start = r.u32(); pad.link = uint8_t(r.u8() % 17); pad.offset = uint8_t(std::min(100u, r.u8()));
    pad.velocity_sense = uint8_t(std::min(100u, r.u8()));
    pad.filter = PadFilter(std::min(r.u8(), pad_filter_count - 1));
    pad.cutoff = uint8_t(std::min(100u, r.u8())); pad.resonance = uint8_t(std::min(100u, r.u8()));
    pad.filter_attack = uint8_t(std::min(100u, r.u8())); pad.filter_decay = uint8_t(std::min(100u, r.u8()));
    pad.filter_sustain = uint8_t(std::min(100u, r.u8())); pad.filter_release = uint8_t(std::min(100u, r.u8()));
    pad.filter_amount = uint8_t(std::min(100u, r.u8()));
    unsigned warp = r.u8();
    pad.warp = uint8_t(warp == warp_seq || (warp >= warp_min && warp <= warp_max) ? warp : warp_off);
    pad.beats = uint8_t(std::min(max_pad_beats, r.u8()));
    pad.gain = int16_t(std::clamp(r.i16(), -2000, 6000));
}
// A step as files before parity phase 2 kept it (PATT).
void read_step(Reader& r, Step& s) {
    s = Step {};
    unsigned notes = std::min<unsigned>(r.u8(), notes_per_step);
    // Keys keep the whole byte: kit pads in banks F-H are notes past 127 (model/sampler.h).
    for (unsigned k = 0; k < notes; ++k) { s.notes[k].key = uint8_t(r.u8()); s.notes[k].velocity = uint8_t(r.u8() & 127); }
    s.length = uint8_t(std::max(1u, r.u8())); s.chance = uint8_t(std::min(100u, r.u8())); s.flags = uint8_t(r.u8());
}
} // namespace

std::vector<uint8_t> save_project(const Project& project, const PatternBank& patterns) {
    Writer w;
    w.out.insert(w.out.end(), magic, magic + 8);
    auto at = w.begin("PROJ");
    w.text(project.name); w.u16(project.tempo); w.u8(project.swing); w.u8(project.root); w.u8(project.scale);
    w.end(at);
    at = w.begin("PVER"); // which engine parameters TRAK's values cover
    w.u8(params_version);
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
        unsigned used = 0; // banks A-D here, as before; E-H in KITX
        for (unsigned n = 0; n < legacy_pads; ++n) used += tr.pads[n].used();
        w.u8(used);
        for (unsigned n = 0; n < legacy_pads; ++n) if (tr.pads[n].used()) write_pad(w, n, tr.pads[n]);
        w.end(at);
        for (unsigned p = 0; p < pattern_count; ++p) { // each pattern's notes, on the 96-a-quarter clock
            const auto& pattern = patterns.at[t][p];
            bool plain = pattern.length == 16 * sixteenth_ticks && pattern.division == Division::Sixteenth && pattern.events.empty();
            if (plain) continue;
            at = w.begin("EVNT");
            w.u8(t); w.u8(p); w.u32(pattern.length); w.u8(unsigned(pattern.division));
            w.u32(uint32_t(pattern.events.size()));
            for (const auto& e : pattern.events) { w.u32(e.tick); w.u16(e.length); w.u8(e.key); w.u8(e.velocity); w.u8(e.chance); w.u8(e.flags); }
            w.end(at);
        }
    }
    at = w.begin("TIME"); // time signature, BPM per pattern
    w.u8(project.signature); w.u8(project.seq_tempo);
    for (auto v : project.pattern_tempo) w.u16(v);
    w.end(at);
    // Motion: engine and effect lanes in MOT2 (steps in sixteenths; MOTN
    // counted the pattern's own steps), a kit pad's lanes in MOTP with their
    // pad, so files from before parity phase 3 and the builds that read them
    // never meet a pad lane.
    unsigned pad_lanes = 0;
    for (unsigned n = 0; n < project.motion.count; ++n) pad_lanes += project.motion.events[n].param >= motion_pad_base;
    at = w.begin("MOT2");
    w.u16(uint16_t(project.motion.count - pad_lanes));
    for (unsigned n = 0; n < project.motion.count; ++n) {
        const auto& e = project.motion.events[n];
        if (e.param >= motion_pad_base) continue;
        w.u8(e.track); w.u8(e.pattern); w.u16(e.step); w.u8(e.param); w.i16(e.value);
    }
    w.end(at);
    if (pad_lanes) {
        at = w.begin("MOTP"); // pad lanes: track, pattern, sixteenth, kit pad, which value (PadParam), value
        w.u16(uint16_t(pad_lanes));
        for (unsigned n = 0; n < project.motion.count; ++n) {
            const auto& e = project.motion.events[n];
            if (e.param < motion_pad_base) continue;
            w.u8(e.track); w.u8(e.pattern); w.u16(e.step); w.u8(e.pad); w.u8(uint8_t(e.param - motion_pad_base)); w.i16(e.value);
        }
        w.end(at);
    }
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
    at = w.begin("TRNS"); // each track's transposition
    for (unsigned t = 0; t < track_count; ++t) w.i16(project.tracks[t].transpose);
    w.end(at);
    at = w.begin("SPCH"); // punches written into the song
    w.u16(project.song.punch_count);
    for (unsigned n = 0; n < project.song.punch_count; ++n) {
        const auto& e = project.song.punches[n];
        w.u16(e.bar); w.u8(e.step); w.u8(e.length); w.u8(e.kind); w.u8(e.target); w.u8(e.depth);
    }
    w.end(at);
    at = w.begin("PDFX"); // each sampler pad's own chain
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned n = 0; n < sampler_pads; ++n) {
            const auto& chain = project.tracks[t].pad_fx.at[n];
            if (chain.count()) { w.u8(t); w.u8(n); write_chain(w, chain); }
        }
    w.u8(255);
    w.end(at);
    at = w.begin("PNCH");
    w.u8(project.punch_target); w.u8(project.punch_bank); w.u8(project.punch_depth); w.u8(project.punch_snap); w.u8(project.punch_tail);
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned n = 0; n < sampler_pads; ++n) {
            const auto& pad = project.tracks[t].pads[n];
            if (pad.punch) { w.u8(t); w.u8(n); w.u8(pad.punch); w.u8(pad.punch_rate); w.u8(pad.punch_depth); }
        }
    w.u8(255);
    w.end(at);
    at = w.begin("KNFX"); // Knob FX (parity phase 7): the entry, its effect and values, bypass, the tracks and kit pads it is on
    {
        const auto& k = project.knob_fx;
        w.u8(k.choice); w.u8(k.kind); w.u8(k.bypass);
        for (auto v : k.params) w.i16(v);
        w.u16(k.tracks);
        for (unsigned t = 0; t < track_count; ++t)
            if (k.any_pad(t)) { w.u8(t); w.u32(uint32_t(k.pads[t][0])); w.u32(uint32_t(k.pads[t][0] >> 32)); w.u32(uint32_t(k.pads[t][1])); w.u32(uint32_t(k.pads[t][1] >> 32)); }
        w.u8(255);
    }
    w.end(at);
    at = w.begin("PFXP"); // Pad FX's K1-K3 for the MPC's sixteen, Flex Beat's settings (parity phase 7b)
    for (const auto& one : project.pad_fx_params) for (auto v : one) w.i16(v);
    w.u8(project.flex.mode); w.u8(project.flex.mix); w.u8(project.flex.quantize);
    w.end(at);
    at = w.begin("CMPR"); // the master compressor (parity phase 7)
    {
        const auto& c = project.compressor;
        w.u8(c.on); w.u8(c.color); w.u8(c.attack); w.u8(c.release); w.u8(c.amount); w.u8(c.boost);
    }
    w.end(at);
    for (unsigned t = 0; t < track_count; ++t) { // kit pads in banks E-H (parity phase 1c)
        const auto& tr = project.tracks[t];
        unsigned used = 0;
        for (unsigned n = legacy_pads; n < sampler_pads; ++n) used += tr.pads[n].used();
        if (!used) continue;
        at = w.begin("KITX");
        w.u8(t); w.u8(used);
        for (unsigned n = legacy_pads; n < sampler_pads; ++n) if (tr.pads[n].used()) write_pad(w, n, tr.pads[n]);
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // every used kit pad's Sample Mode values (parity phase 4)
        const auto& tr = project.tracks[t];
        unsigned used = 0;
        for (const auto& pad : tr.pads) used += pad.used();
        if (!used) continue;
        at = w.begin("PAD4");
        w.u8(t); w.u8(used);
        for (unsigned n = 0; n < sampler_pads; ++n) if (tr.pads[n].used()) write_pad4(w, n, tr.pads[n]);
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // muted kit pads, a bit each
        const auto& tr = project.tracks[t];
        if (!tr.any_pad_muted()) continue;
        at = w.begin("PMUT");
        w.u8(t); w.u8(unsigned(tr.pad_mutes.size()));
        for (auto bits : tr.pad_mutes) { w.u32(uint32_t(bits)); w.u32(uint32_t(bits >> 32)); }
        w.end(at);
    }
    for (unsigned n = 0; n < sample_slots; ++n) {
        const auto& s = project.samples[n];
        if (!s.used()) continue;
        at = w.begin("SMPL");
        w.u8(n); w.text(s.path); w.text(s.name);
        w.end(at);
    }
    return w.out;
}

std::string load_project(const std::vector<uint8_t>& data, Project& target, PatternBank& target_patterns, unsigned* version) {
    unsigned file_version = 0;
    if (data.size() < 8 || std::memcmp(data.data(), magic, 8)) return "not a Melodee project";
    auto project = std::make_unique<Project>();
    auto patterns = std::make_unique<PatternBank>();
    std::array<bool, track_count> pad4 {};  // tracks whose pads had a PAD4 section
    std::vector<MotionEvent> legacy_motion; // MOTN: steps of each pattern's division, turned into sixteenths at the end
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
            tr.levels = r.u8() != 0; tr.levels_pad = uint8_t(r.u8() % sampler_pads); tr.levels_kind = uint8_t(r.u8() % 4);
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad(r, tr);
        } else if (name == "PVER") {
            file_version = r.u8();
        } else if (name == "KITX") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad(r, project->tracks[t]);
        } else if (name == "PAD4") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            pad4[t] = true;
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad4(r, project->tracks[t]);
        } else if (name == "PMUT") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            unsigned words = r.u8();
            for (unsigned k = 0; k < words && r.ok; ++k) {
                uint64_t bits = r.u32();
                bits |= uint64_t(r.u32()) << 32;
                if (k < project->tracks[t].pad_mutes.size()) project->tracks[t].pad_mutes[k] = bits;
            }
        } else if (name == "PATT") { // before parity phase 2: steps of the pattern's division, 24 ticks a quarter
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            unsigned steps = std::clamp<unsigned>(r.u16(), 1, 256);
            pattern.division = legacy_division(r.u8());
            unsigned q = pattern.q();
            pattern.length = steps * q;
            pattern.events.clear();
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                unsigned s = r.u16();
                Step step;
                read_step(r, step);
                if (s >= steps) continue;
                for (const auto& n : step.notes) {
                    if (!n.velocity) continue;
                    Event e;
                    e.tick = s * q; e.length = uint16_t(step.length * q);
                    e.key = n.key; e.velocity = n.velocity; e.chance = step.chance; e.flags = step.flags;
                    pattern.add(e);
                }
            }
        } else if (name == "EVNT") {
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            pattern.length = std::clamp<uint32_t>(r.u32(), 1, max_bars * 16 * ticks_per_quarter);
            pattern.division = Division(std::min<unsigned>(r.u8(), unsigned(Division::Count) - 1));
            uint32_t count = std::min<uint32_t>(r.u32(), max_pattern_events);
            pattern.events.clear();
            pattern.events.reserve(count);
            for (uint32_t k = 0; k < count && r.ok; ++k) {
                Event e;
                e.tick = r.u32(); e.length = uint16_t(std::max(1u, r.u16()));
                e.key = uint8_t(r.u8()); e.velocity = uint8_t(std::clamp(r.u8(), 1u, 127u));
                e.chance = uint8_t(std::min(100u, r.u8())); e.flags = uint8_t(r.u8());
                if (r.ok) pattern.events.push_back(e);
            }
            pattern.sort();
        } else if (name == "MOTN" || name == "MOT2") {
            bool legacy = name == "MOTN";
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                MotionEvent e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16());
                e.param = uint8_t(r.u8()); e.value = int16_t(r.i16());
                if (e.param >= motion_pad_base) continue; // pad lanes live in MOTP
                if (legacy) legacy_motion.push_back(e);
                else project->motion.write(e);
            }
        } else if (name == "MOTP") { // a kit pad's lanes (parity phase 3)
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                MotionEvent e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16());
                e.pad = uint8_t(r.u8() % sampler_pads);
                unsigned which = r.u8();
                e.value = int16_t(r.i16());
                if (r.ok && which < pad_param_count) { e.param = uint8_t(motion_pad_param(PadParam(which))); project->motion.write(e); }
            }
        } else if (name == "TIME") {
            project->signature = uint8_t(std::min<unsigned>(r.u8(), time_signature_count - 1));
            project->seq_tempo = r.u8() != 0;
            for (auto& v : project->pattern_tempo) { unsigned bpm = r.u16(); v = uint16_t(bpm ? std::clamp<unsigned>(bpm, 300, 3000) : 0); }
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
        } else if (name == "TRNS") {
            for (unsigned t = 0; t < track_count && r.ok; ++t) project->tracks[t].transpose = int8_t(std::clamp(r.i16(), -12, 12));
        } else if (name == "SPCH") {
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                SongPunch e;
                e.bar = uint16_t(std::min<unsigned>(r.u16(), song_bars)); e.step = uint8_t(r.u8() % 16); e.length = uint8_t(std::max(1u, r.u8()));
                e.kind = uint8_t(r.u8() % punch_kinds); e.target = uint8_t(r.u8()); e.depth = uint8_t(std::min(100u, r.u8()));
                if (e.target != 255 && e.target >= track_count) e.target = 255;
                if (r.ok) project->song.add_punch(e);
            }
        } else if (name == "PDFX") {
            while (r.ok && !r.done()) {
                unsigned t = r.u8();
                if (t >= track_count) break;
                auto& chain = project->tracks[t].pad_fx.at[r.u8() % sampler_pads];
                read_chain(r, chain);
                for (unsigned k = pad_slots; k < max_slots; ++k) chain.slots[k] = EffectSlot {};
                chain.size = pad_slots;
            }
        } else if (name == "PNCH") {
            project->punch_target = uint8_t(r.u8() % 2); project->punch_bank = uint8_t(r.u8() % 3);
            project->punch_depth = uint8_t(std::min(100u, r.u8())); project->punch_snap = uint8_t(r.u8() % 4); project->punch_tail = uint8_t(r.u8() % 3);
            while (r.ok && !r.done()) {
                unsigned t = r.u8();
                if (t >= track_count) break;
                auto& pad = project->tracks[t].pads[r.u8() % sampler_pads];
                pad.punch = uint8_t(std::min(punch_kinds, r.u8())); pad.punch_rate = uint8_t(r.u8() % 4); pad.punch_depth = uint8_t(std::min(100u, r.u8()));
            }
        } else if (name == "KNFX") {
            auto& k = project->knob_fx;
            k.choice = uint8_t(r.u8()); k.kind = uint8_t(r.u8()); k.bypass = r.u8() != 0;
            if (k.kind >= effect_kinds) k.kind = 0;
            for (auto& v : k.params) v = int16_t(r.i16());
            k.tracks = uint16_t(r.u16());
            while (r.ok && !r.done()) {
                unsigned t = r.u8();
                if (t >= track_count) break;
                uint64_t words[4];
                for (auto& w : words) w = r.u32();
                k.pads[t][0] = words[0] | words[1] << 32; k.pads[t][1] = words[2] | words[3] << 32;
            }
        } else if (name == "PFXP") {
            for (auto& one : project->pad_fx_params) for (auto& v : one) v = int16_t(r.i16());
            project->flex.mode = uint8_t(r.u8() & 1); project->flex.mix = uint8_t(std::min(100u, r.u8())); project->flex.quantize = r.u8() != 0;
        } else if (name == "CMPR") {
            auto& c = project->compressor;
            c.on = r.u8() != 0; c.color = r.u8() != 0;
            c.attack = uint8_t(std::min(100u, r.u8())); c.release = uint8_t(std::min(100u, r.u8()));
            c.amount = uint8_t(std::min(100u, r.u8())); c.boost = uint8_t(std::min(24u, r.u8()));
        } else if (name == "SMPL") {
            unsigned n = r.u8();
            if (n >= sample_slots) continue;
            r.text(project->samples[n].path, sizeof(project->samples[n].path));
            r.text(project->samples[n].name, sizeof(project->samples[n].name));
        }
        if (!r.ok) return "damaged section " + name;
    }
    if (!file.ok) return "damaged file";
    for (unsigned t = 0; t < track_count; ++t) // before parity phase 4 a pad's decay ran from its start
        if (!pad4[t]) for (auto& pad : project->tracks[t].pads) pad.decay_from_start = true;
    for (auto e : legacy_motion) { // the pattern's own steps into sixteenths
        unsigned q = patterns->at[e.track][e.pattern].q();
        e.step = uint16_t((e.step * q + sixteenth_ticks / 2) / sixteenth_ticks);
        project->motion.write(e);
    }
    target = *project;
    target_patterns = *patterns;
    if (version) *version = file_version;
    return {};
}

} // namespace melodee
