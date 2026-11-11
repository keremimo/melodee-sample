// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/glyphs.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace melodee::ui {
namespace {

// An SVG elliptical arc from (x1, y1) to (x2, y2), unrotated, added as JUCE's
// centred arc (the SVG spec's endpoint-to-centre conversion, appendix B.2.4).
void arc_to(juce::Path& path, float x1, float y1, float rx, float ry, bool large, bool sweep, float x2, float y2) {
    if (rx == 0 || ry == 0) { path.lineTo(x2, y2); return; }
    rx = std::abs(rx); ry = std::abs(ry);
    double xp = (x1 - x2) / 2.0, yp = (y1 - y2) / 2.0;
    double lambda = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry);
    if (lambda > 1) { rx *= float(std::sqrt(lambda)); ry *= float(std::sqrt(lambda)); }
    double rx2 = double(rx) * rx, ry2 = double(ry) * ry;
    double num = rx2 * ry2 - rx2 * yp * yp - ry2 * xp * xp, den = rx2 * yp * yp + ry2 * xp * xp;
    double coef = (large != sweep ? 1.0 : -1.0) * std::sqrt(std::max(0.0, den > 0 ? num / den : 0.0));
    double cxp = coef * rx * yp / ry, cyp = -coef * ry * xp / rx;
    double cx = cxp + (x1 + x2) / 2.0, cy = cyp + (y1 + y2) / 2.0;
    auto angle = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    double ux = (xp - cxp) / rx, uy = (yp - cyp) / ry, vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
    double theta = angle(1, 0, ux, uy), delta = angle(ux, uy, vx, vy);
    constexpr double tau = 2 * juce::MathConstants<double>::pi;
    if (!sweep && delta > 0) delta -= tau;
    else if (sweep && delta < 0) delta += tau;
    // SVG measures from the x axis, JUCE clockwise from 12 o'clock: a quarter turn apart.
    constexpr double quarter = juce::MathConstants<double>::halfPi;
    path.addCentredArc(float(cx), float(cy), rx, ry, 0, float(theta + quarter), float(theta + delta + quarter), false);
}

// The glyphs' shapes in path data: circles and rounded boxes as arcs.
std::string circle(float cx, float cy, float r) {
    auto n = [](float v) { return std::to_string(v); };
    return "M" + n(cx - r) + " " + n(cy) + " A" + n(r) + " " + n(r) + " 0 1 0 " + n(cx + r) + " " + n(cy) + " A" + n(r) + " " + n(r)
         + " 0 1 0 " + n(cx - r) + " " + n(cy) + " Z ";
}
std::string box(float x, float y, float w, float h, float r) {
    auto n = [](float v) { return std::to_string(v); };
    auto a = " A" + n(r) + " " + n(r) + " 0 0 1 ";
    return "M" + n(x + r) + " " + n(y) + " H" + n(x + w - r) + a + n(x + w) + " " + n(y + r) + " V" + n(y + h - r) + a + n(x + w - r) + " "
         + n(y + h) + " H" + n(x + r) + a + n(x) + " " + n(y + h - r) + " V" + n(y + r) + a + n(x + r) + " " + n(y) + " Z ";
}

struct Shape { juce::Path stroke, fill; };

