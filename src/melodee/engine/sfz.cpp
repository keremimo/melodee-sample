// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sfz.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <map>

namespace melodee {
namespace {
using Opcodes = std::map<std::string, std::string>;

constexpr double longest_stage = 10.0; // seconds: a decay this long is the sample's own to make

std::optional<int> int_of(const std::string& text) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(text.c_str(), &end, 10);
    if (*end || errno) return std::nullopt;
    return int(std::clamp<long>(value, -2147483647L, 2147483647L));
}

std::optional<double> double_of(const std::string& text) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return std::nullopt;
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (*end || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<std::string> at(const Opcodes& o, const std::string& name) {
    auto found = o.find(name);
    if (found == o.end()) return std::nullopt;
    return found->second;
}
std::optional<int> int_at(const Opcodes& o, const std::string& name) {
    auto text = at(o, name);
    return text ? int_of(*text) : std::nullopt;
}
std::optional<double> double_at(const Opcodes& o, const std::string& name) {
    auto text = at(o, name);
    return text ? double_of(*text) : std::nullopt;
}
std::optional<int> note_at(const Opcodes& o, const std::string& name) {
    auto text = at(o, name);
    return text ? sfz_note(*text) : std::nullopt;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text) {
    size_t from = 0, to = text.size();
    while (from < to && std::isspace(static_cast<unsigned char>(text[from]))) ++from;
    while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1]))) --to;
    return text.substr(from, to - from);
}

bool starts_with(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t n = 0; n <= text.size(); ++n)
        if (n == text.size() || text[n] == '\n' || text[n] == '\r') {
            if (n > from) out.push_back(text.substr(from, n - from));
            from = n + 1;
        }
    return out;
}

void replace_all(std::string& text, const std::string& what, const std::string& with) {
    if (what.empty()) return;
    for (size_t at_ = text.find(what); at_ != std::string::npos; at_ = text.find(what, at_ + with.size())) text.replace(at_, what.size(), with);
}

using Defines = std::map<std::string, std::string>;

std::string defined(std::string text, const Defines& defines) {
    if (text.find('$') == std::string::npos) return text;
    // the longest first, so $KEY2 is not $KEY's
    std::vector<const Defines::value_type*> order;
    for (const auto& d : defines) order.push_back(&d);
    std::stable_sort(order.begin(), order.end(), [](auto a, auto b) { return a->first.size() > b->first.size(); });
    for (auto d : order) replace_all(text, d->first, d->second);
    return text;
}

// The text without comments, its #defines applied and its #includes read in.
std::string prepared(const std::string& text, const SfzInclude& include, unsigned depth, Defines& defines) {
    std::string out;
    for (const auto& raw : lines_of(sfz_uncommented(text))) {
        std::string line = trimmed(raw);
        if (starts_with(line, "#define")) {
            std::vector<std::string> parts;
            size_t n = 0;
            while (n < line.size()) {
                while (n < line.size() && std::isspace(static_cast<unsigned char>(line[n]))) ++n;
                size_t from = n;
                while (n < line.size() && !std::isspace(static_cast<unsigned char>(line[n]))) ++n;
                if (n > from) parts.push_back(line.substr(from, n - from));
            }
            if (parts.size() >= 3) defines[parts[1]] = parts[2];
            continue;
        }
        if (starts_with(line, "#include")) {
            // its path may name what was defined: "$DIR/$DYN.txt"
            std::string named = defined(line, defines);
            size_t open = named.find('"'), close = named.rfind('"');
            if (depth < 8 && include && open != std::string::npos && close > open)
                if (auto more = include(named.substr(open + 1, close - open - 1))) {
                    out += prepared(*more, include, depth + 1, defines);
                    out += '\n';
                }
            continue;
        }
        out += defined(raw, defines);
        out += '\n';
    }
    return out;
}

// Where a controller starts, 0 to 1: set_hdcc or set_cc if the file says, else as a player has them.
double starts(int controller, const Opcodes& o) {
    if (auto high = double_at(o, "set_hdcc" + std::to_string(controller))) return std::clamp(*high, 0.0, 1.0);
    if (auto plain = double_at(o, "set_cc" + std::to_string(controller))) return std::clamp(*plain / 127, 0.0, 1.0);
    return controller == 7 ? 100.0 / 127 : controller == 10 ? 64.0 / 127 : controller == 11 ? 1.0 : 0.0;
}

