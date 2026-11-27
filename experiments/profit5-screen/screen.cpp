// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "screen.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
constexpr uint32_t background = 0x171918, panel = 0x222522, line_colour = 0x393d36;
constexpr uint32_t ink = 0xf0e6cd, muted = 0xb0b0a0, amber = 0xefba65, dark = 0x11130f;
constexpr float pi = 3.14159265359f;
struct Glyph { unsigned offset; int width; };
struct Font { const Glyph* glyphs; const unsigned char* pixels; int height; };
#include "fonts.h"

uint32_t blend(uint32_t a, uint32_t b, unsigned alpha) {
    uint32_t result = 0;
    for (int shift = 0; shift < 24; shift += 8)
        result |= (((a >> shift & 255) * (255 - alpha) + (b >> shift & 255) * alpha + 127) / 255) << shift;
    return result;
}
struct Canvas {
    const melodee_screen_frame_t& f;
    void dot(int x, int y, uint32_t colour, unsigned alpha = 255) {
        if (x < 0 || y < 0 || x >= int(f.width) || y >= int(f.height)) return;
        auto& pixel = f.pixels[y * f.stride + x];
        pixel = alpha == 255 ? colour : blend(pixel, colour, alpha);
    }
    void box(int x, int y, int w, int h, uint32_t colour) {
        for (int yy = std::max(0, y); yy < std::min(int(f.height), y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(int(f.width), x + w); ++xx) dot(xx, yy, colour);
    }
    void line(float x0, float y0, float x1, float y1, uint32_t colour) {
        int steps = std::max(1, int(std::ceil(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)))));
        for (int i = 0; i <= steps; ++i) {
            float t = float(i) / steps;
            dot(int(std::lround(x0 + t * (x1 - x0))), int(std::lround(y0 + t * (y1 - y0))), colour);
        }
    }
    void disc(float cx, float cy, float radius, uint32_t colour) {
        for (int y = int(cy - radius - 1); y <= int(cy + radius + 1); ++y)
            for (int x = int(cx - radius - 1); x <= int(cx + radius + 1); ++x) {
                float coverage = std::clamp(radius + 0.5f - std::hypot(x - cx, y - cy), 0.0f, 1.0f);
                if (coverage > 0) dot(x, y, colour, unsigned(255 * coverage));
            }
    }
    int width(const char* text, const Font& font = small_font) const {
        int w = 0;
        for (; *text; ++text) if (*text >= 32 && *text < 127) w += font.glyphs[*text - 32].width;
        return w;
    }
    void text(int x, int y, const char* text, uint32_t colour, const Font& font = small_font, int max_width = 320) {
        const int end = std::min(int(f.width), x + max_width);
        for (; *text; ++text) {
            unsigned code = static_cast<unsigned char>(*text);
            if (code < 32 || code > 126) continue;
            const auto& glyph = font.glyphs[code - 32];
            if (x + glyph.width > end) break;
            for (int yy = 0; yy < font.height; ++yy)
                for (int xx = 0; xx < glyph.width; ++xx) {
                    unsigned alpha = font.pixels[glyph.offset + yy * glyph.width + xx];
                    if (alpha) dot(x + xx, y + yy, colour, alpha);
                }
            x += glyph.width;
        }
    }
    void centre(int x, int y, const char* s, uint32_t colour, const Font& font = small_font) {
        text(x - width(s, font) / 2, y, s, colour, font);
    }
};

