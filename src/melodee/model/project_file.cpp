// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/project_file.h"
#include "melodee/model/fader.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/scales.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/midi_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace melodee {
namespace {
constexpr char magic[8] {'M', 'E', 'L', 'O', 'D', 'E', 'E', '1'};
constexpr unsigned loop_record = 15; // LOOP: bars, feedback, source, frames, ticks, tempo, layers, stopped

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
// Into `pads` (`count` of them; a project's kit or a bank file), `samples` the slots its sample may name.
void read_pad(Reader& r, PadSound* pads, unsigned count, unsigned samples = sample_slots) {
    auto& pad = pads[r.u8() % count];
    pad.sample = uint16_t(r.u16()); pad.start = r.u32(); pad.end = r.u32();
    pad.pitch = int8_t(std::clamp(r.i16(), -24, 24)); pad.fine = int8_t(std::clamp(r.i16(), -50, 50));
    pad.level = uint8_t(std::min(127u, r.u8())); pad.pan = int8_t(std::clamp(r.i16(), -50, 50));
    pad.play = PadPlay(r.u8() & 1); pad.reverse = r.u8() != 0; pad.group = uint8_t(r.u8() % 17);
    pad.attack = uint8_t(r.u8()); pad.decay = uint8_t(r.u8()); pad.sustain = uint8_t(r.u8()); pad.release = uint8_t(r.u8());
    pad.own_tempo = uint16_t(r.u16());
    if (pad.sample >= samples) pad.sample = no_sample;
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
void read_pad4(Reader& r, PadSound* pads, unsigned count) {
    auto& pad = pads[r.u8() % count];
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

std::vector<uint8_t> save_project(const Project& project, const PatternBank& patterns, const std::array<std::vector<uint8_t>, max_plugins>* states) {
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
        // A MIDI or PLUGIN track is None here and itself in MIDT / PLGT, so older builds load it empty (not as the engine
        // after their last).
        bool own_section = tr.engine == EngineKind::Midi || tr.engine == EngineKind::Plugin;
        w.u8(t); w.u8(own_section ? 0u : unsigned(tr.engine)); w.u16(tr.preset); w.text(tr.name);
        w.u8(max_params); for (auto v : tr.params) w.i16(v);
        // Older builds know one send: the largest of the four, into its bus (SNDS, later, has all four).
        unsigned loudest = unsigned(std::max_element(tr.mix.sends.begin(), tr.mix.sends.end()) - tr.mix.sends.begin());
        w.u8(tr.mix.level); w.i16(tr.mix.pan); w.u8(tr.mix.sends[loudest]); w.u8(loudest); w.u8(tr.mix.mute); w.u8(tr.mix.solo);
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
            bool plain = pattern.length == 16 * sixteenth_ticks && pattern.division == Division::Sixteenth && pattern.events.empty() &&
                         pattern.motion.empty() && pattern.lanes.empty() && pattern.feel == Feel {} && pattern.euclids.empty();
            if (plain) continue;
            at = w.begin("EVNT");
            w.u8(t); w.u8(p); w.u32(pattern.length); w.u8(unsigned(pattern.division));
            w.u32(uint32_t(pattern.events.size()));
            for (const auto& e : pattern.events) { w.u32(e.tick); w.u16(e.length); w.u8(e.key); w.u8(e.velocity); w.u8(e.chance); w.u8(e.flags); }
            w.end(at);
            uint32_t pitched = 0;
            for (const auto& e : pattern.events) pitched += e.pitch != 0;
            if (pitched) { // what older builds skip: the notes that play off their pad's root, by their place in EVNT
                at = w.begin("PTCH");
                w.u8(t); w.u8(p); w.u32(pitched);
                for (uint32_t n = 0; n < pattern.events.size(); ++n)
                    if (pattern.events[n].pitch) { w.u32(n); w.u8(uint8_t(pattern.events[n].pitch)); }
                w.end(at);
            }
            uint32_t ratchets = 0;
            for (const auto& e : pattern.events) ratchets += e.hits() > 1 || e.ramp || e.pitch_ramp;
            if (ratchets) { // ratchets (roadmap P1), by their place in EVNT: hits, velocity ramp, pitch ramp
                at = w.begin("RTCH");
                w.u8(t); w.u8(p); w.u32(ratchets);
                for (uint32_t n = 0; n < pattern.events.size(); ++n) {
                    const auto& e = pattern.events[n];
                    if (e.hits() > 1 || e.ramp || e.pitch_ramp) { w.u32(n); w.u8(e.hits()); w.u8(uint8_t(e.ramp)); w.u8(uint8_t(e.pitch_ramp)); }
                }
                w.end(at);
            }
            uint32_t conditions = 0;
            for (const auto& e : pattern.events) conditions += e.condition != trig_always;
            if (conditions) { // trig conditions (roadmap P1, model/trig.h), by their place in EVNT
                at = w.begin("COND");
                w.u8(t); w.u8(p); w.u32(conditions);
                for (uint32_t n = 0; n < pattern.events.size(); ++n)
                    if (pattern.events[n].condition != trig_always) { w.u32(n); w.u8(pattern.events[n].condition); }
                w.end(at);
            }
            if (pattern.feel != Feel {}) { // its feel (roadmap P1): strength, swing, humanize, its groove
                const auto& f = pattern.feel;
                at = w.begin("FEEL");
                w.u8(t); w.u8(p); w.u8(f.strength); w.u8(f.swing); w.u8(f.humanize); w.u8(f.humanize_velocity);
                w.u16(f.groove_from); w.u8(f.groove_amount); w.u16(f.groove_step);
                w.u8(unsigned(f.groove_offsets.size()));
                for (size_t n = 0; n < f.groove_offsets.size(); ++n) { w.u8(uint8_t(f.groove_offsets[n])); w.u8(n < f.groove_levels.size() ? f.groove_levels[n] : 100); }
                w.end(at);
            }
            if (!pattern.euclids.empty()) { // the Euclidean rhythms laid on its kit lanes (roadmap P1): key, hits, steps, rotation
                at = w.begin("EUCL");
                w.u8(t); w.u8(p); w.u8(unsigned(pattern.euclids.size()));
                for (const auto& e : pattern.euclids) { w.u8(e.key); w.u8(e.hits); w.u8(e.steps); w.u8(e.rotate); }
                w.end(at);
            }
            if (!pattern.lanes.empty()) { // kit lanes of their own length (roadmap P1): key, ticks
                at = w.begin("LANE");
                w.u8(t); w.u8(p); w.u8(unsigned(pattern.lanes.size()));
                for (const auto& l : pattern.lanes) { w.u8(l.key); w.u32(l.length); }
                w.end(at);
            }
            if (!pattern.motion.empty()) { // Motion (roadmap P1): the pattern's own points at their ticks, locks with their hold
                at = w.begin("MOTK");
                w.u8(t); w.u8(p); w.u32(uint32_t(pattern.motion.size()));
                for (const auto& m : pattern.motion) { w.u32(m.tick); w.u8(m.param); w.u8(m.pad); w.i16(m.value); w.u16(m.hold); }
                w.end(at);
            }
        }
    }
    bool sped = false; // the tracks' playback speeds (roadmap P1), when one is not x1
    for (const auto& tr : project.tracks) sped |= tr.speed != speed_normal;
    if (sped) {
        at = w.begin("TSPD");
        w.u8(track_count);
        for (const auto& tr : project.tracks) w.u8(tr.speed);
        w.end(at);
    }
    at = w.begin("TIME"); // time signature, BPM per pattern
    w.u8(project.signature); w.u8(project.seq_tempo);
    for (auto v : project.pattern_tempo) w.u16(v);
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
    // The Looper (docs/LOOPER.md): on, its settings, each loop's set length, feedback, source and the audio kept
    // beside the file (frames, ticks in 1/65536, tempo, layers); its tracks' strips and chains.
    bool loops_used = project.looper.on;
    for (unsigned l = 0; l < loop_tracks; ++l) loops_used |= project.loop_fx[l].count() > 0;
    if (loops_used) {
        const auto& lp = project.looper;
        at = w.begin("LOOP");
        w.u8(lp.on); w.u8(lp.quantize); w.u8(lp.then); w.u8(lp.transport);
        w.u8(loop_record); // each loop's record is this long; a reader skips what it does not know, takes defaults for what is missing
        for (const auto& l : lp.loops) {
            w.u8(l.bars); w.u8(l.feedback); w.u8(l.source); w.u32(l.frames);
            w.u32(uint32_t(std::clamp(l.ticks, 0.0, 65535.0) * 65536.0)); w.u16(l.tempo); w.u8(l.layers); w.u8(l.stopped);
        }
        w.end(at);
        at = w.begin("LPMX");
        for (unsigned l = 0; l < loop_tracks; ++l) {
            const auto& m = project.loop_mix[l];
            w.u8(m.level); w.i16(m.pan); w.u8(m.send); w.u8(m.bus); w.u8(m.mute); w.u8(m.solo);
            write_chain(w, project.loop_fx[l]);
        }
        w.end(at);
    }
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
        w.u8(1); // Knob FX C, after what older builds read (version 1): A and B of K1-K3, the fader's place between them
        for (auto v : k.a) w.i16(v);
        for (auto v : k.b) w.i16(v);
        w.u8(k.morph);
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
        w.u8(c.sidechain); w.u8(c.key); // Sidechain (older files end before it: off)
        w.u8(c.key_pad);                // its key pad (older files: the whole track)
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
    for (unsigned t = 0; t < track_count; ++t) { // the four sends of a track that sends to more than one bus (round 2)
        const auto& sends = project.tracks[t].mix.sends;
        if (std::count_if(sends.begin(), sends.end(), [](uint8_t v) { return v > 0; }) < 2) continue; // TRAK says it already
        at = w.begin("SNDS");
        w.u8(t); w.u8(bus_count);
        for (auto v : sends) w.u8(v);
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // 16 Levels' ends where they are not as they come (round 2)
        const auto& tr = project.tracks[t];
        if (tr.levels_ends == Track {}.levels_ends) continue;
        at = w.begin("LVLE");
        w.u8(t); w.u8(4);
        for (const auto& e : tr.levels_ends) { w.u8(uint8_t(e[0])); w.u8(uint8_t(e[1])); }
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // the fader's target where one is picked, or it is off (parity phase 8)
        const auto& tr = project.tracks[t];
        if (tr.fader_target == fader_by_engine && tr.fader_on && !tr.fader_learned.learned() && !tr.fader_pin) continue;
        at = w.begin("FADR");
        w.u8(t); w.u8(tr.fader_target); w.u8(tr.fader_on);
        const auto& l = tr.fader_learned; // fader C, after what older builds read: the pot it learned, and PIN
        w.u8(uint8_t(l.what)); w.u16(l.lane); w.u8(l.engine); w.u8(l.pot); w.u8(tr.fader_pin);
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // a pattern chain (seq-sheet2 B): the patterns in the order they play
        const auto& tr = project.tracks[t];
        if (tr.chain_count < 2) continue;
        at = w.begin("CHAN");
        w.u8(t); w.u8(tr.chain_count);
        for (unsigned n = 0; n < tr.chain_count; ++n) w.u8(tr.chain[n]);
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // a MIDI track (docs/MIDI_TRACKS.md): its port and channel are in TRAK's params
        if (project.tracks[t].engine != EngineKind::Midi) continue;
        at = w.begin("MIDT");
        w.u8(t); w.u8(2); // the section's version: 2 has bank, program and the CCs in TRAK's params; later ones add more after it
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // a PLUGIN track and the entry of PLUG it plays; TRAK said None
        const auto& tr = project.tracks[t];
        if (tr.engine != EngineKind::Plugin) continue;
        at = w.begin("PLGT");
        w.u8(t); w.u8(tr.plugin);
        w.end(at);
    }
    for (unsigned e = 0; e < max_plugins; ++e) { // the plug-ins: the entry, its path, its state as it saved itself
        const auto& ref = project.plugins[e];
        if (!ref.used()) continue;
        at = w.begin("PLUG");
        size_t length = std::strlen(ref.path);
        w.u8(e); w.u16(unsigned(length));
        w.out.insert(w.out.end(), ref.path, ref.path + length);
        static const std::vector<uint8_t> none;
        const auto& state = states ? (*states)[e] : none;
        w.u32(uint32_t(state.size()));
        w.out.insert(w.out.end(), state.begin(), state.end());
        w.u32(uint32_t(ref.id)); w.text(ref.name); // after the state: which plug-in of a bundle, and its name
        w.end(at);
    }
    for (unsigned t = 0; t < track_count; ++t) { // a track's own MIDI channel (parity phase 9)
        if (!project.tracks[t].midi_channel) continue;
        at = w.begin("MIDC");
        w.u8(t); w.u8(project.tracks[t].midi_channel);
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

std::string load_project(const std::vector<uint8_t>& data, Project& target, PatternBank& target_patterns, unsigned* version,
                         std::array<std::vector<uint8_t>, max_plugins>* states) {
    std::array<std::vector<uint8_t>, max_plugins> plugin_states;
    unsigned file_version = 0;
    if (data.size() < 8 || std::memcmp(data.data(), magic, 8)) return "not a Melodee project";
    auto project = std::make_unique<Project>();
    auto patterns = std::make_unique<PatternBank>();
    std::array<bool, track_count> pad4 {};  // tracks whose pads had a PAD4 section
    // Motion before roadmap P1 (MOT2, MOTP, MOTW: the project's, on sixteenths; MOTN: steps of each pattern's division),
    // laid into the patterns at the end, once their lengths and divisions are known.
    struct LegacyMotion { uint8_t track = 0, pattern = 0, param = 0, pad = 0; uint16_t step = 0; int16_t value = 0; bool division = false; };
    std::vector<LegacyMotion> legacy_motion;
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
            tr.mix.sends = {}; tr.mix.sends[tr.mix.bus] = tr.mix.send; // one send, as older files keep it; SNDS has all four
            tr.mix.mute = r.u8() != 0; tr.mix.solo = r.u8() != 0;
            tr.pattern = uint8_t(r.u8() % pattern_count); tr.layout = PadLayout(std::min<unsigned>(r.u8(), unsigned(PadLayout::Count) - 1));
            tr.octave = int8_t(std::clamp(r.i16(), -1, 7)); tr.last_note = uint8_t(r.u8() & 127); // octave: a kit's 16 Levels goes to -1 (model/scales.h)
            unsigned routes = r.u8();
            for (unsigned k = 0; k < routes; ++k) {
                ModRoute m {uint8_t(r.u8()), uint8_t(r.u8()), int8_t(std::clamp(r.i16(), -100, 100))};
                if (k < mod_routes) tr.mods[k] = m;
            }
            read_chain(r, tr.fx);
            tr.bank = uint8_t(r.u8() % sampler_banks); tr.last_pad = uint8_t(r.u8() % sampler_pads);
            tr.levels = r.u8() != 0; tr.levels_pad = uint8_t(r.u8() % sampler_pads); tr.levels_kind = uint8_t(r.u8() % 5); // LevelsKind: Velocity, Scale, Tune, Filter, Loop
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad(r, tr.pads.data(), sampler_pads);
        } else if (name == "PVER") {
            file_version = r.u8();
        } else if (name == "KITX") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad(r, project->tracks[t].pads.data(), sampler_pads);
        } else if (name == "PAD4") {
            unsigned t = r.u8();
            if (t >= track_count) continue;
            pad4[t] = true;
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad4(r, project->tracks[t].pads.data(), sampler_pads);
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
        } else if (name == "PTCH") {
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            uint32_t count = std::min<uint32_t>(r.u32(), max_pattern_events);
            for (uint32_t k = 0; k < count && r.ok; ++k) {
                uint32_t place = r.u32();
                auto pitch = int8_t(uint8_t(r.u8()));
                if (r.ok && place < pattern.events.size()) pattern.events[place].pitch = pitch;
            }
        } else if (name == "RTCH") { // ratchets (roadmap P1); without it every note plays once
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            uint32_t count = std::min<uint32_t>(r.u32(), max_pattern_events);
            for (uint32_t k = 0; k < count && r.ok; ++k) {
                uint32_t place = r.u32();
                unsigned hits = r.u8();
                auto ramp = int8_t(uint8_t(r.u8())), pitch_ramp = int8_t(uint8_t(r.u8()));
                if (!r.ok || place >= pattern.events.size()) continue;
                auto& e = pattern.events[place];
                e.ratchet = uint8_t(std::clamp<unsigned>(hits, 1, max_ratchet));
                e.ramp = int8_t(std::clamp<int>(ramp, -100, 100));
                e.pitch_ramp = int8_t(std::clamp<int>(pitch_ramp, -max_pitch_ramp, max_pitch_ramp));
            }
        } else if (name == "COND") { // trig conditions (roadmap P1); without it every note plays on every pass
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            uint32_t count = std::min<uint32_t>(r.u32(), max_pattern_events);
            for (uint32_t k = 0; k < count && r.ok; ++k) {
                uint32_t place = r.u32();
                unsigned condition = r.u8();
                if (r.ok && place < pattern.events.size() && condition < trig_count) pattern.events[place].condition = uint8_t(condition);
            }
        } else if (name == "TSPD") { // the tracks' playback speeds (roadmap P1); without it every track at x1
            unsigned count = r.u8();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                unsigned speed = r.u8();
                if (k < track_count && r.ok) project->tracks[k].speed = uint8_t(speed < speed_count ? speed : speed_normal);
            }
        } else if (name == "EUCL") { // Euclidean rhythms laid on kit lanes (roadmap P1); their notes are in EVNT either way
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            unsigned count = r.u8();
            pattern.euclids.clear();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                EuclidLane e;
                e.key = uint8_t(r.u8());
                unsigned hits = r.u8(), steps = std::clamp(r.u8(), 1u, 64u), rotate = r.u8();
                e.steps = uint8_t(steps); e.hits = uint8_t(std::min(hits, steps)); e.rotate = uint8_t(rotate % steps);
                if (r.ok) pattern.euclids.push_back(e);
            }
        } else if (name == "FEEL") { // a pattern's feel (roadmap P1); without it its notes play as written
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            Feel f;
            f.strength = uint8_t(std::min(100u, r.u8())); f.swing = uint8_t(std::clamp(r.u8(), 50u, 75u));
            f.humanize = uint8_t(std::min(100u, r.u8())); f.humanize_velocity = uint8_t(std::min(100u, r.u8()));
            f.groove_from = uint16_t(r.u16()); f.groove_amount = uint8_t(std::min(100u, r.u8()));
            f.groove_step = uint16_t(std::clamp(r.u16(), 1u, 4u * ticks_per_quarter));
            unsigned steps = std::min(r.u8(), 64u);
            for (unsigned k = 0; k < steps && r.ok; ++k) {
                f.groove_offsets.push_back(int8_t(uint8_t(r.u8())));
                f.groove_levels.push_back(uint8_t(std::clamp(r.u8(), 25u, 175u)));
            }
            if (r.ok) patterns->at[t][p].feel = f;
        } else if (name == "LANE") { // kit lanes of their own length (roadmap P1); without it every key loops with the pattern
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            unsigned count = r.u8();
            pattern.lanes.clear();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                unsigned key = r.u8();
                uint32_t ticks = r.u32();
                if (r.ok) pattern.set_lane_length(key, ticks); // EVNT came first: the pattern's length is known
            }
        } else if (name == "MOTK") { // Motion (roadmap P1): a pattern's points at their ticks
            unsigned t = r.u8(), p = r.u8();
            if (t >= track_count || p >= pattern_count) continue;
            auto& pattern = patterns->at[t][p];
            uint32_t count = std::min<uint32_t>(r.u32(), motion_points);
            pattern.motion.clear();
            pattern.motion.reserve(count);
            for (uint32_t k = 0; k < count && r.ok; ++k) {
                MotionPoint m;
                m.tick = r.u32(); m.param = uint8_t(r.u8()); m.pad = uint8_t(r.u8() % sampler_pads); m.value = int16_t(r.i16());
                m.hold = uint16_t(r.u16());
                if (r.ok) pattern.motion.push_back(m);
            }
            pattern.sort_motion();
        } else if (name == "MOTN" || name == "MOT2") { // the project's Motion before roadmap P1
            bool division = name == "MOTN";
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                LegacyMotion e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16());
                e.param = uint8_t(r.u8()); e.value = int16_t(r.i16()); e.division = division;
                if (e.param >= motion_pad_base) continue; // pad lanes live in MOTP
                if (r.ok) legacy_motion.push_back(e);
            }
        } else if (name == "MOTP") { // a kit pad's lanes (parity phase 3)
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                LegacyMotion e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16());
                e.pad = uint8_t(r.u8() % sampler_pads);
                unsigned which = r.u8();
                e.value = int16_t(r.i16());
                if (r.ok && which < pad_param_count) { e.param = uint8_t(motion_pad_param(PadParam(which))); legacy_motion.push_back(e); }
            }
        } else if (name == "MOTW") { // the mod wheel's lane (parity phase 8)
            unsigned count = r.u16();
            for (unsigned k = 0; k < count && r.ok; ++k) {
                LegacyMotion e;
                e.track = uint8_t(r.u8() % track_count); e.pattern = uint8_t(r.u8() % pattern_count); e.step = uint16_t(r.u16());
                e.param = uint8_t(motion_wheel); e.value = int16_t(std::clamp<int>(r.i16(), 0, 127));
                if (r.ok) legacy_motion.push_back(e);
            }
        } else if (name == "SNDS") { // a track's four sends (round 2); without it the one TRAK holds
            unsigned t = r.u8(), buses = r.u8();
            std::array<uint8_t, bus_count> sends {};
            for (unsigned b = 0; b < buses && r.ok; ++b) { unsigned v = r.u8(); if (b < bus_count) sends[b] = uint8_t(std::min(100u, v)); }
            if (r.ok && t < track_count) project->tracks[t].mix.sends = sends;
        } else if (name == "LVLE") { // 16 Levels' two ends a type (round 2); without it each type's own
            unsigned t = r.u8(), kinds = r.u8();
            for (unsigned k = 0; k < kinds && r.ok; ++k) {
                int low = int8_t(r.u8()), high = int8_t(r.u8());
                if (!r.ok || t >= track_count || k >= 4) continue;
                auto range = levels_range(LevelsKind(k));
                project->tracks[t].levels_ends[k] = {int8_t(std::clamp(low, range.low, range.high)), int8_t(std::clamp(high, range.low, range.high))};
            }
        } else if (name == "FADR") { // the fader's target (parity phase 8); without it each track takes its engine's default
            unsigned t = r.u8(), target = r.u8(), on = r.u8();
            if (!r.ok || t >= track_count) continue;
            project->tracks[t].fader_target = uint8_t(target < fader_targets ? target : fader_by_engine);
            project->tracks[t].fader_on = on != 0;
            if (r.done()) continue; // older files end here: nothing learned
            unsigned what = r.u8(), lane = r.u16(), engine = r.u8(), pot = r.u8(), pin = r.u8(); // fader C
            if (r.ok && what <= unsigned(FaderLearn::Send)) {
                project->tracks[t].fader_learned = {FaderLearn(what), uint16_t(lane), uint8_t(engine), uint8_t(pot % 3)};
                project->tracks[t].fader_pin = pin != 0;
            }
        } else if (name == "CHAN") { // a pattern chain (seq-sheet2 B); without it a track plays the one pattern
            unsigned t = r.u8(), count = r.u8();
            if (!r.ok || t >= track_count) continue;
            auto& tr = project->tracks[t];
            tr.chain_count = 0; tr.chain_at = 0;
            for (unsigned n = 0; n < count && r.ok; ++n) {
                unsigned p = r.u8();
                if (r.ok && n < max_chain) tr.chain[tr.chain_count++] = uint8_t(p % pattern_count);
            }
            if (tr.chain_count < 2) tr.chain_count = 0;
        } else if (name == "MIDT") { // a MIDI track; TRAK, before it, said None
            unsigned t = r.u8(), version = r.u8();
            if (r.ok && t < track_count && version >= 1) {
                auto& tr = project->tracks[t];
                tr.engine = EngineKind::Midi;
                const auto& info = engine_info(EngineKind::Midi);
                if (version < 2) // before Stage: bank, program and the CCs at their initial values, not the zeros the file holds
                    for (unsigned p = midi_track::routing_params; p < info.param_count; ++p) tr.params[p] = info.params[p].initial;
            }
        } else if (name == "PLGT") { // a PLUGIN track; TRAK, before it, said None
            unsigned t = r.u8(), entry = r.u8();
            if (r.ok && t < track_count) {
                project->tracks[t].engine = EngineKind::Plugin;
                project->tracks[t].plugin = uint8_t(entry < max_plugins ? entry : no_plugin);
            }
        } else if (name == "PLUG") {
            unsigned e = r.u8(), length = r.u16();
            char path[plugin_path_length] {};
            for (unsigned k = 0; k < length; ++k) { char ch = char(r.u8()); if (k + 1 < sizeof(path)) path[k] = ch; }
            uint32_t size = r.u32();
            std::vector<uint8_t> state;
            for (uint32_t k = 0; k < size && r.ok; ++k) state.push_back(uint8_t(r.u8()));
            if (!r.ok || e >= max_plugins) continue;
            std::memcpy(project->plugins[e].path, path, sizeof(path));
            plugin_states[e] = std::move(state);
            if (!r.done()) { project->plugins[e].id = int32_t(r.u32()); r.text(project->plugins[e].name, sizeof(project->plugins[e].name)); }
        } else if (name == "MIDC") { // a track's own MIDI channel; without it none
            unsigned t = r.u8(), channel = r.u8();
            if (r.ok && t < track_count) project->tracks[t].midi_channel = uint8_t(channel <= 16 ? channel : 0);
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
        } else if (name == "LOOP") { // the Looper (docs/LOOPER.md)
            auto& lp = project->looper;
            lp.on = r.u8() != 0;
            lp.quantize = uint8_t(std::min<unsigned>(r.u8(), unsigned(looper::Quantize::Count) - 1));
            lp.then = uint8_t(std::min<unsigned>(r.u8(), unsigned(looper::Then::Count) - 1));
            lp.transport = uint8_t(std::min<unsigned>(r.u8(), unsigned(looper::Transport::Count) - 1));
            unsigned size = r.u8();
            for (unsigned l = 0; l < loop_tracks && r.ok; ++l) {
                auto& t = lp.loops[l];
                unsigned left = size;
                auto byte = [&]() -> unsigned { if (!left) return 0; --left; return r.u8(); };
                auto word = [&]() { unsigned a = byte(); return a | byte() << 8; };
                auto quad = [&]() { uint32_t a = word(); return a | uint32_t(word()) << 16; };
                t.bars = uint8_t(std::min(16u, byte())); t.feedback = uint8_t(std::min(100u, byte()));
                unsigned source = byte();
                t.source = uint8_t(source < looper::source_count && source != looper::source_loop(l) ? source : looper::source_input);
                t.frames = quad(); t.ticks = quad() / 65536.0; t.tempo = uint16_t(word()); t.layers = uint8_t(byte()); t.stopped = byte() != 0;
                while (left) byte(); // what a newer build added
                if (!r.ok || t.ticks <= 0) t.frames = 0;
            }
        } else if (name == "LPMX") {
            for (unsigned l = 0; l < loop_tracks && r.ok; ++l) {
                auto& m = project->loop_mix[l];
                m.level = uint8_t(std::min(127u, r.u8())); m.pan = int8_t(std::clamp(r.i16(), -50, 50));
                m.send = uint8_t(std::min(100u, r.u8())); m.bus = uint8_t(r.u8() % bus_count); m.mute = r.u8() != 0; m.solo = r.u8() != 0;
                read_chain(r, project->loop_fx[l]);
                project->loop_fx[l].size = track_slots;
            }
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
            k.settle(); // before Knob FX C: A and B are the values heard
            if (r.ok && !r.done() && r.u8() >= 1) {
                std::array<int16_t, 3> a {}, b {};
                for (auto& v : a) v = int16_t(r.i16());
                for (auto& v : b) v = int16_t(r.i16());
                unsigned morph = r.u8();
                if (r.ok) { k.a = a; k.b = b; k.morph = uint8_t(std::min(127u, morph)); }
            }
        } else if (name == "PFXP") {
            for (auto& one : project->pad_fx_params) for (auto& v : one) v = int16_t(r.i16());
            project->flex.mode = uint8_t(r.u8() & 1); project->flex.mix = uint8_t(std::min(100u, r.u8())); project->flex.quantize = r.u8() != 0;
        } else if (name == "CMPR") {
            auto& c = project->compressor;
            c.on = r.u8() != 0; c.color = r.u8() != 0;
            c.attack = uint8_t(std::min(100u, r.u8())); c.release = uint8_t(std::min(100u, r.u8()));
            c.amount = uint8_t(std::min(100u, r.u8())); c.boost = uint8_t(std::min(24u, r.u8()));
            c.sidechain = false; c.key = 0;
            if (!r.done()) { c.sidechain = r.u8() != 0; c.key = uint8_t(r.u8() % track_count); }
            c.key_pad = MasterComp::whole_kit;
            if (!r.done()) { unsigned pad = r.u8(); c.key_pad = pad < sampler_pads ? uint8_t(pad) : MasterComp::whole_kit; }
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
    for (const auto& e : legacy_motion) { // into its pattern, at its sixteenth (MOTN's step of Q to the nearest sixteenth)
        auto& pattern = patterns->at[e.track][e.pattern];
        uint32_t sixteenth = e.division ? (uint32_t(e.step) * pattern.q() + sixteenth_ticks / 2) / sixteenth_ticks : e.step;
        MotionPoint m;
        m.tick = sixteenth * sixteenth_ticks; m.param = e.param; m.pad = e.pad; m.value = e.value;
        pattern.write_motion(m);
    }
    target = *project;
    target_patterns = *patterns;
    if (version) *version = file_version;
    if (states) *states = std::move(plugin_states);
    return {};
}