struct Gate { bool low = false, unit = false; int controller = 0; };
// locc64, hihdcc70: whether it is the low limit, whether it counts 0 to 1, its controller.
std::optional<Gate> gate_of(const std::string& name) {
    Gate g;
    g.low = starts_with(name, "lo");
    if (!g.low && !starts_with(name, "hi")) return std::nullopt;
    std::string rest = name.substr(2);
    g.unit = starts_with(rest, "hd");
    if (g.unit) rest = rest.substr(2);
    if (!starts_with(rest, "cc")) return std::nullopt;
    auto controller = int_of(rest.substr(2));
    if (!controller || *controller < 0 || *controller > 127) return std::nullopt;
    g.controller = *controller;
    return g;
}

struct Moved { std::string stage; int controller = 0; };
// A stage moved by a controller with no curve of its own (ampeg_release_oncc72, ampeg_releasecc64).
std::optional<Moved> moved_of(const std::string& name, const Opcodes& o) {
    if (!starts_with(name, "ampeg_")) return std::nullopt;
    for (const char* stage : {"attack", "hold", "decay", "sustain", "release"}) {
        std::string s = stage;
        if (name.compare(6, s.size(), s) != 0) continue;
        std::string rest = name.substr(6 + s.size());
        if (starts_with(rest, "_on")) rest = rest.substr(3);
        if (!starts_with(rest, "cc")) return std::nullopt;
        auto controller = int_of(rest.substr(2));
        if (!controller || *controller < 0 || *controller > 127) return std::nullopt;
        if (o.count("ampeg_" + s + "_curvecc" + std::to_string(*controller))) return std::nullopt;
        return Moved {s, *controller};
    }
    return std::nullopt;
}

bool known(const std::string& name, const Opcodes& o) {
    static const std::set<std::string> names {
        "sample", "default_path", "note_offset", "octave_offset", "key", "lokey", "hikey", "pitch_keycenter", "lovel", "hivel", "tune",
        "pitch", "transpose", "volume", "gain", "pan", "offset", "end", "loop_mode", "loopmode", "loop_type", "loop_start", "loopstart",
        "loop_end", "loopend", "seq_length", "seq_position", "lorand", "hirand", "group", "off_by", "offby", "pitch_keytrack",
        "ampeg_attack", "ampeg_hold", "ampeg_decay", "ampeg_sustain", "ampeg_release", "amp_veltrack", "cutoff", "resonance",
        "fil_type", "filtype", "fil_veltrack", "fil_keytrack", "fil_keycenter", "global_volume", "master_volume", "group_volume", "trigger",
        "region_label", "group_label", "master_label", "global_label", // only for reading
    };
    return names.count(name) || starts_with(name, "label_cc") || starts_with(name, "set_cc") || starts_with(name, "set_hdcc")
        || gate_of(name) || moved_of(name, o);
}

// What keeps a region from a press, named; none if nothing does. A controller's
// condition is read where it starts: a layer for the pedal down is left out.
std::optional<std::string> kept_from(const Opcodes& o) {
    for (const auto& [name, value] : o) { // in name order, the first waiting
        auto found = gate_of(name);
        auto limit = double_of(value);
        if (!found || !limit) continue;
        double start = starts(found->controller, o), asked = found->unit ? *limit : *limit / 127;
        if (found->low ? start < asked : start > asked) return name;
    }
    // what plays on a release, or only on a legato, played on every press would double it
    if (auto trigger = at(o, "trigger")) {
        auto kind = lowered(*trigger);
        if (kind == "release" || kind == "release_key" || kind == "legato") return std::string("trigger");
    }
    if (auto last = note_at(o, "sw_last")) {
        auto first = o.count("sw_default") ? note_at(o, "sw_default") : note_at(o, "sw_lokey");
        if (first && *last != *first) return std::string("sw_last");
    }
    return std::nullopt;
}

// A region's envelope: what it names, moved by what a controller starts at. A
// hold goes on its decay; a fall past the longest stage is the sample's own.
SfzEnvelope envelope_of(const Opcodes& o) {
    struct Move { std::string stage; int controller; double amount; };
    std::vector<Move> moves;
    for (const auto& [name, text] : o)
        if (auto move = moved_of(name, o))
            if (auto amount = double_of(text)) moves.push_back({move->stage, move->controller, *amount});
    std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.controller < b.controller; });
    auto stage = [&](const std::string& name, double plain) {
        auto value = double_at(o, "ampeg_" + name);
        for (const auto& move : moves)
            if (move.stage == name) value = value.value_or(plain) + move.amount * starts(move.controller, o);
        return value;
    };
    SfzEnvelope made {stage("attack", 0), stage("decay", 0), stage("sustain", 100), stage("release", 0.001)};
    auto hold = stage("hold", 0);
    if (hold && *hold > 0) made.decay = made.decay.value_or(0) + *hold;
    if (made.decay && *made.decay >= longest_stage) made.sustain = 100;
    return made;
}