struct Row { int group; const char* name; int ids[3]; const char* labels[3]; };
constexpr Row rows[] {
    {0, "OSCILLATOR A", {2, 3, 10}, {"Pitch", "Pulse width", "Level"}},
    {0, "OSC A / SHAPES", {27, 28, 29}, {"Saw", "Pulse", "Hard sync"}},
    {0, "OSCILLATOR B", {6, 7, 8}, {"Pitch", "Fine", "Pulse width"}},
    {0, "OSC B / SHAPES", {38, 39, 40}, {"Saw", "Triangle", "Pulse"}},
    {0, "OSC B / RANGE", {41, 42, 11}, {"Low mode", "Keyboard", "Level"}},
    {0, "SOURCE MIXER", {10, 11, 12}, {"Osc A", "Osc B", "Noise"}},
    {1, "LOW-PASS FILTER", {13, 14, 15}, {"Cutoff", "Resonance", "Env amount"}},
    {1, "KEYBOARD TRACKING", {43, 57, 48}, {"Tracking", "Track range", "Velocity"}},
    {2, "FILTER ENVELOPE", {16, 17, 18}, {"Attack", "Decay", "Sustain"}},
    {2, "FILTER / RELEASE", {19, 44, 15}, {"Release", "Release on", "Env amount"}},
    {2, "AMPLIFIER ENVELOPE", {20, 21, 22}, {"Attack", "Decay", "Sustain"}},
    {2, "AMP / RELEASE", {23, 44, 49}, {"Release", "Release on", "Velocity"}},
    {3, "POLY-MOD / SOURCES", {0, 1, 26}, {"Filter env", "Osc B", "To filter"}},
    {3, "POLY-MOD / ROUTING", {24, 25, 26}, {"To A pitch", "To A width", "To filter"}},
    {3, "WHEEL-MOD / SOURCES", {4, 5, 56}, {"LFO rate", "LFO / noise", "Initial amt"}},
    {3, "LFO / SHAPES", {30, 31, 32}, {"Saw", "Triangle", "Square"}},
    {3, "WHEEL-MOD / PITCH", {33, 34, 37}, {"To A pitch", "To B pitch", "To filter"}},
    {3, "WHEEL-MOD / WIDTH", {35, 36, 37}, {"To A width", "To B width", "To filter"}},
    {4, "VOICE / CHARACTER", {46, 47, 55}, {"Revision", "Vintage", "Quality"}},
    {4, "VOICE / PERFORMANCE", {9, 45, 54}, {"Glide", "Unison", "Voices"}},
    {4, "VOICE / PRESSURE", {50, 51, 52}, {"To filter", "To LFO", "Master tune"}},
    {5, "PROGRAM / OUTPUT", {58, 53, 54}, {"Program", "Volume", "Voices"}},
};
constexpr int row_count = int(sizeof(rows) / sizeof(rows[0]));
constexpr const char* tabs[] {"OSC", "FILTER", "ENV", "MOD", "VOICE", "PATCH"};

struct Snapshot {
    std::array<float, 59> values {};
    char text[3][48] {};
    char program[64] {};
    int row = -1, focus = -1;
    bool editing = false;
    int16_t pots[3] {-2, -2, -2};
    uint8_t live[3] {2, 2, 2};
    uint32_t area = 99, width = 0, height = 0;
};
struct Screen {
    profit5_ui_host host;
    int row = 0, focus = 0;
    bool editing = false, valid = false;
    Snapshot drawn;
    explicit Screen(profit5_ui_host h) : host(h) {}
};

void rotary(Canvas& g, int x, int y, float value, int pot, bool live, bool selected, bool editing) {
    constexpr float start = -0.75f * pi, span = 1.5f * pi;
    for (int i = 0; i <= 60; ++i) {
        float a = start + span * i / 60.0f;
        uint32_t c = i <= int(value * 60) ? amber : line_colour;
        g.disc(x + std::sin(a) * 22, y - std::cos(a) * 22, 1.0f, c);
    }
    g.disc(x, y + 1, 17, dark);
    g.disc(x, y - 1, 16, panel);
    const float angle = start + span * value;
    g.line(x + std::sin(angle) * 5, y - std::cos(angle) * 5, x + std::sin(angle) * 12, y - std::cos(angle) * 12, ink);
    if (pot >= 0 && !live) {
        const float at = start + span * pot / 127;
        g.disc(x + std::sin(at) * 28, y - std::cos(at) * 28, 2.0f, ink);
    }
    if (selected) {
        g.box(x - 23, y + 28, 46, 2, editing ? amber : muted);
        if (editing) g.disc(x + 29, y, 2.5f, amber);
    }
}

