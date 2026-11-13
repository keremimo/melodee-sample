// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/dspreset.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace melodee {
namespace {
using Level = std::map<std::string, std::string>;

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
std::optional<double> number(const std::string& text) {
    auto t = trimmed(text);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (*end || !std::isfinite(v)) return std::nullopt;
    return v;
}
std::optional<std::string> at(const Level& l, const char* name) {
    auto it = l.find(name);
    if (it == l.end()) return std::nullopt;
    return it->second;
}
std::optional<double> number_at(const Level& l, const char* name) {
    auto t = at(l, name);
    return t ? number(*t) : std::nullopt;
}
// A note as a number or a name (c4 is 60).
std::optional<int> note(const std::optional<std::string>& text) {
    if (!text) return std::nullopt;
    if (auto v = number(*text)) return int(*v);
    return sfz_note(trimmed(*text));
}
std::vector<std::string> list(const std::optional<std::string>& text) {
    std::vector<std::string> out;
    if (!text) return out;
    size_t from = 0;
    while (from <= text->size()) {
        size_t comma = text->find(',', from);
        auto part = trimmed(text->substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (!part.empty()) out.push_back(part);
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    return out;
}
// loCC64, onHiCC70: whether it triggers rather than gates, whether it is the low limit, and its controller.
struct Gate { bool triggers = false, low = false; int controller = 0; };
std::optional<Gate> gate(const std::string& name) {
    Gate g;
    std::string rest = name;
    if (rest.rfind("on", 0) == 0) { g.triggers = true; rest = rest.substr(2); }
    auto l = lowered(rest);
    g.low = l.rfind("lo", 0) == 0;
    if (!g.low && l.rfind("hi", 0) != 0) return std::nullopt;
    if (l.compare(2, 2, "cc") != 0) return std::nullopt;
    char* end = nullptr;
    long c = std::strtol(l.c_str() + 4, &end, 10);
    if (end == l.c_str() + 4 || *end || c < 0 || c > 127) return std::nullopt;
    g.controller = int(c);
    return g;
}
const std::set<std::string> known {
    "path", "rootNote", "loNote", "hiNote", "loVel", "hiVel", "volume", "groupVolume", "globalVolume", "pan", "groupPan", "tuning", "groupTuning",
    "globalTuning", "pitchKeyTrack", "attack", "decay", "sustain", "release", "ampEnvAttack", "ampEnvDecay", "ampEnvSustain", "ampEnvRelease",
    "start", "end", "loopStart", "loopEnd", "loopEnabled", "seqMode", "seqLength", "seqPosition", "trigger", "tags", "silencedByTags",
    "silencingMode", "enabled", "ampVelTrack", "name", "minVersion", "loopCrossfade",
};
constexpr double longest_stage = 10.0;

// A volume written as linear 0: the format's way to mute a level.
bool muted(const std::optional<std::string>& text) {
    if (!text) return false;
    auto t = lowered(trimmed(*text));
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "db") == 0) return false;
    auto v = number(t);
    return v && *v <= 0;
}
std::string text_of(double v) {
    char made[32];
    std::snprintf(made, sizeof(made), "%.9g", v);
    return made;
}
bool truth(const std::string& text) {
    auto t = lowered(trimmed(text));
    if (auto v = number(t)) return *v >= 0.5;
    return t == "true";
}

// A <binding>'s value from its control's: factor, then the translation (linear by default, from the control's range
// or translationInputMin/Max to translationOutputMin/Max, or the value itself when no output range is named; a table
// of input,output pairs drawn straight between; or a fixed value).
std::optional<std::string> bound(const Level& b, double value, double low, double high) {
    value *= number_at(b, "factor").value_or(1);
    auto translation = lowered(at(b, "translation").value_or("linear"));
    if (translation == "fixed_value") return at(b, "translationValue");
    if (translation == "table") {
        std::vector<std::pair<double, double>> points;
        for (const auto& pair : [&] {
                 std::vector<std::string> parts;
                 auto text = at(b, "translationTable").value_or("0,0;1,1");
                 size_t from = 0;
                 while (from <= text.size()) {
                     size_t semi = text.find(';', from);
                     parts.push_back(text.substr(from, semi == std::string::npos ? std::string::npos : semi - from));
                     if (semi == std::string::npos) break;
                     from = semi + 1;
                 }
                 return parts;
             }()) {
            auto both = list(pair);
            if (both.size() != 2) continue;
            auto x = number(both[0]), y = number(both[1]);
            if (x && y) points.push_back({*x, *y});
        }
        if (points.empty()) return std::nullopt;
        std::sort(points.begin(), points.end());
        if (value <= points.front().first) return text_of(points.front().second);
        for (size_t n = 1; n < points.size(); ++n)
            if (value <= points[n].first) {
                const auto& [x0, y0] = points[n - 1];
                const auto& [x1, y1] = points[n];
                return text_of(x1 > x0 ? y0 + (value - x0) / (x1 - x0) * (y1 - y0) : y1);
            }
        return text_of(points.back().second);
    }
    auto out_low = number_at(b, "translationOutputMin"), out_high = number_at(b, "translationOutputMax");
    if (!out_low && !out_high) return text_of(value);
    const double in_low = number_at(b, "translationInputMin").value_or(low), in_high = number_at(b, "translationInputMax").value_or(high);
    double t = in_high != in_low ? std::clamp((value - in_low) / (in_high - in_low), 0.0, 1.0) : 0.0;
    if (lowered(at(b, "translationReversed").value_or("")) == "true") t = 1 - t;
    const double lo = out_low.value_or(0), hi = out_high.value_or(1);
    return text_of(lo + t * (hi - lo));
}

// Roadmap P3: the preset as its face first shows it. DecentSampler pushes each <ui> control's starting value through
// its <binding>s when the preset opens (triggerOnLoad, on unless turned off): a knob's value, a button's state (its
// 0-based value), a menu's option (its 1-based value). What a binding sets is written into the tree before it is read:
// at level instrument the <groups>, at level group the groups its position or tags pick, at a tag the <tag>; a sample's
// start, end and loop, which only a <sample> carries, into each sample in reach. Bindings to what the sampler has no
// place for are named.
void apply_bindings(DsElement& root, std::set<std::string>& ignored) {
    struct Push { const Level* binding; double value, low, high; };
    std::vector<Push> pushes;
    auto controls = [&](auto& self, const DsElement& e) -> void {
        auto value_of = [&](const char* name, double otherwise) { return number_at(e.attributes, name).value_or(otherwise); };
        auto bindings_of = [&](const DsElement& holder, double value, double low, double high) {
            for (const auto& c : holder.children)
                if (c.name == "binding") pushes.push_back({&c.attributes, value, low, high});
        };
        if (e.name == "labeled-knob" || e.name == "control" || e.name == "knob" || e.name == "slider") {
            double low = value_of("minValue", 0), high = value_of("maxValue", 1);
            double value = value_of("value", 0);
            if (low < high) value = std::clamp(value, low, high);
            bindings_of(e, value, low, high);
            return;
        }
        if (e.name == "button" || e.name == "menu") {
            const bool menu = e.name == "menu";
            const auto chosen = long(value_of("value", 0)) - (menu ? 1 : 0); // a menu at 0 has none chosen
            long n = 0;
            for (const auto& c : e.children)
                if (c.name == (menu ? "option" : "state") && n++ == chosen) bindings_of(c, double(chosen), 0, 1);
            return;
        }
        for (const auto& c : e.children) self(self, c);
    };
    for (const auto& c : root.children)
        if (c.name == "ui") controls(controls, c);
    if (pushes.empty()) return;
    // Copies: a binding's attributes live in the tree the pushes change.
    std::vector<std::pair<Level, Push>> work;
    for (const auto& p : pushes) work.push_back({*p.binding, p});
    std::map<std::string, Level> tag_changes;
    for (const auto& [b, p] : work) {
        if (lowered(at(b, "triggerOnLoad").value_or("true")) == "false" || lowered(at(b, "enabled").value_or("true")) == "false") continue;
        const auto type = lowered(at(b, "type").value_or("")), level = lowered(at(b, "level").value_or(""));
        const auto parameter = at(b, "parameter").value_or("");
        if (type != "amp" && type != "general") continue; // effects (named elsewhere), the face's own controls
        auto value = bound(b, p.value, p.low, p.high);
        if (!value) continue;
        if (parameter == "TAG_ENABLED" || parameter == "TAG_VOLUME") {
            auto name = at(b, "identifier");
            if (!name) name = at(b, "tags");
            if (!name) continue;
            for (const auto& tag : list(name))
                tag_changes[tag][parameter == "TAG_ENABLED" ? "enabled" : "volume"] = parameter == "TAG_ENABLED" ? (truth(*value) ? "true" : "false") : *value;
            continue;
        }
        std::vector<DsElement*> reach; // the <groups> or <group>s it sets
        if (level == "instrument") {
            for (auto& c : root.children) if (c.name == "groups") reach.push_back(&c);
        } else if (level == "group") {
            auto by = at(b, "groupTags");
            if (!by) by = at(b, "tags");
            auto wanted = list(by);
            auto index = number_at(b, "groupIndex");
            if (!index) index = number_at(b, "position");
            long n = 0;
            for (auto& gs : root.children) {
                if (gs.name != "groups") continue;
                for (auto& g : gs.children) {
                    if (g.name != "group") continue;
                    const bool picked = by ? std::any_of(wanted.begin(), wanted.end(), [&](const std::string& t) {
                        auto has = list(at(g.attributes, "tags"));
                        return std::find(has.begin(), has.end(), t) != has.end();
                    }) : index && long(*index) == n;
                    if (picked) reach.push_back(&g);
                    ++n;
                }
            }
        } else {
            continue;
        }
        const bool instrument = level == "instrument";
        auto set = [&](DsElement& e, const char* name, const std::string& v, const char* alias = nullptr) {
            e.attributes[name] = v;
            if (alias) e.attributes.erase(alias);
        };
        auto each_sample = [&](DsElement& e, const char* name) {
            const auto frames = text_of(std::round(number(*value).value_or(0)));
            auto into = [&](auto& self, DsElement& under) -> void {
                for (auto& c : under.children) {
                    if (c.name == "sample") c.attributes[name] = frames;
                    else if (c.name == "group") self(self, c);
                }
            };
            into(into, e);
        };
        static const std::map<std::string, std::pair<const char*, const char*>> stages {
            {"ENV_ATTACK", {"attack", "ampEnvAttack"}}, {"ENV_DECAY", {"decay", "ampEnvDecay"}},
            {"ENV_SUSTAIN", {"sustain", "ampEnvSustain"}}, {"ENV_RELEASE", {"release", "ampEnvRelease"}},
        };
        static const std::map<std::string, const char*> places {
            {"SAMPLE_START", "start"}, {"SAMPLE_END", "end"}, {"LOOP_START", "loopStart"}, {"LOOP_END", "loopEnd"},
        };
        for (auto* e : reach) {
            if (parameter == "AMP_VOLUME") set(*e, "volume", *value, instrument ? "globalVolume" : "groupVolume");
            else if (parameter == "GLOBAL_TUNING" && instrument) set(*e, "globalTuning", *value);
            else if (parameter == "GROUP_TUNING" && !instrument) set(*e, "groupTuning", *value, "tuning");
            else if (parameter == "PAN") set(*e, "pan", *value);
            else if (parameter == "AMP_VEL_TRACK") set(*e, "ampVelTrack", *value);
            else if (parameter == "ENABLED" && !instrument) set(*e, "enabled", truth(*value) ? "true" : "false");
            else if (auto s = stages.find(parameter); s != stages.end()) set(*e, s->second.first, *value, s->second.second);
            else if (auto w = places.find(parameter); w != places.end()) each_sample(*e, w->second);
            else ignored.insert(parameter);
        }
    }
    for (const auto& change : tag_changes) {
        const std::string& name = change.first;
        const Level& changes = change.second;
        bool found = false;
        auto into = [&](auto& self, DsElement& e) -> void {
            for (auto& c : e.children) {
                if (c.name == "tag" && at(c.attributes, "name") == name) {
                    for (const auto& [k, v] : changes) c.attributes[k] = v;
                    found = true;
                } else if (c.name != "ui") self(self, c);
            }
        };
        into(into, root);
        if (found) continue;
        DsElement tag {"tag", changes, {}};
        tag.attributes["name"] = name;
        root.children.push_back({"tags", {}, {std::move(tag)}});
    }
}
} // namespace