// A fil_type as the pad's two-pole kinds, a one-pole's too; none: a notch or a shelf.
std::optional<PadFilter> filter_of(const std::string& kind) {
    auto k = lowered(kind);
    if (starts_with(k, "lpf")) return PadFilter::Lpf2;
    if (starts_with(k, "hpf")) return PadFilter::Hpf2;
    if (starts_with(k, "bpf")) return PadFilter::Bpf2;
    return std::nullopt;
}

template <typename T> T clamped(int value, int low, int high) { return T(std::clamp(value, low, high)); }

std::optional<SfzRegion> region_of(const Opcodes& o) {
    auto named = at(o, "sample");
    if (!named || named->empty() || (*named)[0] == '*') return std::nullopt; // *sine and the like: generated
    SfzRegion made;
    made.sample = at(o, "default_path").value_or("") + *named;
    std::replace(made.sample.begin(), made.sample.end(), '\\', '/');
    int shift = int_at(o, "note_offset").value_or(0) + 12 * int_at(o, "octave_offset").value_or(0);
    int low = 0, high = 127, root = 60;
    if (auto both = note_at(o, "key")) low = high = root = *both;
    if (auto v = note_at(o, "lokey")) low = *v;
    if (auto v = note_at(o, "hikey")) high = *v;
    if (low < 0 || high < 0) return std::nullopt; // played by nothing
    if (auto v = note_at(o, "pitch_keycenter")) root = *v;
    auto& z = made.zone;
    z.low_key = clamped<uint8_t>(low + shift, 0, 127);
    z.high_key = clamped<uint8_t>(high + shift, 0, 127);
    z.root = clamped<uint8_t>(root + shift, 0, 127);
    // velocity 0 is a release: the lowest that plays is 1
    z.low_velocity = clamped<uint8_t>(int_at(o, "lovel").value_or(1), 1, 127);
    z.high_velocity = clamped<uint8_t>(int_at(o, "hivel").value_or(127), 1, 127);
    auto tune = int_at(o, "tune");
    if (!tune) tune = int_at(o, "pitch");
    z.tune = clamped<int16_t>(tune.value_or(0) + 100 * int_at(o, "transpose").value_or(0), -32000, 32000);
    z.keytrack = clamped<int16_t>(int_at(o, "pitch_keytrack").value_or(100), -32000, 32000);
    auto volume = double_at(o, "volume");
    if (!volume) volume = double_at(o, "gain");
    double gain = volume.value_or(0);
    for (const char* more : {"group_volume", "master_volume", "global_volume"}) gain += double_at(o, more).value_or(0);
    z.gain = float(gain);
    z.pan = float(std::clamp(double_at(o, "pan").value_or(0) / 100, -1.0, 1.0));
    auto kind = at(o, "fil_type");
    if (!kind) kind = at(o, "filtype");
    auto filter = filter_of(kind.value_or("lpf_2p"));
    auto hz = double_at(o, "cutoff");
    if (hz && *hz > 0 && filter) {
        z.cutoff = float(std::clamp(*hz, 20.0, 20000.0));
        z.filter = *filter;
        z.resonance = float(std::clamp(double_at(o, "resonance").value_or(0), 0.0, 40.0));
        z.cutoff_velocity = clamped<int16_t>(int_at(o, "fil_veltrack").value_or(0), -32000, 32000);
        z.cutoff_key = clamped<int16_t>(int_at(o, "fil_keytrack").value_or(0), -32000, 32000);
        z.cutoff_center = clamped<uint8_t>(note_at(o, "fil_keycenter").value_or(60), 0, 127);
    }
    z.sequence_length = uint32_t(std::max(int_at(o, "seq_length").value_or(0), 0));
    z.sequence = uint32_t(std::max(int_at(o, "seq_position").value_or(1), 1));
    z.low_random = float(std::clamp(double_at(o, "lorand").value_or(0), 0.0, 1.0));
    z.high_random = float(std::clamp(double_at(o, "hirand").value_or(1), 0.0, 1.0));
    z.group = uint32_t(std::max(int_at(o, "group").value_or(0), 0));
    auto off_by = int_at(o, "off_by");
    if (!off_by) off_by = int_at(o, "offby");
    z.off_by = uint32_t(std::max(off_by.value_or(0), 0));
    if (auto offset = int_at(o, "offset")) made.offset = *offset;
    if (auto end = int_at(o, "end")) {
        if (*end < 0) return std::nullopt; // end=-1: never plays
        made.end = int64_t(*end) + 1;
    }
    auto mode = at(o, "loop_mode");
    if (!mode) mode = at(o, "loopmode");
    if (!mode) mode = at(o, "loop_type");
    made.loop_named = mode.has_value();
    if (mode) {
        auto m = lowered(*mode);
        if (m == "one_shot") z.one_shot = true;
        else if (m == "loop_continuous" || m == "forward") z.loop = ZoneLoop::On;
        else if (m == "loop_sustain") z.loop = ZoneLoop::UntilReleased;
    }
    made.loop_asked = z.loop != ZoneLoop::Off;
    auto start = int_at(o, "loop_start");
    if (!start) start = int_at(o, "loopstart");
    if (start) z.loop_start = uint32_t(std::max(*start, 0));
    auto end = int_at(o, "loop_end");
    if (!end) end = int_at(o, "loopend");
    if (end) z.loop_end = uint32_t(std::max(*end, 0)) + 1; // the format's end is the last frame in the loop
    return made;
}