void toggle(Canvas& g, int x, int y, float value, int pot, bool live, bool selected, bool editing) {
    g.box(x - 29, y - 13, 58, 27, line_colour);
    g.box(x - 28, y - 12, 56, 25, dark);
    const int cap = value >= 0.5f ? x + 3 : x - 25;
    g.box(cap, y - 9, 23, 19, panel);
    g.box(cap + 2, y - 8, 19, 2, amber);
    for (int i = 0; i < 3; ++i) g.box(cap + 7 + i * 4, y - 3, 1, 8, muted);
    if (pot >= 0 && !live) g.disc(x - 27 + 54 * pot / 127.0f, y + 20, 2.0f, ink);
    if (selected) {
        g.box(x - 23, y + 28, 46, 2, editing ? amber : muted);
        if (editing) g.disc(x + 34, y, 2.5f, amber);
    }
}

void plot(Canvas& g, const Screen& s, const Snapshot& v, int y) {
    const int group = rows[s.row].group;
    const auto value = [&](int id) { return v.values[id]; };
    g.box(12, y, 296, 28, dark);
    for (int x = 28; x < 308; x += 28) g.box(x, y + 3, 1, 22, 0x242922);
    g.box(12, y + 23, 296, 1, line_colour);
    float px = 14, py = float(y + 23);
    if (group == 2) {
        const int a = s.row >= 10 ? 20 : 16;
        const float attack = 12 + 65 * value(a), decay = 12 + 65 * value(a + 1), release = 12 + 65 * value(a + 3);
        const float sustain = y + 23 - 19 * value(a + 2);
        const float end = 306 - release;
        g.line(14, y + 23, 14 + attack, y + 4, amber);
        g.line(14 + attack, y + 4, 14 + attack + decay, sustain, amber);
        g.line(14 + attack + decay, sustain, end, sustain, amber);
        g.line(end, sustain, 306, y + 23, amber);
    } else if (group == 1) {
        // Parameter-position diagram, deliberately not a measured frequency response.
        const float cutoff = 24 + 245 * value(13), resonance = value(14);
        for (int x = 14; x <= 306; ++x) {
            float u = (x - cutoff) / 14;
            float response = 1 / (1 + std::exp(std::clamp(u, -20.0f, 20.0f)));
            float peak = resonance * std::exp(-0.5f * (u + 0.8f) * (u + 0.8f));
            float yy = y + 23 - 13 * response - 7 * peak;
            if (x > 14) g.line(px, py, float(x), yy, amber);
            px = float(x); py = yy;
        }
    } else if (group == 3 && s.row < 14) {
        g.text(24, y + 5, "ENV + OSC B", amber);
        g.line(132, y + 14, 184, y + 14, muted);
        g.line(179, y + 10, 184, y + 14, muted);
        g.line(179, y + 18, 184, y + 14, muted);
        g.text(198, y + 5, "A / FILTER", ink);
    } else if (group == 0 || group == 3) {
        const bool b = s.row >= 2 && s.row <= 4;
        const bool lfo = group == 3 && s.row >= 14;
        int saw = lfo ? 30 : b ? 38 : 27, pulse = lfo ? 32 : b ? 40 : 28, tri = lfo ? 31 : b ? 39 : -1;
        const float pw = lfo ? 0.5f : 0.05f + value(b ? 8 : 3) * 0.9f;
        const float count = (value(saw) >= 0.5f) + (value(pulse) >= 0.5f) + (tri >= 0 && value(tri) >= 0.5f);
        for (int x = 14; x <= 306; ++x) {
            float phase = std::fmod((x - 14) / 97.0f, 1.0f), wave = 0;
            if (value(saw) >= 0.5f) wave += 1 - 2 * phase;
            if (value(pulse) >= 0.5f) wave += phase < pw ? 1 : -1;
            if (tri >= 0 && value(tri) >= 0.5f) wave += 1 - 4 * std::fabs(phase - 0.5f);
            float yy = y + 14 - 9 * wave / std::max(1.0f, count);
            if (x > 14) g.line(px, py, float(x), yy, amber);
            px = float(x); py = yy;
        }
    } else {
        const int voices = 1 + int(std::lround(value(54) * 4));
        for (int n = 0; n < 5; ++n) {
            const int x = 22 + n * 58;
            g.box(x, y + 7, 44, 12, n < voices ? amber : line_colour);
            for (int k = 0; k < 4; ++k) g.box(x + 5 + k * 9, y + 7, 1, 12, dark);
        }
    }
}