// A kit bank as a file: "MLDBANK1", then sections as a project's. BNAM its
// name, BSMP the files its pads play, BPAD / BPD4 the pads (the records of
// TRAK and PAD4, numbered 0-15 and naming BSMP's entries), BPFX their chains.
namespace {
constexpr char bank_magic[8] {'M', 'L', 'D', 'B', 'A', 'N', 'K', '1'};
}

unsigned BankFile::used() const {
    unsigned n = 0;
    for (const auto& pad : pads) n += pad.used();
    return n;
}

std::vector<uint8_t> save_bank(const BankFile& bank) {
    Writer w;
    w.out.assign(bank_magic, bank_magic + 8);
    size_t at = w.begin("BNAM");
    w.text(bank.name);
    w.end(at);
    at = w.begin("BSMP");
    unsigned files = 0;
    for (const auto& ref : bank.samples) files += ref.used();
    w.u8(files);
    for (unsigned k = 0; k < bank_pads; ++k)
        if (bank.samples[k].used()) { w.u8(k); w.text(bank.samples[k].path); w.text(bank.samples[k].name); w.u32(bank.samples[k].frames); }
    w.end(at);
    at = w.begin("BPAD");
    w.u8(bank.used());
    for (unsigned k = 0; k < bank_pads; ++k) if (bank.pads[k].used()) write_pad(w, k, bank.pads[k]);
    w.end(at);
    at = w.begin("BPD4");
    w.u8(bank.used());
    for (unsigned k = 0; k < bank_pads; ++k) if (bank.pads[k].used()) write_pad4(w, k, bank.pads[k]);
    w.end(at);
    at = w.begin("BPFX");
    for (unsigned k = 0; k < bank_pads; ++k)
        if (bank.chains[k].count()) { w.u8(k); write_chain(w, bank.chains[k]); }
    w.u8(255);
    w.end(at);
    return std::move(w.out);
}

