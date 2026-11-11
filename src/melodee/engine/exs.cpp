// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// EXS24 instruments as zones. Every block has an 84-byte head: its byte order
// in its first byte (0 big-endian, 1 little), its kind in the low nibble of
// its fourth, its content's length at 4, "TBOS" ("SOBT" big-endian) at 16 and
// its name at 20. The offsets below are into a block's content, as Logic
// writes them (ConvertWithMoss documents the same); the TX instruments AudioKit's
// ROMPlayer shipped both as .exs and, converted, as .sfz agree with this read
// zone for zone. A zone's sample end and loop end are one past its last frame.
#include "melodee/engine/exs.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace melodee {
namespace {
constexpr size_t head = 84;
constexpr uint32_t to_its_end = 0x7fffffff; // a loop's end: the file's last frame (the loader clamps it there)
enum Kind { zone_block = 1, group_block = 2, sample_block = 3, parameter_block = 4 };

// The parameters read (their ids in the parameter block).
constexpr int master_volume = 0x07, coarse_tune = 0x0e, fine_tune = 0x0f, filter_on = 0x2c, transpose = 0x2d,
              env1_sustain = 0x51, env1_attack = 0x52, env1_decay = 0x54, env1_release = 0x55, env1_hold = 0x58,
              level_via_velocity = 0x5a, random_select = 0xa3;

uint32_t word(const uint8_t* p, bool big) {
    return big ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]
               : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

// A block's content, read past its end as `otherwise`: older files' blocks are shorter.
struct Content {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    bool big = false;
    bool has(size_t at, size_t length) const { return at + length <= size; }
    int u8(size_t at, int otherwise = 0) const { return has(at, 1) ? bytes[at] : otherwise; }
    int s8(size_t at) const { return has(at, 1) ? int(int8_t(bytes[at])) : 0; }
    uint32_t u32(size_t at, uint32_t otherwise = 0) const { return has(at, 4) ? word(bytes + at, big) : otherwise; }
    int32_t s32(size_t at, int32_t otherwise) const { return int32_t(u32(at, uint32_t(otherwise))); }
    unsigned u16(size_t at) const {
        if (!has(at, 2)) return 0;
        return big ? unsigned(bytes[at]) << 8 | bytes[at + 1] : unsigned(bytes[at + 1]) << 8 | bytes[at];
    }
    std::string text(size_t at, size_t length) const {
        if (!has(at, 1)) return {};
        const char* from = reinterpret_cast<const char*>(bytes + at);
        return std::string(from, strnlen(from, std::min(length, size - at)));
    }
};

struct ExsZone {
    int options = 0, key = 60, fine = 0, pan = 0, volume = 0, low_key = 0, high_key = 127, low_velocity = 0, high_velocity = 127;
    uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0, crossfade = 0;
    int loop_options = 0, loop_direction = 0, coarse = 0;
    int32_t group = -1, sample = -1;
};

// How a group is picked, past every press: by a key held (a keyswitch), by round robin, by a controller...
enum Select { always = 0, by_note = 1, round_robin = 2, by_control = 3, by_bend = 4, by_channel = 5, by_articulation = 6, by_tempo = 7 };

struct ExsGroup {
    int volume = 0, pan = 0, options = 0, exclusive = 0, low_velocity = 0, high_velocity = 127, random = 0;
    bool release_trigger = false, envelope_offsets = false;
    int note = 0, select = always, control_low = 0, low_key = 0, high_key = 127, channel = 0, articulation = 0, tempo_low = 0;
    int32_t after = -2; // round robin: the group it follows, -1 first, below that none
    bool muted() const { return options & 16; }
};

struct ExsSample {
    std::string name, folder;
};

ExsZone zone_of(const Content& c) {
    ExsZone z;
    z.options = c.u8(0);
    z.key = c.u8(1, 60);
    z.fine = c.s8(2);
    z.pan = c.s8(3);
    z.volume = c.s8(4);
    z.low_key = c.u8(6);
    z.high_key = c.u8(7, 127);
    z.low_velocity = c.u8(9);
    z.high_velocity = c.u8(10, 127);
    z.start = c.u32(12);
    z.end = c.u32(16);
    z.loop_start = c.u32(20);
    z.loop_end = c.u32(24);
    z.crossfade = c.u32(28);
    z.loop_options = c.u8(33);
    z.loop_direction = c.u8(34);
    z.coarse = c.s8(80);
    z.group = c.s32(88, -1);
    z.sample = c.s32(92, -1);
    return z;
}

ExsGroup group_of(const Content& c) {
    ExsGroup g;
    g.volume = c.s8(0);
    g.pan = c.s8(1);
    g.options = c.u8(3);
    g.exclusive = c.u8(4);
    g.low_velocity = c.u8(5);
    g.high_velocity = c.u8(6, 127);
    g.random = c.u8(7);
    g.tempo_low = c.u8(38);
    for (size_t at : {56, 60, 64, 68}) g.envelope_offsets = g.envelope_offsets || c.u32(at);
    g.release_trigger = c.u8(73) > 0;
    g.note = c.u8(75);
    if (c.size > 76) { // Logic 8 on: round robin, the other ways to pick a group, and its own key range
        g.after = c.s32(80, -2);
        g.select = c.u8(84);
        g.control_low = c.u8(86);
        g.low_key = c.u8(88);
        g.high_key = c.u8(89, 127);
        g.channel = c.u8(90);
        g.articulation = c.u8(91);
    }
    return g;
}

ExsSample sample_of(const Content& c, const std::string& block_name) {
    ExsSample s;
    s.folder = c.text(80, 256);
    s.name = c.text(336, 256); // older files have only the block's name
    if (s.name.empty()) s.name = block_name;
    return s;
}

// 100 one-byte ids then their 16-bit values; Logic 8 on, a count and 16-bit id and value pairs.
void parameters_of(const Content& c, std::map<int, int>& values) {
    const uint32_t count = c.u32(0);
    if (!c.has(4, size_t(count) * 3)) return;
    for (uint32_t n = 0; n < count; ++n)
        if (int id = c.u8(4 + n)) values[id] = int16_t(c.u16(4 + count + 2 * n));
    const size_t more = 4 + size_t(count) * 3;
    const uint32_t pairs = c.u32(more);
    for (uint32_t n = 0; n < pairs && c.has(more + 4 + 4 * size_t(n), 4); ++n)
        if (unsigned id = c.u16(more + 4 + 4 * n)) values[int(id)] = int(c.u16(more + 6 + 4 * n));
}

bool magic_at(const uint8_t* block, bool big) {
    const char* m = reinterpret_cast<const char*>(block + 16);
    return big ? !std::memcmp(m, "SOBT", 4) || !std::memcmp(m, "SOBJ", 4) : !std::memcmp(m, "TBOS", 4) || !std::memcmp(m, "JBOS", 4);
}

// Where a round robin group falls in its chain (1 up), and the chain's first group.
std::pair<int, int> chain_place(const std::vector<ExsGroup>& groups, int g) {
    int place = 1, first = g;
    for (size_t steps = 0; steps < groups.size(); ++steps) {
        const int after = groups[size_t(first)].after;
        if (after < 0 || size_t(after) >= groups.size() || after == first || groups[size_t(after)].select != round_robin) break;
        first = after;
        ++place;
    }
    return {place, first};
}

const char* select_name(int select) {
    switch (select) {
    case by_note: return "keyswitch";
    case by_control: return "controller select";
    case by_bend: return "bend select";
    case by_channel: return "channel select";
    case by_articulation: return "articulation select";
    case by_tempo: return "tempo select";
    default: return "group select";
    }
}
int select_value(const ExsGroup& g) {
    switch (g.select) {
    case by_note: return g.note;
    case by_control: return g.control_low;
    case by_channel: return g.channel;
    case by_articulation: return g.articulation;
    case by_tempo: return g.tempo_low;
    default: return 0;
    }
}

template <typename T> T clamped(long value, long low, long high) { return T(std::clamp(value, low, high)); }
} // namespace

