// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// MPC plug-in presets (.xpl files, as an MPC plug-in folder ships them in Presets/): an XML <pluginstate> with the
// plug-in it is for (<PLUGIN ... uid="4f785a79"/>, the VST2 unique id in hex), the preset's name (<preset>) and the
// plug-in's state, either <state> in JUCE's base64 ("731.CMlaKA...": the byte count, a dot, six bits a character,
// lowest first) as MPC OS writes it, or <state64> in standard base64 (the MPC plug-in SDK's docs). The state is
// what JUCE's VST2 host saves: a VST2 bank or program file (fxb / fxp, "CcnK"), holding the plug-in's chunk or its
// parameter values; a state without that header is taken as the chunk itself.
//
// xpl_preset_in() turns it into Melodee's own VST2 state (vst2::state in plugin/vst2_abi.h), which both VST2 hosts
// (Vst2Instance, Vst2BridgeInstance) take with setStateInformation(). Plain C++, no JUCE: the host tests read it too.
#pragma once
#include "melodee/plugin/vst2_abi.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace melodee {

namespace xpl_detail {
inline uint32_t big_endian(const uint8_t* at) {
    return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | uint32_t(at[3]);
}
inline bool magic(const uint8_t* at, const char* word) { return std::memcmp(at, word, 4) == 0; }

// The text between <tag> and </tag>, "" none.
inline std::string_view element(std::string_view xml, std::string_view tag) {
    std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
    auto from = xml.find(open);
    if (from == std::string_view::npos) return {};
    from += open.size();
    auto to = xml.find(close, from);
    return to == std::string_view::npos ? std::string_view {} : xml.substr(from, to - from);
}

// XML's five entities and numeric ones (ASCII only), as a name may have them.
inline std::string unescape(std::string_view text) {
    std::string out;
    for (size_t n = 0; n < text.size(); ++n) {
        if (text[n] != '&') { out += text[n]; continue; }
        auto end = text.find(';', n);
        if (end == std::string_view::npos || end - n > 8) { out += '&'; continue; }
        auto entity = text.substr(n + 1, end - n - 1);
        char c = 0;
        if (entity == "amp") c = '&';
        else if (entity == "lt") c = '<';
        else if (entity == "gt") c = '>';
        else if (entity == "quot") c = '"';
        else if (entity == "apos") c = '\'';
        else if (entity.size() > 1 && entity[0] == '#') {
            unsigned value = 0;
            bool hex = entity[1] == 'x' || entity[1] == 'X';
            for (size_t k = hex ? 2 : 1; k < entity.size(); ++k) {
                char d = entity[k];
                int digit = d >= '0' && d <= '9' ? d - '0' : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10 : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                if (digit < 0) { value = 0; break; }
                value = value * (hex ? 16 : 10) + unsigned(digit);
            }
            c = value > 0 && value < 128 ? char(value) : '?';
        }
        if (!c) { out += '&'; continue; }
        out += c;
        n = end;
    }
    return out;
}

// The value of `name="..."` inside the first <PLUGIN ...> tag, "" none.
inline std::string_view plugin_attribute(std::string_view xml, std::string_view name) {
    auto tag = xml.find("<PLUGIN");
    if (tag == std::string_view::npos) return {};
    auto end = xml.find('>', tag);
    auto inside = xml.substr(tag, end == std::string_view::npos ? std::string_view::npos : end - tag);
    std::string key = " " + std::string(name) + "=\"";
    for (size_t at = inside.find(key); at != std::string_view::npos; at = inside.find(key, at + 1)) {
        auto from = at + key.size(), to = inside.find('"', from);
        if (to != std::string_view::npos) return inside.substr(from, to - from);
    }
    return {};
}
} // namespace xpl_detail

// JUCE's MemoryBlock::toBase64Encoding() undone: characters outside its alphabet are passed over, as JUCE does.
// Empty when there is no byte count before a dot. `most`: no more bytes than that.
inline std::vector<uint8_t> juce_base64_decode(std::string_view text, size_t most = SIZE_MAX) {
    static constexpr std::string_view alphabet = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    auto dot = text.find('.');
    size_t size = 0;
    for (size_t n = 0; n < dot && n < text.size(); ++n) {
        char c = text[n];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c < '0' || c > '9' || size > (64u << 20)) return {};
        size = size * 10 + size_t(c - '0');
    }
    if (dot == std::string_view::npos || size == 0) return {};
    size = std::min(size, most);
    std::vector<uint8_t> out(size, 0);
    size_t bit = 0;
    for (size_t n = dot + 1; n < text.size() && bit < size * 8; ++n) {
        auto value = alphabet.find(text[n]);
        if (value == std::string_view::npos) continue;
        for (unsigned b = 0; b < 6 && bit < size * 8; ++b, ++bit)
            if (value >> b & 1) out[bit >> 3] = uint8_t(out[bit >> 3] | 1u << (bit & 7));
    }
    return out;
}

// Standard base64 (RFC 4648), spaces and line breaks passed over; empty on any other character. `most`: no more
// bytes than that.
inline std::vector<uint8_t> base64_decode(std::string_view text, size_t most = SIZE_MAX) {
    std::vector<uint8_t> out;
    uint32_t bits = 0;
    unsigned count = 0;
    for (char c : text) {
        if (out.size() >= most) break;
        int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                  : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (value < 0) return {};
        bits = bits << 6 | uint32_t(value);
        if ((count += 6) >= 8) { count -= 8; out.push_back(uint8_t(bits >> count)); }
    }
    return out;
}