std::optional<double> dspreset_decibels(const std::string& text) {
    auto t = lowered(trimmed(text));
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "db") == 0) return number(t.substr(0, t.size() - 2));
    auto v = number(t);
    if (!v) return std::nullopt;
    return *v > 0 ? 20 * std::log10(*v) : -96.0;
}

SfzRead read_dspreset(const DsElement& preset, std::string* error) {
    SfzRead read;
    if (preset.name != "DecentSampler") { if (error) *error = "Not a DecentSampler preset"; return read; }
    DsElement root = preset;
    apply_bindings(root, read.ignored);
    struct Found { Level sample, group, groups; };
    std::vector<Found> samples;
    std::map<std::string, std::pair<bool, double>> tags; // on, level
    unsigned others = 0;
    // The tree, walked as the format nests it.
    auto walk = [&](auto& self, const DsElement& e, const Level* groups, const Level* group) -> void {
        if (e.name == "groups") { for (const auto& c : e.children) self(self, c, &e.attributes, nullptr); return; }
        if (e.name == "group" && groups) { for (const auto& c : e.children) self(self, c, groups, &e.attributes); return; }
        if (e.name == "sample" && groups) { samples.push_back({e.attributes, group ? *group : Level {}, *groups}); return; }
        if (e.name == "tag") {
            if (auto name = at(e.attributes, "name"))
                tags[*name] = {lowered(at(e.attributes, "enabled").value_or("true")) != "false", number_at(e.attributes, "volume").value_or(1)};
            return;
        }
        if (e.name == "effect") read.ignored.insert("effects");
        if (e.name == "lfo" || e.name == "envelope") read.ignored.insert("modulators");
        if (e.name == "oscillator") ++others;
        for (const auto& c : e.children) self(self, c, groups, group);
    };
    walk(walk, root, nullptr, nullptr);
    read.left = others;
    // What silences what: only a tag something waits on becomes a zone's group.
    std::set<std::string> silencing;
    for (const auto& f : samples) {
        auto by = at(f.sample, "silencedByTags");
        if (!by) by = at(f.group, "silencedByTags");
        if (!by) by = at(f.groups, "silencedByTags");
        for (const auto& t : list(by)) silencing.insert(t);
    }
    std::map<std::string, uint32_t> numbers;
    auto number_of = [&](const std::string& tag) {
        auto it = numbers.find(tag);
        if (it != numbers.end()) return it->second;
        uint32_t n = uint32_t(numbers.size() + 1);
        numbers[tag] = n;
        return n;
    };
    std::vector<SfzEnvelope> envelopes;
    struct Sequence { std::string mode; int position = 1, length = 0; };
    std::vector<Sequence> sequences;
    for (const auto& f : samples) {
        const Level& s = f.sample;
        const Level& g = f.group;
        const Level& gs = f.groups;
        auto look = [&](const char* name) { auto v = at(s, name); if (!v) v = at(g, name); if (!v) v = at(gs, name); return v; };
        for (const Level* level : {&s, &g, &gs})
            for (const auto& [name, value] : *level) { (void)value; if (!known.count(name) && !gate(name)) read.ignored.insert(name); }
        auto leave = [&](const std::string& why) { ++read.left; read.left_by.insert(why); };
        if (lowered(at(g, "enabled").value_or("")) == "false" || lowered(at(s, "enabled").value_or("")) == "false") { leave("enabled"); continue; }
        auto its = list(at(s, "tags"));
        for (const auto& t : list(at(g, "tags"))) its.push_back(t);
        if (std::any_of(its.begin(), its.end(), [&](const std::string& t) { auto it = tags.find(t); return it != tags.end() && !it->second.first; })) {
            leave("tags");
            continue;
        }
        { // a level at linear 0 (often a layer's fader down on the preset's face) plays nothing: no voice spent on it
            auto group_level = at(g, "groupVolume"); if (!group_level) group_level = at(g, "volume");
            auto global_level = at(gs, "globalVolume"); if (!global_level) global_level = at(gs, "volume");
            const bool tag_down = std::any_of(its.begin(), its.end(), [&](const std::string& t) { auto it = tags.find(t); return it != tags.end() && it->second.second <= 0; });
            if (muted(at(s, "volume")) || muted(group_level) || muted(global_level) || tag_down) { leave("volume"); continue; }
        }
        auto trigger = lowered(look("trigger").value_or("attack"));
        if (trigger == "legato") { leave("trigger"); continue; } // only on a legato: every press would double it
        std::optional<std::string> waiting;
        for (const Level* level : {&s, &g, &gs})
            for (const auto& [name, value] : *level) {
                auto found = gate(name);
                auto limit = number(value);
                if (!found || !limit || *limit < 0) continue;
                // nothing here follows a controller: read where it starts; one that triggers a sound is no press
                double starts = found->controller == 7 ? 100 : found->controller == 10 ? 64 : found->controller == 11 ? 127 : 0;
                if ((found->triggers || (found->low ? starts < *limit : starts > *limit)) && (!waiting || name < *waiting)) waiting = name;
            }
        if (waiting) { leave(*waiting); continue; }
        auto path = at(s, "path");
        if (!path || trimmed(*path).empty()) { ++read.left; continue; }
        if (read.regions.size() >= max_zones) { ++read.left; continue; }
        SfzRegion r;
        r.sample = trimmed(*path);
        std::replace(r.sample.begin(), r.sample.end(), '\\', '/');
        auto& z = r.zone;
        z.low_key = uint8_t(std::clamp(note(look("loNote")).value_or(0), 0, 127));
        z.high_key = uint8_t(std::clamp(note(look("hiNote")).value_or(127), 0, 127));
        z.root = uint8_t(std::clamp(note(at(s, "rootNote")).value_or(z.low_key == z.high_key ? z.low_key : 60), 0, 127));
        z.low_velocity = uint8_t(std::clamp(note(at(s, "loVel")).value_or(1), 1, 127));
        z.high_velocity = uint8_t(std::clamp(note(at(s, "hiVel")).value_or(127), 1, 127));
        double tuning = 0;
        if (auto t = look("tuning")) tuning += number(*t).value_or(0);
        tuning += number_at(g, "groupTuning").value_or(0) + number_at(gs, "globalTuning").value_or(0);
        z.tune = int16_t(std::clamp(std::lround(tuning * 100), -32000l, 32000l));
        z.keytrack = int16_t(std::clamp(std::lround(number(look("pitchKeyTrack").value_or("1")).value_or(1) * 100), -32000l, 32000l));
        double tagged = 0;
        for (const auto& t : its) if (auto it = tags.find(t); it != tags.end()) tagged += dspreset_decibels(std::to_string(it->second.second)).value_or(0);
        auto db = [&](const std::optional<std::string>& text) { return text ? dspreset_decibels(*text).value_or(0) : 0.0; };
        auto group_volume = at(g, "groupVolume"); if (!group_volume) group_volume = at(g, "volume");
        auto global_volume = at(gs, "globalVolume"); if (!global_volume) global_volume = at(gs, "volume");
        z.gain = float(std::clamp(db(at(s, "volume")) + db(group_volume) + db(global_volume) + tagged, -96.0, 24.0));
        double pan = number(look("pan").value_or("0")).value_or(0) + number_at(g, "groupPan").value_or(0);
        z.pan = float(std::clamp(pan / 100, -1.0, 1.0));
        for (const auto& t : its) if (silencing.count(t)) { z.group = number_of(t); break; }
        auto by = list(look("silencedByTags"));
        if (!by.empty()) z.off_by = number_of(by.front());
        z.on_release = trigger == "release"; // roadmap P3: played when its key is let go
        if (auto v = number_at(s, "start")) r.offset = int64_t(*v);
        if (auto v = number_at(s, "end")) r.end = int64_t(*v) + 1; // its numbers name its last frame
        r.loop_named = true;
        if (lowered(at(s, "loopEnabled").value_or("")) == "true") {
            r.loop_asked = true;
            z.loop = ZoneLoop::On;
            if (auto v = number_at(s, "loopStart")) z.loop_start = uint32_t(std::max(*v, 0.0));
            if (auto v = number_at(s, "loopEnd")) z.loop_end = uint32_t(std::max(*v, 0.0)) + 1;
            if (number_at(s, "loopCrossfade").value_or(0) > 0) read.ignored.insert("loopCrossfade");
        }
        if (read.regions.empty()) {
            auto track = at(g, "ampVelTrack");
            if (!track) track = at(gs, "ampVelTrack");
            read.velocity = track ? number(*track).value_or(1) * 100 : 100;
        }
        auto stage = [&](const char* longer, const char* shorter) -> std::optional<double> {
            for (const Level* level : {&s, &g, &gs}) {
                auto v = at(*level, longer);
                if (!v) v = at(*level, shorter);
                if (v) if (auto n = number(*v)) return n;
            }
            return std::nullopt;
        };
        SfzEnvelope e {stage("ampEnvAttack", "attack"), stage("ampEnvDecay", "decay"), stage("ampEnvSustain", "sustain"), stage("ampEnvRelease", "release")};
        if (e.sustain) e.sustain = *e.sustain * 100;
        if (e.decay && *e.decay >= longest_stage) e.sustain = 100; // a fall that long is the sample's own
        envelopes.push_back(e);
        Sequence q;
        q.mode = lowered(look("seqMode").value_or(""));
        q.position = std::max(int(number(look("seqPosition").value_or("1")).value_or(1)), 1);
        q.length = std::max(int(number(look("seqLength").value_or("0")).value_or(0)), 0);
        sequences.push_back(q);
        read.regions.push_back(std::move(r));
    }
    // Round robins and random picks: a length not said is the most any zone of its keys and velocities holds; a random
    // mode becomes a share of a press's one roll of the dice.
    auto cell = [](const SampleZone& z) { return std::array<int, 4> {z.low_key, z.high_key, z.low_velocity, z.high_velocity}; };
    std::map<std::array<int, 4>, int> most;
    for (size_t n = 0; n < read.regions.size(); ++n) {
        const auto& q = sequences[n];
        if (q.mode != "round_robin" && q.mode != "random" && q.mode != "true_random") continue;
        auto& m = most[cell(read.regions[n].zone)];
        m = std::max({m, 1, q.position});
    }
    for (size_t n = 0; n < read.regions.size(); ++n) {
        const auto& q = sequences[n];
        auto& z = read.regions[n].zone;
        const int told = most.count(cell(z)) ? most[cell(z)] : 1;
        if (q.mode == "round_robin") {
            z.sequence = uint32_t(q.position);
            z.sequence_length = uint32_t(q.length ? q.length : told);
        } else if (q.mode == "random" || q.mode == "true_random") {
            int count = std::max(q.length ? q.length : told, 1), place = std::min(q.position, count);
            z.low_random = float(place - 1) / float(count);
            z.high_random = float(place) / float(count);
        }
    }
    sfz_share_envelopes(envelopes, read);
    return read;
}

} // namespace melodee