// Commonest, the earlier of two as common.
double commonest(const std::vector<double>& values) {
    std::map<double, unsigned> counts;
    for (double v : values) ++counts[v];
    double best = values.front();
    for (double v : values) if (counts[v] > counts[best]) best = v;
    return best;
}

// The pad's envelope from the regions', each zone keeping how far it is from it.
void share(const std::vector<SfzEnvelope>& envelopes, SfzRead& read) {
    auto stage = [&](std::optional<double> SfzEnvelope::*field, double plain) {
        std::optional<std::vector<double>> out;
        bool any = false;
        for (const auto& e : envelopes) any |= (e.*field).has_value();
        if (!any) return out;
        out.emplace();
        for (const auto& e : envelopes) out->push_back((e.*field).value_or(plain));
        return out;
    };
    auto& regions = read.regions;
    if (auto values = stage(&SfzEnvelope::attack, 0)) {
        double sound = commonest(*values);
        read.envelope.attack = sound;
        for (size_t n = 0; n < regions.size(); ++n)
            regions[n].zone.attack = int16_t(((*values)[n] > 0 ? sfz_attack_step((*values)[n]) : 0) - (sound > 0 ? sfz_attack_step(sound) : 0));
    }
    if (auto values = stage(&SfzEnvelope::decay, 0)) {
        double sound = commonest(*values);
        read.envelope.decay = sound;
        for (size_t n = 0; n < regions.size(); ++n) regions[n].zone.decay = int16_t(sfz_decay_step((*values)[n]) - sfz_decay_step(sound));
    }
    if (auto values = stage(&SfzEnvelope::sustain, 100)) {
        double sound = commonest(*values);
        read.envelope.sustain = sound;
        for (size_t n = 0; n < regions.size(); ++n) regions[n].zone.sustain = int16_t(sfz_percent_step((*values)[n]) - sfz_percent_step(sound));
    }
    if (auto values = stage(&SfzEnvelope::release, 0.001)) {
        double sound = commonest(*values);
        read.envelope.release = sound;
        for (size_t n = 0; n < regions.size(); ++n) regions[n].zone.release = int16_t(sfz_release_step((*values)[n]) - sfz_release_step(sound));
    }
}

int time_step(double seconds) { // the inverse of time_seconds: 1 ms to 10 s over 100 steps
    return int(std::lround(std::clamp((std::log10(std::max(seconds, 0.001)) + 3) * 25, 0.0, 100.0)));
}
} // namespace

void sfz_share_envelopes(const std::vector<SfzEnvelope>& envelopes, SfzRead& read) { share(envelopes, read); }

int sfz_attack_step(double seconds) { return time_step(seconds / 0.3); }          // the sampler plays attack * 0.3
int sfz_decay_step(double seconds) { return time_step(seconds); }
int sfz_release_step(double seconds) { return time_step((seconds - 0.003) / 0.5); } // and release * 0.5 + 3 ms
int sfz_percent_step(double percent) { return int(std::lround(std::clamp(percent, 0.0, 100.0))); }

std::optional<int> sfz_note(const std::string& text) {
    if (auto number = int_of(text)) return number;
    if (text.empty()) return std::nullopt;
    auto lower = lowered(text);
    static const std::string letters = "cdefgab";
    static const int semitones[] {0, 2, 4, 5, 7, 9, 11};
    size_t at_ = letters.find(lower[0]);
    if (at_ == std::string::npos) return std::nullopt;
    size_t n = 1;
    int shift = 0;
    if (n < lower.size() && lower[n] == '#') { shift = 1; ++n; }
    else if (n < lower.size() && lower[n] == 'b') { shift = -1; ++n; }
    auto octave = int_of(lower.substr(n));
    if (!octave) return std::nullopt;
    return (*octave + 1) * 12 + semitones[at_] + shift;
}

