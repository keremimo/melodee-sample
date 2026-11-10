// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// The Name sheet's text and how the controls change it, as plain data and
// functions (the shell routes the controls here; the page draws it).
// Pads type like phone keys: thirteen pads hold two letters each, a second
// tap within 0.6 s turns the letter just typed into the pad's other one.
// Pad 14 is a space, pad 15 switches to digits and signs, pad 16 deletes.
// Only characters a card takes in a file name are offered.
namespace melodee {

enum class NameTarget : uint8_t { None, ProjectSave, ProjectSaveAs, ProjectRename, Marker, Sound, Sample };

struct NameEntry {
    static constexpr unsigned capacity = 32;
    static constexpr uint32_t retap_ms = 600;
    static constexpr const char* charset = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.()&'";
    static constexpr const char* letters[13] {"AB", "CD", "EF", "GH", "IJ", "KL", "MN", "OP", "QR", "ST", "UV", "WX", "YZ"};
    static constexpr const char* signs[13] {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_", "."};

    NameTarget target = NameTarget::None;
    char text[capacity] {};
    uint8_t length = 0, cursor = 0, limit = capacity - 1;
    bool caps_only = false;  // markers: capitals, as the boards draw them
    bool lower = false;      // Aa
    bool digits = false;     // pad 15: the digits and signs
    int8_t last_pad = -1;    // the pad that typed the character before the cursor
    uint8_t taps = 0;
    uint32_t last_at = 0;
    uint8_t suggestion = 0;  // K1's place in the ready names
    char first_suggestion[capacity] {}; // the ready name made for this target (a free project name, a sound's)

    bool open() const { return target != NameTarget::None; }

    void start(NameTarget t, const char* initial, unsigned max_length, bool capitals) {
        *this = NameEntry {};
        target = t; limit = uint8_t(std::min<unsigned>(max_length, capacity - 1)); caps_only = capitals;
        set(initial);
        std::memcpy(first_suggestion, text, sizeof(text)); // text is always zero-terminated within capacity
    }
    void set(const char* value) {
        std::memset(text, 0, sizeof(text));
        length = 0;
        for (const char* c = value; *c && length < limit; ++c) if (allowed(*c)) text[length++] = caps_only ? upper(*c) : *c;
        cursor = length; last_pad = -1;
    }
    static bool allowed(char c) { return c && std::strchr(charset, c) != nullptr; }
    static char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }
    static char lower_of(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

    bool insert(char c) {
        if (length >= limit || !allowed(c)) return false;
        std::memmove(text + cursor + 1, text + cursor, length - cursor);
        text[cursor++] = c; ++length;
        return true;
    }
    void erase() { // the character before the cursor
        if (!cursor) return;
        std::memmove(text + cursor - 1, text + cursor, length - cursor);
        --cursor; --length; text[length] = 0;
        last_pad = -1;
    }
    void clear() { std::memset(text, 0, sizeof(text)); length = cursor = 0; last_pad = -1; }
    void move(int way) { cursor = uint8_t(std::clamp(int(cursor) + way, 0, int(length))); last_pad = -1; }

    // What a pad types, in the case in force; 0 for pads 14-16.
    char pad_char(unsigned pad, unsigned which) const {
        if (pad >= 13) return 0;
        const char* group = digits ? signs[pad] : letters[pad];
        char c = group[std::min<unsigned>(which, unsigned(std::strlen(group)) - 1)];
        return lower && !caps_only && !digits ? lower_of(c) : c;
    }
    unsigned pad_choices(unsigned pad) const { return pad < 13 ? unsigned(std::strlen(digits ? signs[pad] : letters[pad])) : 0; }

    void pad(unsigned pad, uint32_t now) {
        if (pad == 13) { insert(' '); last_pad = -1; return; }
        if (pad == 14) { digits = !digits; last_pad = -1; return; }
        if (pad == 15) { erase(); return; }
        if (pad >= 13) return;
        unsigned choices = pad_choices(pad);
        if (last_pad == int(pad) && now - last_at < retap_ms && choices > 1 && cursor > 0) { // again: the pad's next letter
            taps = uint8_t((taps + 1) % choices);
            text[cursor - 1] = pad_char(pad, taps);
        } else {
            taps = 0;
            if (!insert(pad_char(pad, 0))) return;
        }
        last_pad = int8_t(pad); last_at = now;
    }

    // K2 or Shift + wheel: the character under the cursor (before it at the end) through the whole set.
    unsigned char_at() const {
        unsigned at = cursor < length ? cursor : (cursor ? cursor - 1u : 0u);
        if (!length) return 0;
        const char* p = std::strchr(charset, text[at]);
        return p ? unsigned(p - charset) : 0;
    }
    static unsigned charset_size() { return unsigned(std::strlen(charset)); }
    void set_char(unsigned index) {
        char c = charset[index % charset_size()];
        if (caps_only) c = upper(c);
        if (!length) { insert(c); return; }
        unsigned at = cursor < length ? cursor : cursor - 1u;
        text[at] = c;
        last_pad = -1;
    }
    void step_char(int way) {
        unsigned n = charset_size();
        set_char((char_at() + n + unsigned(way % int(n) + int(n))) % n);
    }
};

} // namespace melodee
