// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A drum plug-in's voices, as an Open MPC drum machine states them. 8W8 and
// its siblings (sd88me/mpc-vst-TR-MPC, ported from schwung) carry their
// screen's map in the library as JSON, once for each of their note maps
// (Rack 36, GM):
//
//   {"pad_layout":"drums", ..., "levels":{"bd":{"name":"Bass Drum", ...,
//    "note":36, ...}, "sd":{...}, ...}}
//
// Found, a PLUGIN track on it plays its pads as Drums (model/scales.h) and the
// Keys page names them. Plain C++, no JUCE: the host tests read it too.
namespace melodee {

struct DrumVoice {
    uint8_t key = 0;  // the MIDI note it plays on
    std::string name; // "Bass Drum"
};

namespace drum_map_detail {
// Just enough JSON to walk an object's members and read their strings and integers; anything else is skipped.
struct Reader {
    std::string_view s;
    size_t at = 0;
    unsigned depth = 0;

    void space() { while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r' || s[at] == '\t')) ++at; }
    bool peek(char c) { space(); return at < s.size() && s[at] == c; }
    bool eat(char c) { if (!peek(c)) return false; ++at; return true; }
    bool string(std::string* out) {
        if (!eat('"')) return false;
        while (at < s.size() && s[at] != '"') {
            char c = s[at++];
            if (c == '\\') {
                if (at >= s.size()) return false;
                char e = s[at++];
                if (e == 'u') { at += 4; c = '?'; } // names here are ASCII
                else c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false; // the end of the library's string, or not JSON
            }
            if (out) out->push_back(c);
        }
        return eat('"');
    }
    bool number(long* out) {
        space();
        size_t from = at;
        bool minus = at < s.size() && s[at] == '-';
        if (minus) ++at;
        long value = 0;
        bool digits = false;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') { value = value * 10 + (s[at++] - '0'); digits = true; if (value > 1000000) value = 1000000; }
        while (at < s.size() && (s[at] == '.' || s[at] == 'e' || s[at] == 'E' || s[at] == '+' || s[at] == '-' || (s[at] >= '0' && s[at] <= '9'))) ++at;
        if (!digits) { at = from; return false; }
        if (out) *out = minus ? -value : value;
        return true;
    }
    bool word(const char* w) {
        space();
        std::string_view v(w);
        if (s.substr(at, v.size()) != v) return false;
        at += v.size();
        return true;
    }
    bool skip() { // any value
        if (peek('"')) return string(nullptr);
        if (peek('{')) return object([&](const std::string&) { return skip(); });
        if (peek('[')) {
            if (++depth > 32) return false;
            ++at;
            if (!eat(']')) {
                do { if (!skip()) return false; } while (eat(','));
                if (!eat(']')) return false;
            }
            --depth;
            return true;
        }
        if (word("true") || word("false") || word("null")) return true;
        return number(nullptr);
    }
    // Calls member(key) at each member's value; member reads (or skips) it.
    template <class Member>
    bool object(Member&& member) {
        if (++depth > 32 || !eat('{')) return false;
        if (!eat('}')) {
            do {
                std::string key;
                if (!string(&key) || !eat(':') || !member(key)) return false;
            } while (eat(','));
            if (!eat('}')) return false;
        }
        --depth;
        return true;
    }
};

// The voices of the map whose object starts at `from`; empty when it is not one.
inline std::vector<DrumVoice> read_map(std::string_view bytes, size_t from) {
    Reader r {bytes, from};
    std::vector<DrumVoice> voices;
    bool drums = false;
    bool ok = r.object([&](const std::string& key) {
        if (key == "pad_layout") {
            std::string layout;
            if (!r.string(&layout)) return false;
            drums = layout == "drums";
            return true;
        }
        if (key != "levels") return r.skip();
        return r.object([&](const std::string&) { // a level: a voice when it has a name and a note
            if (!r.peek('{')) return r.skip();
            std::string name;
            long note = -1;
            bool read = r.object([&](const std::string& field) {
                if (field == "name") return r.string(&name);
                if (field == "note" && r.number(&note)) return true;
                return r.skip();
            });
            if (read && !name.empty() && note >= 0 && note <= 127) voices.push_back({uint8_t(note), name});
            return read;
        });
    });
    return ok && drums ? voices : std::vector<DrumVoice> {};
}
} // namespace drum_map_detail

// Every drum map in `bytes` (a plug-in's library), in the order they lie there; maps without a voice are left out.
inline std::vector<std::vector<DrumVoice>> drum_maps_in(std::string_view bytes) {
    std::vector<std::vector<DrumVoice>> maps;
    constexpr std::string_view mark = "\"pad_layout\"";
    for (size_t at = bytes.find(mark); at != std::string_view::npos; at = bytes.find(mark, at + mark.size())) {
        size_t from = at; // its object's brace, before it with only spaces between
        while (from > 0 && (bytes[from - 1] == ' ' || bytes[from - 1] == '\n' || bytes[from - 1] == '\r' || bytes[from - 1] == '\t')) --from;
        if (from == 0 || bytes[from - 1] != '{') continue;
        auto voices = drum_map_detail::read_map(bytes, from - 1);
        if (!voices.empty()) maps.push_back(std::move(voices));
    }
    return maps;
}

// The map Drums pads play, the plug-ins' own default (their drum-rack map, 36 up, over GM): the one with the most
// voices on bank A (keys 36-51), of those the one spanning the fewest keys, then the first; empty for a plug-in that
// states none. 8W8: Rack 36 has all 16 on bank A, GM 10. 9W9: both have its 11 there, Rack's in 36-46.
inline std::vector<DrumVoice> drum_voices_in(std::string_view bytes) {
    auto maps = drum_maps_in(bytes);
    size_t best = 0;
    int best_score = -1, best_span = 1000;
    for (size_t m = 0; m < maps.size(); ++m) {
        int score = 0, low = 127, high = 0;
        for (const auto& v : maps[m]) {
            score += v.key >= 36 && v.key <= 51;
            low = std::min<int>(low, v.key); high = std::max<int>(high, v.key);
        }
        if (score > best_score || (score == best_score && high - low < best_span)) { best = m; best_score = score; best_span = high - low; }
    }
    return maps.empty() ? std::vector<DrumVoice> {} : maps[best];
}

} // namespace melodee