Shape make(Icon icon) {
    std::string s, f; // stroked, filled
    switch (icon) {
    case Icon::None: case Icon::Count: break;
    case Icon::Play: f = "M7 4 L20 12 L7 20 Z"; break;
    case Icon::Stop: f = box(5, 5, 14, 14, 1.5f); break;
    case Icon::Rec: f = circle(12, 12, 7); break;
    case Icon::Back: s = "M9 4 L4 9 L9 14 M4 9 H14 A6 6 0 0 1 14 21 H10"; break;
    case Icon::Check: s = "M4 12.5 L9.5 18 L20 6"; break;
    case Icon::Cross: s = "M6 6 L18 18 M18 6 L6 18"; break;
    case Icon::Plus: s = "M12 4 V20 M4 12 H20"; break;
    case Icon::Shift: s = "M12 3 L21 12 H16 V20 H8 V12 H3 Z"; break;
    case Icon::Power: s = "M12 3 V11 M6.6 6.6 A7.6 7.6 0 1 0 17.4 6.6"; break;
    case Icon::Card: s = "M7 2 H16 L20 6 V22 H4 V5 Z M9 3.5 V7 M12 3.5 V7 M15 4 V7"; break;
    case Icon::Laptop: s = box(4, 4, 16, 11, 1.5f) + "M2 19 H22"; break;
    case Icon::Usb: s = "M12 2 V18 M8.5 5.5 L12 2 L15.5 5.5 M12 13 L7 10 V8 M12 15 L17 12 V10"; f = circle(12, 20, 2); break;
    case Icon::Eject: s = "M12 5 L19 13 H5 Z M5 18 H19"; break;
    case Icon::Mic: s = box(9, 3, 6, 11, 3) + "M5.5 11 A6.5 6.5 0 0 0 18.5 11 M12 17.5 V21 M8.5 21 H15.5"; break;
    case Icon::Jack: s = circle(12, 12, 8); f = circle(12, 12, 3); break;
    case Icon::Speaker: s = "M4 9 H8 L13 5 V19 L8 15 H4 Z M16.5 9 A4 4 0 0 1 16.5 15 M19 6.5 A8 8 0 0 1 19 17.5"; break;
    case Icon::SpeakerOff: s = "M4 9 H8 L13 5 V19 L8 15 H4 Z M16 9.5 L21 14.5 M21 9.5 L16 14.5"; break;
    case Icon::Loop: s = "M17 3 L20 6 L17 9 M20 6 H9 A5 5 0 0 0 4 11 M7 21 L4 18 L7 15 M4 18 H15 A5 5 0 0 0 20 13"; break;
    case Icon::OneShot: s = "M3 12 H17 M13 7 L18 12 L13 17 M21 5 V19"; break;
    case Icon::Reverse: s = "M21 12 H7 M11 7 L6 12 L11 17 M3 5 V19"; break;
    case Icon::Trash: s = "M4 7 H20 M9 7 V4 H15 V7 M6.5 7 L7.5 20 H16.5 L17.5 7"; break;
    case Icon::Undo: s = "M8 4 L3 9 L8 14 M3 9 H15 A5 5 0 0 1 15 19 H11"; break;
    case Icon::Folder: s = "M3 6 H10 L12 8 H21 V19 H3 Z"; break;
    case Icon::Plug: s = "M9 2 V7 M15 2 V7 M6 7 H18 V11 A6 6 0 0 1 6 11 Z M12 17 V22"; break;
    case Icon::PlugBroken: s = "M9 2 V7 M15 2 V7 M6 7 H18 V11 A6 6 0 0 1 6 11 Z M12 17 V22 M3 21 L21 3"; break;
    case Icon::Knob: s = circle(12, 12, 8) + "M12 12 L12 5"; break;
    case Icon::Lock: s = box(5, 11, 14, 10, 1.5f) + "M8 11 V8 A4 4 0 0 1 16 8 V11"; break;
    case Icon::Warn: s = "M12 3 L22 20 H2 Z M12 9.5 V13.5 M12 16.8 V17"; break;
    case Icon::Pads: s = box(4, 4, 7, 7, 1) + box(13, 4, 7, 7, 1) + box(4, 13, 7, 7, 1) + box(13, 13, 7, 7, 1); break;
    case Icon::Scissors: s = circle(6, 18, 3) + circle(18, 18, 3) + "M8 16 L19 3 M16 16 L5 3"; break;
    case Icon::Wav: s = "M6 2 H14 L19 7 V22 H6 Z M14 2 V7 H19 M9 14 V17 M11.5 11.5 V19 M14 13 V18 M16.5 14.5 V16.5"; break;
    case Icon::Pattern: s = box(3, 4, 18, 16, 2) + "M3 9.5 H21 M3 14.5 H21 M9 4 V20 M15 4 V20"; break;
    case Icon::Scale: s = "M3 20 H8 V15 H13 V10 H18 V5 H21"; break;
    case Icon::Chromatic: s = "M5 4 V20 M9.5 4 V20 M14 4 V20 M18.5 4 V20"; break;
    case Icon::Chord: f = circle(12, 5.5f, 2.5f) + circle(12, 12, 2.5f) + circle(12, 18.5f, 2.5f); break;
    case Icon::Velocity: s = "M3 20 L21 4 V20 Z"; break;
    case Icon::Fader: s = "M12 3 V21"; f = box(7.5f, 10, 9, 5, 1); break;
    case Icon::Ring: s = circle(12, 12, 7); break;
    case Icon::Push: s = circle(12, 12, 8.5f); f = circle(12, 12, 3.5f); break;
    case Icon::Mpc:
        s = box(3, 3, 18, 18, 2.5f) + "M6 7 H18";
        f = box(6, 11, 5, 3.5f, 0.6f) + box(13, 11, 5, 3.5f, 0.6f) + box(6, 16, 5, 3, 0.6f) + box(13, 16, 5, 3, 0.6f);
        break;
    case Icon::Note: s = "M9 18 V5 L19 3 V16"; f = circle(7, 18, 2.4f) + circle(17, 16, 2.4f); break;
    case Icon::Motion: s = "M3 17 L8 10 L12 13.5 L17 6 L21 9"; break;
    case Icon::ArrowUp: s = "M12 20 V5 M6 11 L12 5 L18 11"; break;
    case Icon::Space: s = "M3 19 C6 8 18 8 21 19 M7.5 19 C9 13.5 15 13.5 16.5 19"; break;
    case Icon::Time: f = circle(6, 12, 3); s = circle(13, 12, 2.2f) + circle(19, 12, 1.4f); break;
    case Icon::Modulation: s = "M2 12 C4.5 5 6.5 5 9 12 C11.5 19 13.5 19 16 12 C18.5 5 20.5 5 22 12"; break;
    case Icon::Dirt: s = "M3 20 C10 20 9 4 21 4"; break;
    case Icon::Tone: s = "M3 16 H8 L13 8 H21"; break;
    case Icon::Dynamics: s = "M3 21 L12 12 L21 8"; break;
    case Icon::Minus: s = "M5 12 H19"; break;
    case Icon::ArrowDown: s = "M12 4 V19 M6 13 L12 19 L18 13"; break;
    case Icon::ArrowRight: s = "M4 12 H19 M13 6 L19 12 L13 18"; break;
    case Icon::ChevronLeft: s = "M15 5 L8 12 L15 19"; break;
    case Icon::ChevronRight: s = "M9 5 L16 12 L9 19"; break;
    case Icon::Star: f = "M12 3 L14.7 9.2 L21.5 9.8 L16.3 14.2 L17.9 20.8 L12 17.3 L6.1 20.8 L7.7 14.2 L2.5 9.8 L9.3 9.2 Z"; break;
    case Icon::Saw: s = "M3 17 L11 7 V17 L19 7 V17 H21"; break;
    case Icon::Sine: s = "M3 12 C6 3 9 3 12 12 C15 21 18 21 21 12"; break;
    case Icon::Fm: s = circle(7, 8, 3) + circle(17, 8, 3) + circle(12, 17, 3) + "M9 10.5 L10.6 14.4 M15 10.5 L13.4 14.4"; break;
    case Icon::Chip: s = box(7, 7, 10, 10, 1) + "M10 4 V7 M14 4 V7 M10 17 V20 M14 17 V20 M4 10 H7 M4 14 H7 M17 10 H20 M17 14 H20"; break;
    case Icon::Drum: s = "M4 8 A8 3.5 0 1 0 20 8 A8 3.5 0 1 0 4 8 Z M4 8 V16 C4 18 7.6 19.5 12 19.5 C16.4 19.5 20 18 20 16 V8"; break;
    case Icon::Din:
        s = circle(12, 12, 9);
        f = circle(7.5f, 12.5f, 1.1f) + circle(16.5f, 12.5f, 1.1f) + circle(9, 8.6f, 1.1f) + circle(15, 8.6f, 1.1f) + circle(12, 7, 1.1f);
        break;
    case Icon::Keyboard: s = box(3, 6, 18, 12, 1.5f) + "M7.5 12 V18 M12 12 V18 M16.5 12 V18 M9.75 6 V12 M14.25 6 V12"; break;
    case Icon::Save: s = "M5 3 H16 L20 7 V21 H5 Z M8 3 V8 H15 V3 M8 21 V14 H16 V21"; break;
    case Icon::Copy: s = box(8, 8, 12, 12, 1.5f) + "M16 8 V5.5 A1.5 1.5 0 0 0 14.5 4 H5.5 A1.5 1.5 0 0 0 4 5.5 V14.5 A1.5 1.5 0 0 0 5.5 16 H8"; break;
    case Icon::Flag: s = "M6 21 V4 H18 L15 8.5 L18 13 H6"; break;
    case Icon::Magnet: s = "M6 4 V12 A6 6 0 0 0 18 12 V4 H14 V12 A2 2 0 0 1 10 12 V4 Z M6 8 H10 M14 8 H18"; break;
    case Icon::Metro: s = "M8 3 H16 L19 21 H5 Z M12 15 L17 6 M8.5 15 H15.5"; break;
    case Icon::Tie: s = "M4 9 C8 17 16 17 20 9"; break;
    case Icon::Slide: s = "M4 18 L20 6"; f = circle(4, 18, 2) + circle(20, 6, 2); break;
    case Icon::Accent: s = "M5 6 L19 12 L5 18"; break;
    case Icon::Pin: s = "M12 21 V14 M7 14 H17 L15.5 9 V4 H8.5 V9 Z"; break;
    case Icon::Gear:
        s = circle(12, 12, 3.2f) + "M12 2.5 V6 M12 18 V21.5 M2.5 12 H6 M18 12 H21.5 M5.3 5.3 L7.8 7.8 M16.2 16.2 L18.7 18.7 M5.3 18.7 L7.8 16.2 M16.2 7.8 L18.7 5.3";
        break;
    case Icon::Info: s = circle(12, 12, 9) + "M12 11 V17 M12 7.4 V7.6"; break;
    case Icon::Cloud: s = "M7 18 A4.5 4.5 0 0 1 7.5 9 A5.5 5.5 0 0 1 18 10.5 A3.8 3.8 0 0 1 17.5 18 Z"; break;
    case Icon::Tape: s = box(3, 6, 18, 12, 2) + circle(8.5f, 12, 2) + circle(15.5f, 12, 2); break;
    case Icon::Clock: s = circle(12, 12, 8.5f) + "M12 7 V12 L15.5 14"; break;
    case Icon::Swap: s = "M4 8 H18 M14 4 L18 8 L14 12 M20 16 H6 M10 12 L6 16 L10 20"; break;
    case Icon::Route: s = "M3 12 H9 C14 12 12 6 21 6 M9 12 C14 12 12 18 21 18"; break;
    case Icon::Bars: s = "M6 20 V10 M12 20 V4 M18 20 V13"; break;
    case Icon::Sliders: s = "M6 4 V20 M12 4 V20 M18 4 V20"; f = box(3.5f, 7, 5, 4, 1) + box(9.5f, 13, 5, 4, 1) + box(15.5f, 6, 5, 4, 1); break;
    case Icon::Layers: s = "M12 4 L21 9 L12 14 L3 9 Z M3 13.5 L12 18.5 L21 13.5"; break;
    case Icon::Screen: s = box(3, 5, 18, 13, 2) + "M9 21 H15"; break;
    case Icon::File: s = "M6 2 H14 L19 7 V22 H6 Z M14 2 V7 H19"; break;
    case Icon::Wheel: s = circle(12, 12, 8.5f); f = circle(12, 6.6f, 1.6f); break;
    }
    return {svg_path(s.c_str()), svg_path(f.c_str())};
}

