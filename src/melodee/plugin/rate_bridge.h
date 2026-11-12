// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "dsp/frame_ring.h"
#include <algorithm>
#include <array>
#include <cmath>

// A plug-in that runs at a rate of its own, heard at the device's. Open MPC
// plug-ins (open-mpc.github.io) are written to MPC OS's contract, 44.1 kHz
// whatever the host says: at 48 or 96 kHz Melodee runs one at 44.1 kHz and
// converts its output up to the device's rate (an effect's input down first)
// with the base's band-limited SincResampler (dsp/frame_ring.h), so it plays in tune. An
// instrument lags by the converter's half kernel, under a millisecond; an
// effect by both converters and a margin, about 3 ms. Audio thread only;
// nothing allocates.
namespace melodee {

class PluginRateBridge {
public:
    static constexpr unsigned most = 128; // frames of either rate in one call

    // `plugin_rate`: what the plug-in runs at; `device_rate`: what render() is asked for. Equal: nothing converts.
    void reset(unsigned plugin_rate, unsigned device_rate, bool instrument) {
        on = plugin_rate && device_rate && plugin_rate != device_rate;
        is_instrument = instrument;
        ratio = on ? double(plugin_rate) / device_rate : 1.0;
        up.reset(plugin_rate, device_rate);
        down.reset(device_rate, plugin_rate);
        out_read = out_write = in_read = in_write = 0;
        // An effect starts with what its two conversions hold back, as silence, so its output never runs short
        // while the input comes in at the other rate.
        if (on && !instrument) {
            unsigned lag = unsigned(std::ceil(::SincResampler<2>::max_half / ratio)) + 8;
            for (unsigned n = 0; n < lag; ++n) push_out(0, 0);
        }
    }
    bool converting() const { return on; }

    // `frames` device frames into left/right: an instrument adds into them, an effect replaces them with what it
    // made of them. `process(left, right, n)` runs the plug-in for n frames of its own, in place (an effect's input
    // there, both its output).
    template <class Process>
    void render(float* left, float* right, unsigned frames, Process&& process) {
        frames = std::min(frames, most);
        std::array<float, most> l {}, r {};
        auto play = [&](unsigned n) {
            process(l.data(), r.data(), n);
            for (unsigned f = 0; f < n; ++f) up.feed(l[f], r[f], [&](float a, float b) { push_out(a, b); });
        };
        if (is_instrument) {
            // Its frames for what is missing, and one over (the conversion's step varies).
            for (unsigned guard = 0; out_size() < frames && guard < 8; ++guard) {
                unsigned n = std::min<unsigned>(most, unsigned(std::ceil((frames - out_size()) * ratio)) + 1);
                std::fill(l.begin(), l.begin() + n, 0.0f);
                std::fill(r.begin(), r.begin() + n, 0.0f);
                play(n);
            }
        } else {
            for (unsigned f = 0; f < frames; ++f) down.feed(left[f], right[f], [&](float a, float b) { push_in(a, b); });
            while (unsigned n = std::min(in_size(), most)) {
                for (unsigned f = 0; f < n; ++f, ++in_read) {
                    l[f] = in[2 * (in_read % in_ring)];
                    r[f] = in[2 * (in_read % in_ring) + 1];
                }
                play(n);
            }
        }
        for (unsigned f = 0; f < frames; ++f) {
            float a = 0, b = 0;
            if (out_size()) { a = out[2 * (out_read % out_ring)]; b = out[2 * (out_read % out_ring) + 1]; ++out_read; }
            if (is_instrument) { left[f] += a; right[f] += b; }
            else { left[f] = a; right[f] = b; }
        }
    }

private:
    static constexpr unsigned out_ring = 2048, in_ring = 1024; // frames
    unsigned out_size() const { return out_write - out_read; }
    unsigned in_size() const { return in_write - in_read; }
    void push_out(float a, float b) {
        if (out_size() >= out_ring) return; // cannot happen at these rates: a frame dropped rather than one overwritten
        out[2 * (out_write % out_ring)] = a; out[2 * (out_write % out_ring) + 1] = b; ++out_write;
    }
    void push_in(float a, float b) {
        if (in_size() >= in_ring) return;
        in[2 * (in_write % in_ring)] = a; in[2 * (in_write % in_ring) + 1] = b; ++in_write;
    }

    ::SincResampler<2> up, down;
    std::array<float, 2 * out_ring> out {};
    std::array<float, 2 * in_ring> in {};
    unsigned out_read = 0, out_write = 0, in_read = 0, in_write = 0;
    double ratio = 1; // plug-in frames per device frame
    bool on = false, is_instrument = true;
};

} // namespace melodee
