// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SoundFont 2 as the format's own specification (2.04) lays it out: a RIFF
// "sfbk" with its sounds in sdta (smpl, and sm24's low bytes) and its lists in
// pdta (phdr, pbag, pmod, pgen for presets; inst, ibag, imod, igen for
// instruments; shdr for sounds). A preset's zones name instruments, an
// instrument's zones name sounds; each level's first zone with nothing named
// is what the rest start from; a preset's numbers add to its instrument's.
#include "melodee/engine/sf2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace melodee {
namespace {
constexpr size_t phdr_size = 38, bag_size = 4, mod_size = 10, gen_size = 4, inst_size = 22, shdr_size = 46;
namespace gen {
constexpr int start_offset = 0, end_offset = 1, loop_start_offset = 2, loop_end_offset = 3, start_coarse = 4, mod_lfo_pitch = 5,
              vib_lfo_pitch = 6, mod_env_pitch = 7, filter_cutoff = 8, filter_q = 9, mod_lfo_filter = 10, mod_env_filter = 11,
              end_coarse = 12, mod_lfo_volume = 13, pan = 17, delay_vol = 33, attack_vol = 34, hold_vol = 35, decay_vol = 36,
              sustain_vol = 37, release_vol = 38, key_hold = 39, key_decay = 40, instrument = 41, key_range = 43,
              velocity_range = 44, loop_start_coarse = 45, attenuation = 48, loop_end_coarse = 50, coarse_tune = 51, fine_tune = 52,
              sample_id = 53, sample_modes = 54, scale_tuning = 56, exclusive_class = 57, root_key = 58;
constexpr int no_time = -12000; // a time left unsaid: as good as none
}
using Gens = std::map<int, int>;
constexpr double longest_stage = 10.0; // seconds: a fall this long is the sound's own to make

struct Likeness { // what makes two sounds of a pair the same but for their sound and pan
    int low_key, high_key, low_velocity, high_velocity, root, tune, keytrack, mode, loop_from, loop_to, length, rate, group;
    double gain, attack, decay, sustain, release, cutoff, resonance;
    auto tied() const {
        return std::tie(low_key, high_key, low_velocity, high_velocity, root, tune, keytrack, mode, loop_from, loop_to, length, rate, group, gain,
                        attack, decay, sustain, release, cutoff, resonance);
    }
    bool operator<(const Likeness& o) const { return tied() < o.tied(); }
};
struct Voice {
    int sound = 0;
    unsigned link = 0;
    std::string name;
    double pan = 0;
    int offset = 0, end = 0;
    Likeness like {};
};
} // namespace