const Shape& shape(Icon icon) {
    static const auto all = [] {
        std::array<Shape, size_t(Icon::Count)> out;
        for (size_t n = 0; n < out.size(); ++n) out[n] = make(Icon(n));
        return out;
    }();
    return all[size_t(icon) % all.size()];
}

} // namespace

juce::Path svg_path(const char* data) {
    juce::Path path;
    const char* at = data;
    auto skip = [&] { while (*at == ' ' || *at == ',' || *at == '\n') ++at; };
    auto number = [&] {
        skip();
        char* end = nullptr;
        float v = std::strtof(at, &end);
        at = end && end != at ? end : at + (*at ? 1 : 0);
        return v;
    };
    char command = 0;
    float x = 0, y = 0, start_x = 0, start_y = 0;
    for (skip(); *at; skip()) {
        if (std::isalpha(static_cast<unsigned char>(*at))) command = *at++;
        switch (command) {
        case 'M': x = number(); y = number(); path.startNewSubPath(x, y); start_x = x; start_y = y; command = 'L'; break;
        case 'L': x = number(); y = number(); path.lineTo(x, y); break;
        case 'H': x = number(); path.lineTo(x, y); break;
        case 'V': y = number(); path.lineTo(x, y); break;
        case 'C': {
            float x1 = number(), y1 = number(), x2 = number(), y2 = number();
            x = number(); y = number();
            path.cubicTo(x1, y1, x2, y2, x, y);
            break;
        }
        case 'Q': {
            float x1 = number(), y1 = number();
            x = number(); y = number();
            path.quadraticTo(x1, y1, x, y);
            break;
        }
        case 'A': {
            float rx = number(), ry = number();
            number(); // the ellipse's rotation: the glyphs draw none
            bool large = number() != 0, sweep = number() != 0;
            float tx = number(), ty = number();
            arc_to(path, x, y, rx, ry, large, sweep, tx, ty);
            x = tx; y = ty;
            break;
        }
        case 'Z': path.closeSubPath(); x = start_x; y = start_y; break;
        default: ++at; break; // not part of the subset: skipped
        }
    }
    return path;
}

void icon(Paint& p, Icon which, float x, float y, float size, juce::Colour c, float stroke) {
    if (which == Icon::None) return;
    const auto& s = shape(which);
    auto place = juce::AffineTransform::scale(size / 24.0f).translated(x, y);
    p.g.setColour(c);
    if (!s.fill.isEmpty()) p.g.fillPath(s.fill, place);
    if (!s.stroke.isEmpty()) {
        auto line = s.stroke;
        line.applyTransform(place);
        p.g.strokePath(line, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

} // namespace melodee::ui