bool load_bank(const std::vector<uint8_t>& data, BankFile& out) {
    if (data.size() < 8 || std::memcmp(data.data(), bank_magic, 8)) return false;
    BankFile bank;
    Reader file(data.data() + 8, data.size() - 8);
    while (!file.done() && file.ok) {
        char tag[5];
        Reader r = file.section(tag);
        if (!file.ok) return false;
        std::string name(tag);
        if (name == "BNAM") {
            r.text(bank.name, sizeof(bank.name));
        } else if (name == "BSMP") {
            unsigned files = r.u8();
            for (unsigned k = 0; k < files && r.ok; ++k) {
                auto& ref = bank.samples[r.u8() % bank_pads];
                r.text(ref.path, sizeof(ref.path)); r.text(ref.name, sizeof(ref.name)); ref.frames = r.u32();
            }
        } else if (name == "BPAD") {
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad(r, bank.pads.data(), bank_pads, bank_pads);
        } else if (name == "BPD4") {
            unsigned used = r.u8();
            for (unsigned k = 0; k < used && r.ok; ++k) read_pad4(r, bank.pads.data(), bank_pads);
        } else if (name == "BPFX") {
            while (r.ok && !r.done()) {
                unsigned k = r.u8();
                if (k >= bank_pads) break;
                auto& chain = bank.chains[k];
                read_chain(r, chain);
                for (unsigned n = pad_slots; n < max_slots; ++n) chain.slots[n] = EffectSlot {};
                chain.size = pad_slots;
            }
        }
        if (!r.ok) return false;
    }
    for (auto& pad : bank.pads) // a pad whose file is not listed plays nothing
        if (pad.used() && !bank.samples[pad.sample % bank_pads].used()) pad = PadSound {};
    out = bank;
    return true;
}

BankFile bank_of(const Project& project, unsigned track, unsigned bank, const char* name) {
    BankFile out;
    const auto& tr = project.tracks[track % track_count];
    std::snprintf(out.name, sizeof(out.name), "%s", name);
    std::array<int, sample_slots> mapped;
    mapped.fill(-1);
    unsigned files = 0;
    for (unsigned k = 0; k < bank_pads; ++k) {
        unsigned n = (bank % sampler_banks) * bank_pads + k;
        PadSound pad = tr.pads[n];
        if (pad.used()) {
            unsigned s = pad.sample % sample_slots;
            if (!project.samples[s].used()) pad = PadSound {};
            else {
                if (mapped[s] < 0) { mapped[s] = int(files); out.samples[files++] = project.samples[s]; }
                pad.sample = uint16_t(mapped[s]);
            }
        }
        out.pads[k] = pad;
        if (pad.used()) out.chains[k] = tr.pad_fx.at[n];
    }
    return out;
}

} // namespace melodee
