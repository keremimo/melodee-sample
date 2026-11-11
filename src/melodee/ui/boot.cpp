// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/boot.h"
#include "melodee/ui/boot_art.h"
#include <algorithm>

namespace melodee::ui {
namespace {

constexpr int feather = 48; // the wipe's soft edge, in pixels

juce::Image decode(const char* base64) {
    juce::MemoryOutputStream png;
    if (!juce::Base64::convertFromBase64(png, base64)) return {};
    auto image = juce::ImageFileFormat::loadFrom(png.getData(), png.getDataSize());
    if (!image.isValid() || image.getWidth() != 320 || image.getHeight() != 240) return {};
    return image.convertedToFormat(juce::Image::ARGB);
}

float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

} // namespace

uint32_t BootAnimation::elapsed(uint32_t now) {
    if (!started) { started = true; start = now; }
    return now - start;
}

bool BootAnimation::finished(uint32_t now) {
    if (!loaded) load();
    return broken || elapsed(now) >= fade_from + out_ms;
}

bool BootAnimation::covers(uint32_t now) { return !finished(now) && elapsed(now) < fade_from; }

void BootAnimation::skip(uint32_t now) { fade_from = std::min(fade_from, elapsed(now)); }

void BootAnimation::load() {
    loaded = true;
    logo = decode(boot_logo_png);
    backdrop = decode(boot_backdrop_png);
    broken = !logo.isValid() || !backdrop.isValid();
    if (broken) return;
    canvas = juce::Image(juce::Image::ARGB, 320, 240, false);
    // The wipe runs across the logo's own columns, not the whole screen.
    juce::Image::BitmapData pixels(logo, juce::Image::BitmapData::readOnly);
    logo_left = 320, logo_right = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            if (pixels.getPixelColour(x, y).getAlpha()) {
                logo_left = std::min(logo_left, x);
                logo_right = std::max(logo_right, x + 1);
            }
    if (logo_left >= logo_right) logo_left = 0, logo_right = 320;
}

// The frame `at` ms in, into `canvas`: the logo's columns up to the wipe's
// edge, over the backdrop at its fade, over black. Both images are
// premultiplied, so a fade scales all four channels alike.
void BootAnimation::compose(uint32_t at) {
    if (composed == int64_t(at)) return;
    composed = at;
    float wipe = smooth(float(int(at) - int(black_ms)) / float(wipe_ms));
    float edge = float(logo_left - feather) + wipe * float(logo_right - logo_left + feather);
    float shown = smooth(float(int(at) - int(black_ms + wipe_ms + logo_hold_ms)) / float(backdrop_ms));
    float column[320];
    for (int x = 0; x < 320; ++x) column[x] = std::clamp((edge - float(x)) / float(feather), 0.0f, 1.0f);

    juce::Image::BitmapData out(canvas, juce::Image::BitmapData::writeOnly);
    juce::Image::BitmapData front(logo, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData back(backdrop, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < 240; ++y) {
        auto* o = reinterpret_cast<juce::PixelARGB*>(out.getLinePointer(y));
        auto* l = reinterpret_cast<const juce::PixelARGB*>(front.getLinePointer(y));
        auto* b = reinterpret_cast<const juce::PixelARGB*>(back.getLinePointer(y));
        for (int x = 0; x < 320; ++x) {
            float a = column[x];
            float under = shown * (1.0f - float(l[x].getAlpha()) * a / 255.0f);
            auto mix = [&](uint8_t top, uint8_t bottom) { return uint8_t(std::min(255.0f, float(top) * a + float(bottom) * under + 0.5f)); };
            o[x].setARGB(255, mix(l[x].getRed(), b[x].getRed()), mix(l[x].getGreen(), b[x].getGreen()), mix(l[x].getBlue(), b[x].getBlue()));
        }
    }
}

void BootAnimation::draw(juce::Graphics& g, uint32_t now) {
    if (finished(now)) return;
    uint32_t at = elapsed(now);
    compose(std::min(at, fade_from));
    float opacity = at < fade_from ? 1.0f : 1.0f - smooth(float(at - fade_from) / float(out_ms));
    g.setOpacity(opacity);
    g.drawImageAt(canvas, 0, 0);
    g.setOpacity(1.0f);
}

} // namespace melodee::ui