bool is_sf2(const std::string& path) {
    if (path.size() < 4) return false;
    std::string end = path.substr(path.size() - 4);
    std::transform(end.begin(), end.end(), end.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return end == ".sf2";
}

bool split_sf2_preset(const std::string& path, std::string& file, unsigned& preset) {
    auto mark = path.rfind('#');
    if (mark == std::string::npos || mark + 1 >= path.size() || !is_sf2(path.substr(0, mark))) return false;
    char* end = nullptr;
    unsigned long n = std::strtoul(path.c_str() + mark + 1, &end, 10);
    if (*end || n > 65535) return false;
    file = path.substr(0, mark);
    preset = unsigned(n);
    return true;
}

std::string Sf2::text(size_t at, size_t count) const {
    std::string out;
    for (size_t n = 0; n < count && data[at + n]; ++n) out += char(data[at + n] < 0x80 ? data[at + n] : '?');
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

std::string Sf2::open(const uint8_t* bytes, size_t bytes_size) {
    data = bytes; size = bytes_size; list.clear();
    if (size < 12 || std::memcmp(data, "RIFF", 4) || std::memcmp(data + 8, "sfbk", 4)) return "Not an SF2";
    std::map<std::string, Range> found;
    unsigned major = 2;
    for (size_t at = 12; at + 8 <= size;) { // the LISTs and what each holds
        uint32_t length = u32(at + 4);
        size_t from = at + 8, to = std::min(size, from + size_t(length));
        if (!std::memcmp(data + at, "LIST", 4) && to - from >= 4) {
            const bool info = !std::memcmp(data + from, "INFO", 4);
            for (size_t in = from + 4; in + 8 <= to;) {
                uint32_t inner = u32(in + 4);
                size_t body = in + 8, body_end = std::min(to, body + size_t(inner));
                std::string id(reinterpret_cast<const char*>(data + in), 4);
                if (info && id == "ifil" && body_end - body >= 2) major = u16(body);
                if (!info) found[id] = Range {body, body_end - body};
                in = body + inner + inner % 2;
            }
        }
        at = from + length + length % 2;
    }
    if (major >= 3) return "Packed sounds (SF3)";
    auto take = [&](const char* id, size_t unit, Range& into) {
        auto it = found.find(id);
        if (it == found.end() || it->second.size < unit || it->second.size % unit) return false;
        into = it->second;
        return true;
    };
    auto smpl = found.find("smpl");
    if (smpl == found.end()) return "Not an SF2";
    sounds = smpl->second;
    auto sm24 = found.find("sm24"); // a byte a frame, if it is of use; a file saying otherwise is read in 16 bits
    low = sm24 != found.end() && sm24->second.size >= sounds.size / 2 ? sm24->second : Range {};
    if (!take("phdr", phdr_size, phdr) || !take("pbag", bag_size, pbag) || !take("pmod", mod_size, pmod) || !take("pgen", gen_size, pgen)
        || !take("inst", inst_size, inst) || !take("ibag", bag_size, ibag) || !take("imod", mod_size, imod) || !take("igen", gen_size, igen)
        || !take("shdr", shdr_size, shdr))
        return "Not an SF2";
    for (size_t at = shdr.from; at + 2 * shdr_size <= shdr.from + shdr.size; at += shdr_size) {
        unsigned type = u16(at + 44);
        if ((type & 0x10) && !(type & 0x8000)) return "Packed sounds (SF3)";
    }
    const size_t count = phdr.size / phdr_size;
    for (size_t n = 0; n + 1 < count; ++n) { // the last is the list's end
        size_t at = phdr.from + n * phdr_size;
        Sf2Preset p;
        p.index = unsigned(n); p.program = int(u16(at + 20)); p.bank = int(u16(at + 22));
        p.name = text(at, 20);
        if (p.name.empty()) p.name = "Preset " + std::to_string(n + 1);
        list.push_back(p);
    }
    std::stable_sort(list.begin(), list.end(), [](const Sf2Preset& a, const Sf2Preset& b) {
        return std::tie(a.bank, a.program, a.index) < std::tie(b.bank, b.program, b.index);
    });
    return {};
}

bool Sf2::header(int index, Header& out) const {
    if (index < 0 || size_t(index) + 1 >= shdr.size / shdr_size) return false;
    size_t at = shdr.from + size_t(index) * shdr_size;
    out.name = text(at, 20);
    out.start = u32(at + 20); out.end = u32(at + 24); out.loop_start = u32(at + 28); out.loop_end = u32(at + 32); out.rate = u32(at + 36);
    out.pitch = data[at + 40]; out.correction = int8_t(data[at + 41]); out.link = u16(at + 42); out.type = u16(at + 44);
    // in ROM, or past what the file holds: nothing to play
    return !(out.type & 0x8000) && out.end > out.start && size_t(out.end) * 2 <= sounds.size && out.rate > 0;
}

namespace {
struct Bag { size_t gens_from = 0, gens_to = 0, mods_from = 0, mods_to = 0; };
} // namespace

bool Sf2::plays(const Sf2Preset& preset) const {
    const size_t presets = phdr.size / phdr_size;
    if (preset.index + 1 >= presets) return false;
    auto bag_of = [&](const Range& bags, size_t n, Bag& out) {
        if ((n + 2) * bag_size > bags.size) return false;
        size_t at = bags.from + n * bag_size;
        out.gens_from = u16(at); out.gens_to = std::max<size_t>(out.gens_from, u16(at + bag_size));
        return true;
    };
    auto find = [&](const Range& gens, const Bag& b, int which, int& value) {
        for (size_t n = b.gens_from; n < b.gens_to && (n + 1) * gen_size <= gens.size; ++n)
            if (int(u16(gens.from + n * gen_size)) == which) { value = s16(gens.from + n * gen_size + 2); return true; }
        return false;
    };
    size_t at = phdr.from + preset.index * phdr_size;
    size_t first = u16(at + 24), last = std::max<size_t>(first, u16(at + phdr_size + 24));
    for (size_t pb = first; pb < last; ++pb) {
        Bag outer;
        int instrument = -1;
        if (!bag_of(pbag, pb, outer) || !find(pgen, outer, gen::instrument, instrument) || instrument < 0
            || size_t(instrument + 1) * inst_size >= inst.size)
            continue;
        size_t its = inst.from + size_t(instrument) * inst_size;
        size_t ifirst = u16(its + 20), ilast = std::max<size_t>(ifirst, u16(its + inst_size + 20));
        for (size_t ib = ifirst; ib < ilast; ++ib) {
            Bag inner;
            int sound = -1;
            Header h;
            if (bag_of(ibag, ib, inner) && find(igen, inner, gen::sample_id, sound) && header(sound, h)) return true;
        }
    }
    return false;
}

SfzRead Sf2::read(const Sf2Preset& preset) const {
    SfzRead read;
    const size_t presets = phdr.size / phdr_size;
    if (preset.index + 1 >= presets) return read;
    auto bag_of = [&](const Range& bags, size_t n, Bag& out) {
        if ((n + 2) * bag_size > bags.size) return false;
        size_t at = bags.from + n * bag_size;
        out.gens_from = u16(at); out.mods_from = u16(at + 2);
        out.gens_to = std::max<size_t>(out.gens_from, u16(at + bag_size));
        out.mods_to = std::max<size_t>(out.mods_from, u16(at + bag_size + 2));
        return true;
    };
    auto gens_of = [&](const Range& gens, const Bag& b) {
        Gens out;
        for (size_t n = b.gens_from; n < b.gens_to && (n + 1) * gen_size <= gens.size; ++n)
            out[int(u16(gens.from + n * gen_size))] = s16(gens.from + n * gen_size + 2);
        return out;
    };
    auto merged = [](Gens under, const Gens& over) { for (const auto& [k, v] : over) under[k] = v; return under; };
    // Keys or velocities of two levels: where both reach; false where they meet nowhere.
    auto meet = [](const Gens& a, const Gens& b, int which, int& low, int& high) {
        auto range = [&](const Gens& g, int& lo, int& hi) {
            auto it = g.find(which);
            if (it == g.end()) { lo = 0; hi = 127; return; }
            lo = it->second & 0xff; hi = (it->second >> 8) & 0xff;
        };
        int a0, a1, b0, b1;
        range(a, a0, a1); range(b, b0, b1);
        low = std::max(a0, b0); high = std::min(a1, b1);
        return low <= high;
    };
    std::vector<Voice> voices;
    size_t at = phdr.from + preset.index * phdr_size;
    size_t first = u16(at + 24), last = std::max<size_t>(first, u16(at + phdr_size + 24));
    Gens preset_global;
    for (size_t pb = first; pb < last; ++pb) {
        Bag outer;
        if (!bag_of(pbag, pb, outer)) continue;
        Gens own = gens_of(pgen, outer);
        if (outer.mods_to > outer.mods_from) read.ignored.insert("modulators");
        if (!own.count(gen::instrument)) { if (pb == first) preset_global = own; continue; } // the zones' starting point
        Gens outer_gens = merged(preset_global, own);
        int instrument = outer_gens[gen::instrument];
        if (instrument < 0 || size_t(instrument + 1) * inst_size >= inst.size) continue;
        size_t its = inst.from + size_t(instrument) * inst_size;
        size_t ifirst = u16(its + 20), ilast = std::max<size_t>(ifirst, u16(its + inst_size + 20));
        Gens inner_global;
        for (size_t ib = ifirst; ib < ilast; ++ib) {
            Bag inner;
            if (!bag_of(ibag, ib, inner)) continue;
            Gens its_gens = gens_of(igen, inner);
            if (inner.mods_to > inner.mods_from) read.ignored.insert("modulators");
            if (!its_gens.count(gen::sample_id)) { if (ib == ifirst) inner_global = its_gens; continue; }
            Gens ig = merged(inner_global, its_gens);
            // One sounding: the instrument's numbers, the preset's added to all but what only the sound's zone says.
            Header h;
            int lk, hk, lv, hv;
            if (!header(ig[gen::sample_id], h) || !meet(outer_gens, ig, gen::key_range, lk, hk) || !meet(outer_gens, ig, gen::velocity_range, lv, hv)) {
                ++read.left;
                continue;
            }
            auto both = [&](int g, int plain = 0) {
                auto i = ig.find(g), p = outer_gens.find(g);
                return (i != ig.end() ? i->second : plain) + (p != outer_gens.end() ? p->second : 0);
            };
            auto own_of = [&](int g, int plain = 0) { auto i = ig.find(g); return i != ig.end() ? i->second : plain; };
            auto seconds = [](int cents) { return std::pow(2.0, std::clamp(cents, gen::no_time, 8000) / 1200.0); };
            for (int g : {gen::mod_lfo_pitch, gen::mod_lfo_filter, gen::mod_lfo_volume}) if (both(g)) read.ignored.insert("mod LFO");
            if (both(gen::vib_lfo_pitch)) read.ignored.insert("vibrato LFO");
            for (int g : {gen::mod_env_pitch, gen::mod_env_filter}) if (both(g)) read.ignored.insert("mod envelope");
            if (both(gen::delay_vol, gen::no_time) > -7000) read.ignored.insert("envelope delay");
            const int middle = (lk + hk) / 2;
            const double hold = seconds(both(gen::hold_vol, gen::no_time) + both(gen::key_hold) * (60 - middle));
            const double decay = seconds(both(gen::decay_vol, gen::no_time) + both(gen::key_decay) * (60 - middle));
            // Sustain in tenths of a dB down; the envelope falls in dB, its decay time the whole 96 dB: the time of the
            // first 40 dB (or the whole fall where less) is the sampler's curve's.
            const double fall = std::min(std::max(double(both(gen::sustain_vol)), 0.0) / 10, 96.0);
            double attack = seconds(both(gen::attack_vol, gen::no_time));
            if (attack < 0.002) attack = 0;
            double falling = hold + decay * std::min(fall, 40.0) / 96;
            if (hold + decay < 0.002) falling = 0;
            double sustain = fall >= 96 ? 0 : 100 * std::pow(10.0, -fall / 20);
            if (falling >= longest_stage) sustain = 100;
            const double release = std::max(seconds(both(gen::release_vol, gen::no_time)) * 40 / 96, 0.005); // never stopped dead
            const double hz = 8.176 * std::pow(2.0, both(gen::filter_cutoff, 13500) / 1200.0);
            const bool cuts = hz < 18000;
            const int mode = own_of(gen::sample_modes) & 3;
            const bool looping = mode == 1 || mode == 3;
            const int root_key = own_of(gen::root_key, -1);
            const int length = int(h.end - h.start);
            Voice v;
            v.sound = ig[gen::sample_id]; v.link = h.link; v.name = h.name;
            v.pan = std::clamp(both(gen::pan) / 500.0, -1.0, 1.0);
            v.offset = own_of(gen::start_offset) + own_of(gen::start_coarse) * 32768;
            v.end = length + own_of(gen::end_offset) + own_of(gen::end_coarse) * 32768;
            auto& l = v.like;
            l.low_key = lk; l.high_key = hk; l.low_velocity = std::max(lv, 1); l.high_velocity = std::max(hv, 1);
            l.root = root_key >= 0 && root_key <= 127 ? root_key : h.pitch <= 127 ? h.pitch : 60;
            l.tune = both(gen::coarse_tune) * 100 + both(gen::fine_tune) + h.correction;
            l.keytrack = both(gen::scale_tuning, 100);
            // the hardware the format was made for takes 0.4 of what a file says here, and files are voiced by ear to that
            l.gain = -0.04 * std::max(double(both(gen::attenuation)), 0.0);
            l.mode = looping ? mode : 0;
            l.loop_from = looping ? int(h.loop_start - h.start) + own_of(gen::loop_start_offset) + own_of(gen::loop_start_coarse) * 32768 : 0;
            l.loop_to = looping ? int(h.loop_end - h.start) + own_of(gen::loop_end_offset) + own_of(gen::loop_end_coarse) * 32768 : 0;
            l.length = length; l.rate = int(h.rate); l.group = std::max(own_of(gen::exclusive_class), 0);
            l.attack = attack; l.decay = falling; l.sustain = sustain; l.release = release;
            l.cutoff = cuts ? hz : 0; l.resonance = cuts ? std::clamp(both(gen::filter_q) / 10.0, 0.0, 40.0) : 0;
            voices.push_back(std::move(v));
        }
    }
    // Pairs: a sound hard left and one hard right, alike in all else, are one stereo sound.
    std::map<Likeness, std::vector<size_t>> alike;
    for (size_t n = 0; n < voices.size(); ++n) alike[voices[n].like].push_back(n);
    std::vector<int> partner(voices.size(), -1);
    std::vector<bool> taken(voices.size(), false);
    for (const auto& [like, members] : alike) {
        (void)like;
        std::vector<size_t> rights;
        for (size_t n : members) if (voices[n].pan >= 0.9) rights.push_back(n);
        for (size_t n : members) {
            if (voices[n].pan > -0.9 || rights.empty()) continue;
            auto linked = std::find_if(rights.begin(), rights.end(), [&](size_t r) { return unsigned(voices[r].sound) == voices[n].link; });
            auto chosen = linked != rights.end() ? linked : rights.begin();
            partner[n] = int(*chosen);
            taken[*chosen] = true;
            rights.erase(chosen);
        }
    }
    std::vector<SfzEnvelope> envelopes;
    for (size_t n = 0; n < voices.size(); ++n) {
        if (taken[n]) continue;
        if (read.regions.size() >= max_zones) { ++read.left; continue; }
        const auto& v = voices[n];
        const auto& l = v.like;
        SfzRegion r;
        r.sample = std::to_string(v.sound) + (partner[n] >= 0 ? "+" + std::to_string(voices[size_t(partner[n])].sound) : std::string());
        auto& z = r.zone;
        z.low_key = uint8_t(l.low_key); z.high_key = uint8_t(l.high_key); z.root = uint8_t(l.root);
        z.low_velocity = uint8_t(std::min(l.low_velocity, 127)); z.high_velocity = uint8_t(std::min(l.high_velocity, 127));
        z.tune = int16_t(std::clamp(l.tune, -32000, 32000));
        z.keytrack = int16_t(std::clamp(l.keytrack, -32000, 32000));
        z.gain = float(l.gain);
        z.pan = partner[n] >= 0 ? 0.0f : float(v.pan);
        z.loop = l.mode == 1 ? ZoneLoop::On : l.mode == 3 ? ZoneLoop::UntilReleased : ZoneLoop::Off;
        z.loop_start = uint32_t(std::max(l.loop_from, 0));
        z.loop_end = uint32_t(std::max(l.loop_to, 0));
        z.group = z.off_by = uint32_t(l.group);
        if (l.cutoff > 0) { z.filter = PadFilter::Lpf2; z.cutoff = float(std::clamp(l.cutoff, 20.0, 20000.0)); z.resonance = float(l.resonance); }
        if (v.offset) r.offset = v.offset;
        if (v.end != l.length) r.end = std::max(v.end, 1);
        r.loop_asked = z.loop != ZoneLoop::Off;
        r.loop_named = true;
        read.regions.push_back(std::move(r));
        envelopes.push_back(SfzEnvelope {l.attack, l.decay, l.sustain, l.release});
    }
    sfz_share_envelopes(envelopes, read);
    return read;
}

bool Sf2::sound(const std::string& name, std::vector<float>& left, std::vector<float>& right, unsigned& rate, int64_t& loop_start,
                int64_t& loop_end) const {
    char* end = nullptr;
    long a = std::strtol(name.c_str(), &end, 10);
    long b = -1;
    if (*end == '+') b = std::strtol(end + 1, &end, 10);
    if (*end) return false;
    Header first, second;
    if (!header(int(a), first) || (b >= 0 && !header(int(b), second))) return false;
    const uint32_t frames = first.end - first.start;
    auto side = [&](const Header& h, std::vector<float>& out) {
        out.resize(frames);
        const uint32_t have = std::min(frames, h.end - h.start);
        for (uint32_t n = 0; n < frames; ++n) {
            if (n >= have) { out[n] = 0; continue; }
            const size_t at = size_t(h.start) + n;
            int32_t v = int32_t(int16_t(uint16_t(u16(sounds.from + at * 2)))) * 256;
            if (low.size) v |= data[low.from + at];
            out[n] = float(v) / 8388608.0f;
        }
    };
    side(first, left);
    if (b >= 0) side(second, right); else right.clear();
    rate = first.rate;
    const bool looped = first.loop_end > first.loop_start && first.loop_start >= first.start && first.loop_end <= first.end;
    loop_start = looped ? int64_t(first.loop_start - first.start) : -1;
    loop_end = looped ? int64_t(first.loop_end - first.start) : -1;
    return true;
}

} // namespace melodee
