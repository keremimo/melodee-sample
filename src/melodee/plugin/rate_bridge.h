// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "dsp/frame_ring.h"
#include "melodee/plugin/polyphase.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

// A plug-in that runs at a rate of its own, heard at the device's. Open MPC
// plug-ins (open-mpc.github.io) are written to MPC OS's contract, 44.1 kHz
// whatever the host says: at 48 or 96 kHz Melodee runs one at 44.1 kHz and
// converts its output up to the device's rate (an effect's input down first)
// with the base's band-limited SincResampler's kernel (dsp/frame_ring.h; plugin/polyphase.h), so it plays in tune. An
// instrument lags by the converter's half kernel, under a millisecond; an
// effect by both converters and a margin, about 3 ms. Audio thread only;
// nothing allocates.
namespace melodee {

class PluginRateBridge {
public:
    static constexpr unsigned most = 128; // frames of either rate in one call

    // `plugin_rate`: what the plug-in runs at; `device_rate`: what render() is asked for. Equal: nothing converts.
    // `measured`: its latency() found too (not on the audio thread).
    void reset(unsigned plugin_rate, unsigned device_rate, bool instrument, bool measured = true) {
        on = plugin_rate && device_rate && plugin_rate != device_rate;
        lag = measured ? measure(plugin_rate, device_rate, instrument) : 0;
        is_instrument = instrument;
        ratio = on ? double(plugin_rate) / device_rate : 1.0;
        up.reset(plugin_rate, device_rate);
        down.reset(device_rate, plugin_rate);
        out_read = out_write = in_read = in_write = 0;
        // An effect starts with what its two conversions hold back, as silence, so its output never runs short
        // while the input comes in at the other rate.
        if (on && !instrument) {
            unsigned held = unsigned(std::ceil(::SincResampler<2>::max_half / ratio)) + 8;
            for (unsigned n = 0; n < held; ++n) push_out(0, 0);
        }
    }
    bool converting() const { return on; }
    // Roadmap P4: device frames the conversion holds back (0 when it does not convert), measured once at reset():
    // an impulse through a bridge like this one with a plug-in that passes it through, where its peak comes out.
    unsigned latency() const { return lag; }
    // Not on the audio thread: the measuring makes a bridge of its own.
    static unsigned measure(unsigned plugin_rate, unsigned device_rate, bool instrument) {
        if (!plugin_rate || !device_rate || plugin_rate == device_rate) return 0;
        auto probe = std::make_unique<PluginRateBridge>();
        probe->reset(plugin_rate, device_rate, instrument, false);
        // The impulse well after the start (a converter takes its first frame to have been there always): an
        // instrument's at its own frame `start`, an effect's at the device's.
        constexpr unsigned start = 512;
        unsigned made = 0, at = 0, best = 0;
        float peak = 0;
        std::array<float, most> l {}, r {};
        for (unsigned block = 0; block < 64; ++block) {
            l.fill(0); r.fill(0);
            if (!instrument && at <= start && start < at + most) l[start - at] = r[start - at] = 1;
            probe->render(l.data(), r.data(), most, [&](float* pl, float* pr, unsigned n) {
                if (instrument && made <= start && start < made + n) { pl[start - made] += 1; pr[start - made] += 1; }
                made += n;
            });
            for (unsigned f = 0; f < most; ++f, ++at)
                if (std::fabs(l[f]) > peak) { peak = std::fabs(l[f]); best = at; }
        }
        double sent = instrument ? start * double(device_rate) / plugin_rate : start;
        return unsigned(std::max(0.0, std::round(best - sent)));
    }

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

    PolyphaseResampler up, down; // the base's SincResampler, its weights worked out once
    std::array<float, 2 * out_ring> out {};
    std::array<float, 2 * in_ring> in {};
    unsigned out_read = 0, out_write = 0, in_read = 0, in_write = 0;
    double ratio = 1; // plug-in frames per device frame
    unsigned lag = 0; // latency()
    bool on = false, is_instrument = true;
};

} // namespace melodee