// A VST2 bank or program file (fxb / fxp) as Melodee's VST2 state: its chunk ("FBCh" a bank's, "FPCh" a program's),
// or its parameter values ("FxCk"; of a bank, "FxBk", the program it was on). Anything without "CcnK" first is taken
// as a bank chunk. `fx_id`: the plug-in id it names (0 none). Empty when it is cut short or of no kind here.
inline std::vector<uint8_t> vst2_state_from_fx(const std::vector<uint8_t>& fx, uint32_t* fx_id = nullptr) {
    using namespace xpl_detail;
    std::vector<uint8_t> out(vst2::state::tag, vst2::state::tag + 4);
    if (fx_id) *fx_id = 0;
    const uint8_t* d = fx.data();
    const size_t size = fx.size();
    if (size < 8 || !magic(d, "CcnK")) {
        if (fx.empty()) return {};
        out.push_back(vst2::state::chunk);
        out.insert(out.end(), fx.begin(), fx.end());
        return out;
    }
    if (size < 28) return {};
    if (fx_id) *fx_id = big_endian(d + 16);
    // A program's values: count at 24, its name (28 bytes) at 28, the values from 56, big-endian floats.
    auto program_values = [&](size_t at) {
        if (at + 56 > size || !magic(d + at, "CcnK") || !magic(d + at + 8, "FxCk")) return false;
        uint32_t count = big_endian(d + at + 24);
        if (count > (size - at - 56) / 4) return false;
        out.push_back(vst2::state::parameters);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t word = big_endian(d + at + 56 + i * 4);
            float value;
            std::memcpy(&value, &word, sizeof(value));
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        }
        return true;
    };
    auto chunk_at = [&](size_t at, uint8_t kind) { // its size, then the chunk
        if (at + 4 > size) return false;
        uint32_t length = big_endian(d + at);
        if (length == 0 || length > size - at - 4) return false;
        out.push_back(kind);
        out.insert(out.end(), d + at + 4, d + at + 4 + length);
        return true;
    };
    bool ok = false;
    if (magic(d + 8, "FxCk")) ok = program_values(0);
    else if (magic(d + 8, "FPCh")) ok = chunk_at(56, vst2::state::programChunk);
    else if (magic(d + 8, "FBCh")) ok = chunk_at(156, vst2::state::chunk);
    else if (magic(d + 8, "FxBk")) { // its programs from 156, each a program file; version 2 says which it was on
        uint32_t programs = big_endian(d + 24), version = big_endian(d + 12);
        uint32_t current = version >= 2 && size >= 32 ? big_endian(d + 28) : 0;
        if (current >= programs) current = 0;
        if (size >= 156 + 56) {
            uint32_t each = 56 + 4 * big_endian(d + 156 + 24);
            if (programs > 0 && each <= (size - 156) / programs) ok = program_values(156 + size_t(current) * each);
        }
    }
    return ok ? out : std::vector<uint8_t> {};
}

// Whether a plug-in's own program named `program` is the preset named `name`: the same name (case and outer spaces
// aside), or the same cut to the 23 characters a VST2 program name holds (OxZygen's "SYN Analog Brass Section" is
// its program "SYN Analog Brass Sectio").
inline bool same_preset_name(std::string_view program, std::string_view name) {
    auto trim = [](std::string_view s) {
        size_t first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
        return first == std::string_view::npos ? std::string_view {} : s.substr(first, last - first + 1);
    };
    program = trim(program);
    name = trim(name);
    if (program.empty() || program.size() > name.size() || (program.size() < name.size() && program.size() < 23)) return false;
    for (size_t n = 0; n < program.size(); ++n)
        if (std::tolower(static_cast<unsigned char>(program[n])) != std::tolower(static_cast<unsigned char>(name[n]))) return false;
    return true;
}

// One .xpl as Melodee reads it.
struct XplPreset {
    std::string name;            // <preset>, "" none (the file's name stands in)
    uint32_t uid = 0;            // <PLUGIN uid="...">, 0 not said
    uint32_t fx_id = 0;          // the plug-in id inside its state, 0 not said
    std::vector<uint8_t> state;  // Melodee's VST2 state; empty when not asked for, or none it can read
    // Whether it is for a plug-in of unique id `id`: neither id it names differs.
    bool for_plugin(int32_t id) const { return (uid == 0 || uid == uint32_t(id)) && (fx_id == 0 || fx_id == uint32_t(id)); }
};
// `with_state` false: its name and ids only (listing many).
inline XplPreset xpl_preset_in(std::string_view xml, bool with_state = true) {
    using namespace xpl_detail;
    XplPreset out;
    auto name = unescape(element(xml, "preset"));
    size_t first = name.find_first_not_of(" \t\r\n"), last = name.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) out.name = name.substr(first, last - first + 1);
    auto uid = plugin_attribute(xml, "uid");
    for (char c : uid) {
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0 || out.uid > 0x0fffffffu) { out.uid = 0; break; }
        out.uid = out.uid << 4 | uint32_t(digit);
    }
    // The state's plug-in id (it may be the only id): from its first 20 bytes when the rest is not asked for.
    auto juce_text = element(xml, "state");
    size_t most = with_state ? SIZE_MAX : 20;
    auto bytes = juce_text.empty() ? base64_decode(element(xml, "state64"), most) : juce_base64_decode(juce_text, most);
    if (with_state) out.state = vst2_state_from_fx(bytes, &out.fx_id);
    else if (bytes.size() >= 20 && xpl_detail::magic(bytes.data(), "CcnK")) out.fx_id = xpl_detail::big_endian(bytes.data() + 16);
    return out;
}

} // namespace melodee