// Whichever kind of comment starts first rules: //***** holds a /* that opens
// nothing. One opened and never closed is the rest of the file.
std::string sfz_uncommented(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t n = 0;
    while (n < text.size()) {
        if (text[n] == '/' && n + 1 < text.size()) {
            if (text[n + 1] == '/') { // to its line's end, which stays
                n += 2;
                while (n < text.size() && text[n] != '\n' && text[n] != '\r') ++n;
                continue;
            }
            if (text[n + 1] == '*') {
                n += 2;
                while (n + 1 < text.size() && !(text[n] == '*' && text[n + 1] == '/')) ++n;
                if (n + 1 >= text.size()) break;
                n += 2;
                continue;
            }
        }
        out += text[n++];
    }
    return out;
}

// name=value, a value running on to the next name (a sample's name may have spaces in it).
std::vector<std::pair<std::string, std::string>> sfz_opcodes(const std::string& text) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    auto named = [&](char c) { return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    struct Name { size_t from, equals; };
    std::vector<Name> names;
    for (size_t n = 0; n < text.size(); ++n) {
        if (text[n] != '=') continue;
        size_t from = n;
        while (from > 0 && named(text[from - 1])) --from;
        while (from < n && digit(text[from])) ++from; // a name does not start with a digit
        if (from < n) names.push_back({from, n});
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t k = 0; k < names.size(); ++k) {
        size_t end = k + 1 < names.size() ? names[k + 1].from : text.size();
        out.emplace_back(lowered(text.substr(names[k].from, names[k].equals - names[k].from)),
                         trimmed(text.substr(names[k].equals + 1, end - names[k].equals - 1)));
    }
    return out;
}

SfzRead read_sfz(const std::string& source, const SfzInclude& include) {
    Defines defines;
    const std::string text = prepared(source, include, 0, defines);
    SfzRead read;
    Opcodes control, global, master, group, region, pending;
    bool open = false;
    std::vector<SfzEnvelope> envelopes;
    std::string scope = "region-to-come";

    auto close = [&] {
        if (!open) return;
        open = false;
        if (read.regions.size() >= max_zones) { ++read.left; return; }
        Opcodes merged = control;
        for (const Opcodes* level : {&global, &master, &group, &region})
            for (const auto& [name, value] : *level) merged[name] = value;
        if (auto why = kept_from(merged)) { ++read.left; read.left_by.insert(*why); return; }
        for (const auto& [name, value] : merged) { (void)value; if (!known(name, merged)) read.ignored.insert(name); }
        auto kind = at(merged, "fil_type");
        if (!kind) kind = at(merged, "filtype");
        if (kind && !filter_of(*kind)) read.ignored.insert("fil_type");
        auto made = region_of(merged);
        if (!made) { ++read.left; return; }
        if (read.regions.empty()) read.velocity = double_at(merged, "amp_veltrack").value_or(100);
        read.regions.push_back(std::move(*made));
        envelopes.push_back(envelope_of(merged));
    };
    auto take = [&](const std::string& part) {
        for (auto& [name, value] : sfz_opcodes(part)) {
            Opcodes& into = scope == "control" ? control : scope == "global" ? global : scope == "master" ? master
                          : scope == "group" ? group : scope == "region" ? region : pending;
            into[name] = value;
        }
    };

    size_t from = 0, n = 0;
    while (n < text.size()) {
        if (text[n] != '<') { ++n; continue; }
        // <\s*name\s*>
        size_t k = n + 1;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        size_t word = k;
        while (k < text.size() && std::isalpha(static_cast<unsigned char>(text[k]))) ++k;
        size_t word_end = k;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        if (word_end == word || k >= text.size() || text[k] != '>') { ++n; continue; }
        take(text.substr(from, n - from));
        from = n = k + 1;
        close();
        scope = lowered(text.substr(word, word_end - word));
        if (scope == "global") { global.clear(); master.clear(); group.clear(); }
        else if (scope == "master") { master.clear(); group.clear(); }
        else if (scope == "group") group.clear();
        else if (scope == "region") { region.clear(); open = true; }
    }
    take(text.substr(from));
    close();
    share(envelopes, read);
    return read;
}

} // namespace melodee