bool exs_magic(const std::string& bytes) {
    if (bytes.size() < head) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());
    return magic_at(b, b[0] == 0);
}

double exs_seconds(int value) {
    const double x = std::clamp(value, 0, 127) / 127.0;
    return 10.0 * x * x * x * x;
}

SfzRead read_exs(const std::string& bytes, const ExsFind& find, std::string* error) {
    SfzRead read;
    if (!exs_magic(bytes)) {
        if (error) *error = "Not an EXS instrument";
        return read;
    }
    std::vector<ExsZone> zones;
    std::vector<ExsGroup> groups;
    std::vector<ExsSample> samples;
    std::map<int, int> parameters;
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    for (size_t at = 0; at + head <= bytes.size();) {
        const uint8_t* block = data + at;
        const bool big = block[0] == 0;
        if (!magic_at(block, big)) break;
        uint32_t size = word(block + 4, big);
        if (block[3] & 0x80) size &= 0x7fff;
        if (size > bytes.size() - at - head) break; // cut short: what came whole is kept
        Content c {block + head, size, big};
        const std::string name(reinterpret_cast<const char*>(block + 20), strnlen(reinterpret_cast<const char*>(block + 20), 64));
        switch (block[3] & 0x0f) {
        case zone_block: zones.push_back(zone_of(c)); break;
        case group_block: groups.push_back(group_of(c)); break; // a zone names its group by this order
        case sample_block: samples.push_back(sample_of(c, name)); break;
        case parameter_block: parameters_of(c, parameters); break;
        default: break; // the instrument's own block, and Logic's newer ones: nothing played
        }
        at += head + size;
    }
    auto parameter = [&](int id) -> std::optional<int> {
        auto found = parameters.find(id);
        if (found == parameters.end()) return std::nullopt;
        return found->second;
    };

    // The instrument's own tune and level, over every zone's: Logic's Volume is
    // in dB (its new instruments' -6 dB of headroom included, as Logic plays them).
    const double cents = 100.0 * (parameter(transpose).value_or(0) + parameter(coarse_tune).value_or(0)) + parameter(fine_tune).value_or(0);
    const double master_db = parameter(master_volume).value_or(0);
    if (parameter(filter_on).value_or(0) > 0) read.ignored.insert("filter");
    if (parameter(random_select).value_or(0) > 0) read.ignored.insert("random sample select");
    // Level via Vel: its low end's dB under full, -60 the whole way (an SFZ's amp_veltrack 100).
    if (auto via = parameter(level_via_velocity)) read.velocity = std::clamp(*via / -60.0, 0.0, 1.0) * 100;
    // ENV1 is the amp's; a hold sits on its decay.
    SfzEnvelope envelope;
    if (auto v = parameter(env1_attack)) envelope.attack = exs_seconds(*v);
    if (auto v = parameter(env1_decay)) envelope.decay = exs_seconds(*v);
    if (auto v = parameter(env1_hold); v && *v > 0) envelope.decay = envelope.decay.value_or(0) + exs_seconds(*v);
    if (auto v = parameter(env1_sustain)) envelope.sustain = std::clamp(*v, 0, 127) / 127.0 * 100;
    if (auto v = parameter(env1_release)) envelope.release = exs_seconds(*v);

    // Groups picked by something held at the time (a keyswitch, a controller, a channel...):
    // the sampler has nothing held, so of each kind it keeps those at rest, the lowest value's.
    std::map<int, int> at_rest;
    for (const auto& g : groups)
        if (g.select != always && g.select != round_robin && !g.muted() && !g.release_trigger) {
            auto [it, made] = at_rest.emplace(g.select, select_value(g));
            if (!made) it->second = std::min(it->second, select_value(g));
        }
    // Round robin chains: each group's place and how long its chain is.
    std::vector<std::pair<int, int>> place(groups.size(), {0, -1});
    std::map<int, int> chain_length;
    for (size_t g = 0; g < groups.size(); ++g)
        if (groups[g].select == round_robin && groups[g].after >= -1) {
            place[g] = chain_place(groups, int(g));
            auto& length = chain_length[place[g].second];
            length = std::max(length, place[g].first);
        }

    std::vector<std::optional<std::string>> found(samples.size());
    std::vector<SfzEnvelope> envelopes;
    auto leave = [&](const char* why) {
        ++read.left;
        if (why) read.left_by.insert(why);
    };
    for (const auto& z : zones) {
        if (z.sample < 0 || size_t(z.sample) >= samples.size()) { leave(nullptr); continue; }
        const ExsGroup* g = z.group >= 0 && size_t(z.group) < groups.size() ? &groups[size_t(z.group)] : nullptr;
        if (g && g->muted()) { leave("muted group"); continue; }
        if (g && g->release_trigger) { leave("release trigger"); continue; }
        if (g && g->select != always && g->select != round_robin && select_value(*g) != at_rest[g->select]) { leave(select_name(g->select)); continue; }
        if (read.regions.size() >= max_zones) { leave(nullptr); continue; }
        int low_key = z.low_key, high_key = z.high_key;
        // velocity 0 is a release: the lowest that plays is 1
        int low_velocity = z.options & 8 ? z.low_velocity : 0, high_velocity = z.options & 8 ? z.high_velocity : 127;
        if (g) { // the group's ranges narrow its zones' (0 is no limit)
            if (g->low_velocity) low_velocity = std::max(low_velocity, g->low_velocity);
            if (g->high_velocity) high_velocity = std::min(high_velocity, g->high_velocity);
            if (g->low_key) low_key = std::max(low_key, g->low_key);
            if (g->high_key) high_key = std::min(high_key, g->high_key);
        }
        low_velocity = std::max(low_velocity, 1);
        if (low_key > high_key || low_velocity > high_velocity || high_key > 127 || high_velocity > 127) { leave(nullptr); continue; }

        auto& path = found[size_t(z.sample)];
        if (!path) {
            const auto& s = samples[size_t(z.sample)];
            path = find ? find(s.name, s.folder) : s.name;
        }
        if (path->empty()) { leave("missing files"); continue; }

        SfzRegion made;
        made.sample = *path;
        auto& zone = made.zone;
        zone.low_key = uint8_t(low_key);
        zone.high_key = uint8_t(high_key);
        zone.low_velocity = uint8_t(low_velocity);
        zone.high_velocity = uint8_t(high_velocity);
        zone.root = uint8_t(std::min(z.key, 127));
        if (z.options & 2) zone.keytrack = 0; // Pitch off: every key plays it at its own pitch
        zone.tune = clamped<int16_t>(std::lround(100.0 * z.coarse + z.fine + cents), -32000, 32000);
        zone.gain = float(std::clamp(z.volume + (g ? g->volume : 0) + master_db, -96.0, 12.0));
        zone.pan = float(std::clamp((z.pan + (g ? g->pan : 0)) / 50.0, -1.0, 1.0));
        if (z.options & 4) read.ignored.insert("reverse"); // played forward
        if (z.start > 0) made.offset = z.start;
        if (z.end > z.start) made.end = z.end;
        zone.one_shot = z.options & 1;
        made.loop_named = true; // the file's own loop markers are Logic's to ignore, and so ours
        if ((z.loop_options & 1) && !zone.one_shot) {
            // Play to end on release: it loops while held, then plays on through its tail.
            zone.loop = z.loop_options & 4 ? ZoneLoop::UntilReleased : ZoneLoop::On;
            zone.loop_start = z.loop_start;
            zone.loop_end = z.loop_end > z.loop_start ? z.loop_end : to_its_end;
            if (z.loop_direction) read.ignored.insert("loop direction"); // backwards or back and forth: played forward
            if (z.crossfade) read.ignored.insert("loop crossfade");
        }
        made.loop_asked = zone.loop != ZoneLoop::Off;
        if (g) {
            if (g->exclusive) zone.group = zone.off_by = uint32_t(g->exclusive); // a choke: the group cuts its own
            if (g->random) read.ignored.insert("random sample select");
            if (g->envelope_offsets) read.ignored.insert("group envelope offsets");
            const auto& p = place[size_t(z.group)];
            if (p.first) {
                zone.sequence = uint32_t(p.first);
                zone.sequence_length = uint32_t(chain_length[p.second]);
            }
        }
        read.regions.push_back(std::move(made));
        envelopes.push_back(envelope);
    }
    sfz_share_envelopes(envelopes, read);
    return read;
}

} // namespace melodee