bool same(const Snapshot& a, const Snapshot& b) {
    return a.values == b.values && a.row == b.row && a.focus == b.focus && a.editing == b.editing
        && a.area == b.area && a.width == b.width && a.height == b.height
        && !std::memcmp(a.pots, b.pots, sizeof(a.pots)) && !std::memcmp(a.live, b.live, sizeof(a.live))
        && !std::memcmp(a.text, b.text, sizeof(a.text)) && !std::strcmp(a.program, b.program);
}
} // namespace

extern "C" void* profit5_ui_create(profit5_ui_host host) { return new (std::nothrow) Screen(host); }
extern "C" void profit5_ui_destroy(void* ui) { delete static_cast<Screen*>(ui); }
extern "C" void profit5_ui_controls(void* ui, uint32_t ids[3]) {
    auto& s = *static_cast<Screen*>(ui);
    for (int k = 0; k < 3; ++k) ids[k] = uint32_t(rows[s.row].ids[k]);
}
extern "C" bool profit5_ui_input(void* ui, const melodee_screen_input_t* in) {
    auto& s = *static_cast<Screen*>(ui);
    if (in->kind == MELODEE_SCREEN_STEP) {
        const int direction = in->value < 0 ? -1 : 1;
        if (in->shift) {
            const int group = (rows[s.row].group + direction + 6) % 6;
            for (int i = 0; i < row_count; ++i) if (rows[i].group == group) { s.row = i; break; }
        } else s.row = (s.row + direction + row_count) % row_count;
        s.editing = false;
    } else if (in->kind == MELODEE_SCREEN_PUSH) {
        s.editing = !s.editing;
    } else if (in->kind == MELODEE_SCREEN_WHEEL) {
        if (!s.editing) s.focus = int(std::clamp(int64_t(s.focus) + in->value, int64_t(0), int64_t(2)));
        else {
            const int id = rows[s.row].ids[s.focus], steps = s.host.steps(s.host.instance, id);
            const float step = steps ? 1.0f / steps : in->shift ? 0.001f : 0.01f;
            s.host.set(s.host.instance, id, std::clamp(s.host.get(s.host.instance, id) + step * in->value, 0.0f, 1.0f));
        }
    } else return false;
    return true;
}
extern "C" bool profit5_ui_draw(void* ui, const melodee_screen_frame_t* f) {
    if (!ui || !f || !f->pixels || f->stride < f->width || f->width != 320 || f->height < 144) return false;
    auto& s = *static_cast<Screen*>(ui);
    Snapshot v;
    for (int id = 0; id < 59; ++id) {
        float value = s.host.get(s.host.instance, id);
        v.values[id] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }
    for (int k = 0; k < 3; ++k) {
        s.host.text(s.host.instance, rows[s.row].ids[k], v.text[k], int(sizeof(v.text[k])));
        const int id = rows[s.row].ids[k];
        if (id == 2 || id == 6 || id == 52) {
            const size_t len = std::strlen(v.text[k]);
            std::snprintf(v.text[k] + len, sizeof(v.text[k]) - len, "%s", id == 52 ? " ct" : " st");
        }
        if (id == 58) std::snprintf(v.text[k], sizeof(v.text[k]), "%03d", 1 + int(std::lround(v.values[58] * 127)));
        v.pots[k] = f->pots[k]; v.live[k] = f->pots_live[k];
    }
    s.host.program(s.host.instance, v.program, int(sizeof(v.program)));
    v.row = s.row; v.focus = s.focus; v.editing = s.editing;
    v.area = f->area; v.width = f->width; v.height = f->height;
    if (s.valid && !f->redraw_all && same(v, s.drawn)) return false;
    Canvas g {*f};
    const auto& row = rows[s.row];
    const bool full = !s.valid || f->redraw_all || s.drawn.row != s.row || s.drawn.area != f->area
                   || s.drawn.height != f->height;
    // Header and graph are repainted only when their contents change. The three controls share a small region.
    if (full) g.box(0, 0, 320, int(f->height), background);
    if (full || std::strcmp(v.program, s.drawn.program) || v.values[46] != s.drawn.values[46] || v.values[54] != s.drawn.values[54]) {
        g.box(0, 0, 320, 51, background);
        g.text(12, 2, "PROFIT-5", ink, title_font);
        g.box(254, 9, 54, 20, amber);
        g.centre(281, 10, v.values[46] >= 0.5f ? "REV 4" : "REV 2", dark);
        g.text(13, 31, v.program, muted, small_font, 225);
        for (int n = 0; n < 5; ++n) g.disc(261 + n * 10, 40, 2, n <= int(std::lround(v.values[54] * 4)) ? amber : line_colour);
    }
    if (full) {
        for (int tab = 0; tab < 6; ++tab) {
            int x = 8 + tab * 51;
            g.box(x, 54, 49, 21, row.group == tab ? amber : panel);
            g.centre(x + 24, 56, tabs[tab], row.group == tab ? dark : muted);
        }
        if (f->height >= 218) {
            g.text(12, 80, row.name, ink);
            int index = 0, count = 0;
            for (int n = 0; n < row_count; ++n) if (rows[n].group == row.group) { ++count; if (n <= s.row) ++index; }
            char position[32]; std::snprintf(position, sizeof(position), "%d / %d", index, count);
            g.text(308 - g.width(position), 80, position, muted);
        }
    }
    if (f->height >= 218 && (full || v.values != s.drawn.values)) plot(g, s, v, 101);
    const int cy = f->height >= 218 ? 164 : 109;
    const int label_y = cy - 33, value_y = cy + 29;
    g.box(0, label_y, 320, int(f->height) - label_y, background);
    for (int k = 0; k < 3; ++k) {
        const int x = 57 + k * 103;
        if (k) g.box(x - 52, label_y + 3, 1, int(f->height) - label_y - 13, line_colour);
        g.centre(x, label_y, row.labels[k], s.focus == k ? ink : muted);
        // Compact strip is supported too; instruments normally use the full page.
        if (f->height >= 218) {
            auto control = s.host.steps(s.host.instance, row.ids[k]) == 1 ? toggle : rotary;
            control(g, x, cy, v.values[row.ids[k]], f->pots[k], f->pots_live[k], k == s.focus, s.editing);
        }
        else {
            g.box(x - 39, cy + 10, 78, 2, line_colour);
            g.box(x - 39, cy + 10, int(78 * v.values[row.ids[k]]), 2, amber);
            if (!f->pots_live[k] && f->pots[k] >= 0) g.disc(x - 39 + 78 * f->pots[k] / 127.0f, cy + 10, 2, ink);
        }
        g.centre(x, f->height >= 218 ? value_y : cy - 13, v.text[k], amber, value_font);
    }
    s.drawn = v; s.valid = true;
    return true;
}
