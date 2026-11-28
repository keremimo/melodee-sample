// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A PCM WAV read in place (src/melodee/engine/stream_pager.h's read_in_place()): an instrument's file streamed from the
// card as it is, with no copy laid out. Each chunk the pager fills holds the floats the WAV reader makes of the same
// frames, for every kind of PCM it reads; chunks are counted across two pagers; a file that ends short stops the
// voice and nothing else; and through the sampler a zone read in place sounds frame for frame as the same frames
// held in memory, at the file's own rate whatever the device runs at.
#include "melodee/engine/dsp.h"
#include "melodee/engine/sample_data.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/stream_pager.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace melodee;

namespace {
int failures = 0;
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #ok); ++failures; } } while (0)

void put(std::vector<uint8_t>& out, uint32_t value, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) out.push_back(uint8_t(value >> (8 * n))); }

// A WAV of `frames` frames written by hand: integer PCM of `bits`, or 32-bit float; a sine a side, each its own.
std::string wav_file(const std::string& path, unsigned bits, bool floating, unsigned channels, unsigned rate, uint32_t frames) {
    std::vector<uint8_t> data;
    for (uint32_t n = 0; n < frames; ++n)
        for (unsigned c = 0; c < channels; ++c) {
            const double v = 0.6 * std::sin(n * (c ? 0.0171 : 0.0113)) + 0.2 * std::sin(n * 0.31 + c);
            if (floating) { float f = float(v); uint32_t raw; std::memcpy(&raw, &f, 4); put(data, raw, 4); }
            else put(data, uint32_t(int32_t(std::lround(v * double((1u << (bits - 1)) - 1)))), bits / 8);
        }
    std::vector<uint8_t> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'}); put(file, uint32_t(36 + data.size()), 4);
    file.insert(file.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put(file, 16, 4);
    put(file, floating ? 3 : 1, 2); put(file, channels, 2); put(file, rate, 4); put(file, rate * channels * bits / 8, 4);
    put(file, channels * bits / 8, 2); put(file, bits, 2);
    file.insert(file.end(), {'d', 'a', 't', 'a'}); put(file, uint32_t(data.size()), 4);
    file.insert(file.end(), data.begin(), data.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    return path;
}

// The file's frames as the WAV reader makes them, at its own rate.
struct Frames { std::vector<float> left, right; WavLayout layout; };
Frames read_frames(const std::string& path) {
    Frames out;
    WavReader reader;
    CHECK(reader.open(path, 600).empty());
    out.layout = reader.layout();
    out.left.resize(reader.frames()); out.right.resize(reader.frames());
    CHECK(reader.read(out.left.data(), out.right.data(), reader.frames()) == reader.frames());
    return out;
}

// One zone of one part on a pad, played through the sampler (as tests/melodee.cpp's ZoneRig).
struct Rig {
    std::shared_ptr<Multisample> multi = std::make_shared<Multisample>();
    SampleData data;
    SamplerEngine::Table table {};
    std::unique_ptr<SamplerEngine> engine = std::make_unique<SamplerEngine>();
    ZonePart& part() { return multi->parts[0]; }
    void begin() { multi->parts = std::make_unique<ZonePart[]>(1); multi->part_count = 1; }
    void build() {
        multi->zones.emplace_back();
        multi->turns.assign(1, 0);
        part().frames.store(part().total);
        data.multi = multi;
        table[3] = &data;
        engine->set_samples(&table);
        PadSound pad;
        pad.mono = false; pad.sample = 3; pad.play = PadPlay::OneShot; pad.velocity_sense = 0;
        engine->set_pad(0, pad);
    }
    std::vector<float> run(unsigned frames, StreamPager* pager = nullptr) {
        std::vector<float> out(frames), right(frames);
        for (unsigned done = 0; done < frames; done += 128) {
            const unsigned n = std::min(128u, frames - done);
            engine->render_stereo(out.data() + done, right.data() + done, n);
            if (pager) while (pager->pump()) {}
        }
        return out;
    }
    void key() { engine->note_on(int(sampler_first_note), 127); }
};

double tone_hz(const std::vector<float>& x, size_t from, size_t to, double rate) {
    unsigned rises = 0;
    for (size_t n = from + 1; n < to; ++n) rises += x[n - 1] < 0 && x[n] >= 0;
    return rises * rate / double(to - from);
}
} // namespace

int main(int argc, char** argv) {
    const std::string folder = argc > 1 ? argv[1] : ".";
    const uint32_t frames = stream_chunk * 5 + 1234; // five whole chunks and a part of one

    // Every kind the WAV reader reads, one side and two: a chunk filled is that reader's floats of the same frames.
    struct Kind { unsigned bits; bool floating; unsigned channels, rate; };
    for (const Kind kind : {Kind {16, false, 2, 44100}, Kind {24, false, 2, 48000}, Kind {32, false, 2, 96000}, Kind {32, true, 2, 48000},
                            Kind {16, false, 1, 22050}, Kind {24, false, 1, 48000}}) {
        const std::string path = wav_file(folder + "/in_place.wav", kind.bits, kind.floating, kind.channels, kind.rate, frames);
        const Frames want = read_frames(path);
        ZonePart part;
        CHECK(read_in_place(path, want.layout, part));
        CHECK(part.total == frames && part.rate == float(kind.rate) && bool(part.source));
        CHECK((part.right == part.left) == (kind.channels == 1));
        CHECK(part.pins.size() == 6);
        StreamPager pager;
        // What a zone's start holds: its first chunk, there until the part goes.
        CHECK(pager.hold(part, 0, stream_chunk) && part.pins[0] == 1);
        bool same = true;
        for (uint32_t n = 0; n < stream_chunk; ++n) same = same && part.left[n] == want.left[n] && part.right[n] == want.right[n];
        CHECK(same);
        // A voice from the start to the end: every frame below the limit is the file's, and the limit reaches the end.
        const int stream = pager.open(part, 0, false, 1.0f);
        CHECK(stream >= 0);
        uint32_t checked = 0, at = 0;
        same = true;
        for (unsigned pass = 0; pass < 4000 && checked < frames; ++pass) {
            while (pager.pump()) {}
            const uint32_t limit = std::min(pager.limit(stream), frames);
            for (; checked < limit; ++checked) same = same && part.left[checked] == want.left[checked] && part.right[checked] == want.right[checked];
            at = std::min(at + 4096, limit ? limit - 1 : 0);
            pager.place(stream, at);
        }
        CHECK(same && checked == frames && !pager.dead(stream));
        pager.place(stream, frames - 1); // at its end: what lies behind it is let go
        pager.pump(); pager.pump();
        // Behind the voice the chunks were given back (but for the one before it, and the one held).
        CHECK(part.pins[0] == 1 && part.pins[1] == 0 && part.pins[2] == 0 && part.pins[5] == 1);
#if defined(__linux__)
        stream_cache_give_back(); // let go, they stay in the cache until the memory is wanted (stream_cache_bytes)
        bool silent = true; // given back, their memory went: they read as silence
        for (uint32_t n = stream_chunk; n < 3 * stream_chunk; ++n) silent = silent && part.left[n] == 0.0f;
        CHECK(silent);
#endif
        pager.close(stream);
        while (pager.pump()) {}
        for (size_t c = 1; c < part.pins.size(); ++c) CHECK(part.pins[c] == 0);
        // Wanted again, a chunk is read again.
        CHECK(pager.hold(part, 2 * stream_chunk, 1) && part.left[2 * stream_chunk + 7] == want.left[2 * stream_chunk + 7]);
    }

    // Two pagers on one part (the live one and a print's): a chunk stays while either needs it.
    {
        const std::string path = wav_file(folder + "/in_place.wav", 24, false, 2, 48000, frames);
        const Frames want = read_frames(path);
        ZonePart part;
        CHECK(read_in_place(path, want.layout, part));
        StreamPager live, print;
        const int a = live.open(part, 3 * stream_chunk, false, 1.0f), b = print.open(part, 3 * stream_chunk, false, 1.0f);
        while (live.pump()) {}
        while (print.pump()) {}
        CHECK(part.pins[3] == 2);
        print.close(b);
        while (print.pump()) {}
        CHECK(part.pins[3] == 1 && part.left[3 * stream_chunk + 99] == want.left[3 * stream_chunk + 99]);
        live.close(a);
        while (live.pump()) {}
        CHECK(part.pins[3] == 0);
    }

    // A file that ends short of what it said (the card pulled, a copy cut off): the voice's stream dies, no more.
    {
        const std::string path = wav_file(folder + "/in_place.wav", 16, false, 2, 44100, frames);
        const Frames want = read_frames(path);
        ZonePart part;
        CHECK(read_in_place(path, want.layout, part));
        CHECK(truncate(path.c_str(), off_t(want.layout.data_at + 2 * stream_chunk * want.layout.block())) == 0);
        StreamPager pager;
        CHECK(pager.hold(part, 0, stream_chunk));               // what the file still holds is read
        CHECK(!pager.hold(part, 4 * stream_chunk, stream_chunk)); // what it lost is not
        const int stream = pager.open(part, stream_chunk, false, 1.0f);
        for (unsigned pass = 0; pass < 50; ++pass) { pager.pump(); pager.place(stream, std::min(pager.limit(stream), frames) - 1); }
        CHECK(pager.dead(stream) && pager.limit(stream) <= 2 * stream_chunk);
        pager.close(stream);
        while (pager.pump()) {}
    }
    // Not a file, or nothing in it: refused, the part untouched.
    {
        ZonePart part;
        WavLayout layout;
        layout.frames = 1000; layout.channels = 2; layout.bits = 16; layout.rate = 44100;
        CHECK(!read_in_place(folder + "/no such file.wav", layout, part) && !part.source && !part.mapping);
        layout.frames = 0;
        CHECK(!read_in_place(folder + "/in_place.wav", layout, part) && !part.source);
    }

    // Through the sampler, at the device's 48 kHz and at 44.1 kHz: a zone read in place sounds frame for frame as the
    // same frames held in memory, and at its own pitch (a 48 kHz file is not taken for a 44.1 kHz one).
    for (const float device : {48000.0f, 44100.0f}) {
        dsp::device_rate.store(device);
        const uint32_t length = stream_chunk * 4;
        const std::string path = folder + "/in_place_tone.wav";
        { // a 480 Hz tone at 48 kHz, both sides alike
            std::vector<uint8_t> data;
            for (uint32_t n = 0; n < length; ++n) {
                const auto v = uint32_t(int32_t(std::lround(0.5 * std::sin(2 * 3.14159265358979 * 480.0 * n / 48000.0) * 8388607.0)));
                put(data, v, 3); put(data, v, 3);
            }
            std::vector<uint8_t> file;
            file.insert(file.end(), {'R', 'I', 'F', 'F'}); put(file, uint32_t(36 + data.size()), 4);
            file.insert(file.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put(file, 16, 4);
            put(file, 1, 2); put(file, 2, 2); put(file, 48000, 4); put(file, 48000 * 6, 4); put(file, 6, 2); put(file, 24, 2);
            file.insert(file.end(), {'d', 'a', 't', 'a'}); put(file, uint32_t(data.size()), 4);
            file.insert(file.end(), data.begin(), data.end());
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
        }
        const Frames want = read_frames(path);
        Rig held, placed;
        held.begin();
        held.part().left = want.left.data(); held.part().right = want.right.data();
        held.part().total = length; held.part().rate = 48000;
        held.part().pins.assign(4, 0);
        held.build();
        placed.begin();
        CHECK(read_in_place(path, want.layout, placed.part()));
        StreamPager pager;
        CHECK(pager.hold(placed.part(), 0, stream_chunk)); // as the library holds a zone's start
        placed.build();
        placed.engine->set_pager(&pager);
        held.key(); placed.key();
        while (pager.pump()) {}
        const unsigned out_frames = unsigned(double(length) * device / 48000.0) - 2048;
        const auto x = held.run(out_frames), y = placed.run(out_frames, &pager);
        bool same = true;
        for (unsigned n = 0; n < out_frames; ++n) same = same && x[n] == y[n];
        CHECK(same && pager.underruns() == 0);
        const double hz = tone_hz(y, 4096, out_frames, device);
        CHECK(std::fabs(hz - 480.0) < 2.0);
    }
    dsp::device_rate.store(44100.0f);

    std::remove((folder + "/in_place.wav").c_str());
    std::remove((folder + "/in_place_tone.wav").c_str());
    if (failures) return 1;
    std::printf("direct stream ok\n");
    return 0;
}
