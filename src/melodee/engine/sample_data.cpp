// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sample_data.h"
#include "melodee/engine/dsp.h"
#include "dsp/frame_ring.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

namespace melodee {
namespace {
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
} // namespace

std::string read_wav(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info, float play_at) {
    WavReader reader;
    auto error = reader.open(path, max_seconds, info);
    if (!error.empty()) return error;
    // To 44.1 kHz, band-limited (roadmap P2; it was linear): straight through when it is at that rate already.
    std::vector<float> l(reader.frames()), r(reader.frames());
    l.resize(reader.read(l.data(), r.data(), reader.frames()));
    r.resize(l.size());
    if (l.empty()) return "no audio";
    adopt(out, std::move(l), std::move(r), reader.rate(), play_at);
    return {};
}

std::string WavReader::open(const std::string& path, unsigned max_seconds, WavInfo* info) {
    file = std::ifstream(path, std::ios::binary);
    format = channels = hz = bits = 0;
    count = done = 0;
    if (!file) return "cannot open";
    uint8_t head[12];
    if (!file.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) return "not a WAV file";
    uint64_t data_at = 0, data_size = 0;
    bool found = false;
    while (file) {
        uint8_t chunk[8];
        if (!file.read(reinterpret_cast<char*>(chunk), 8)) break;
        uint32_t size = u32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            std::vector<uint8_t> fmt(size);
            if (size < 16 || !file.read(reinterpret_cast<char*>(fmt.data()), size)) return "broken format chunk";
            format = u16(&fmt[0]); channels = u16(&fmt[2]); hz = u32(&fmt[4]); bits = u16(&fmt[14]);
            if (format == 0xfffe && size >= 26) format = u16(&fmt[24]); // WAVE_FORMAT_EXTENSIBLE: the sub-format
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (!format) return "data before format";
            if (!found) { found = true; data_at = uint64_t(file.tellg()); data_size = size; }
            if (!info) break;
            file.seekg(std::streamoff(uint64_t(size) + (size & 1)), std::ios::cur); // a smpl chunk may follow the audio
        } else if (info && !std::memcmp(chunk, "smpl", 4) && size >= 60) {
            uint8_t body[60];
            if (!file.read(reinterpret_cast<char*>(body), 60)) break;
            if (u32(body + 28) > 0 && u32(body + 48) > u32(body + 44)) { info->loop_start = u32(body + 44); info->loop_end = int64_t(u32(body + 48)) + 1; }
            file.seekg(std::streamoff(uint64_t(size) + (size & 1) - 60), std::ios::cur);
        } else {
            file.seekg(size + (size & 1), std::ios::cur);
        }
    }
    bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32), floating = format == 3 && bits == 32;
    if (!pcm && !floating) return "unsupported sample format";
    if (channels < 1 || channels > 2 || hz < 8000 || hz > 192000) return "unsupported channels or rate";
    // As much of the data chunk as the file holds (a chunk may say more than was written), at most max_seconds.
    file.clear();
    file.seekg(0, std::ios::end);
    const uint64_t end = uint64_t(std::max<std::streamoff>(file.tellg(), 0));
    const uint64_t held = found && end > data_at ? std::min(data_size, end - data_at) : 0;
    const uint64_t limit = uint64_t(max_seconds) * hz * channels * (bits / 8);
    count = uint32_t(std::min(held, limit) / ((bits / 8) * channels));
    if (!count) return "no audio";
    if (info) { info->channels = channels; info->rate = hz; }
    file.seekg(std::streamoff(data_at));
    return {};
}

uint32_t WavReader::read(float* left, float* right, uint32_t frames) {
    const uint32_t wanted = std::min(frames, count - done);
    if (!wanted || !file) return 0;
    const unsigned size = bits / 8, block = size * channels;
    const bool floating = format == 3;
    bytes.resize(size_t(wanted) * block);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    frames = uint32_t(uint64_t(std::max<std::streamsize>(file.gcount(), 0)) / block);
    if (frames < wanted) count = done + frames; // it could not be read: no more
    auto sample = [&](unsigned frame, unsigned channel) {
        const uint8_t* p = &bytes[(size_t(frame) * channels + channel) * size];
        if (floating) { float f; std::memcpy(&f, p, 4); return std::isfinite(f) ? f : 0.0f; }
        if (bits == 16) return int16_t(u16(p)) / 32768.0f;
        if (bits == 24) return int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) / 2147483648.0f;
        return int32_t(u32(p)) / 2147483648.0f;
    };
    for (unsigned n = 0; n < frames; ++n) { left[n] = sample(n, 0); right[n] = sample(n, std::min(1u, channels - 1)); }
    done += frames;
    return frames;
}

SampleFill::SampleFill() = default;
SampleFill::~SampleFill() = default;

void SampleFill::begin(SampleData& s, uint32_t frames, unsigned rate, float play_at, size_t play_room) {
    sample = &s;
    growth = std::make_shared<SampleGrowth>();
    s.growth = growth;
    s.multi = nullptr;
    s.source_rate = rate;
    left_to_add = frames;
    const auto model = unsigned(dsp::model_rate), to = unsigned(std::lround(play_at));
    // As adopt(): a copy at the file's own rate is its frames, any other one band-limited conversion of them, as long as
    // resample() makes it.
    auto length = [&](unsigned at) { return at == rate ? frames : uint32_t(std::llround(double(frames) * at / rate)); };
    auto lay_out = [&](Copy& c, std::vector<float>& l, std::vector<float>& r, std::atomic<uint32_t>& landed, unsigned at) {
        c.left = &l; c.right = &r; c.landed = &landed;
        c.length = length(at); c.written = 0;
        l.assign(c.length, 0.0f); r.assign(c.length, 0.0f);
        c.converter.reset();
        if (at != rate) { c.converter = std::make_unique<SincResampler<2>>(); c.converter->reset(rate, at); }
    };
    copies = {};
    lay_out(copies[0], s.left, s.right, growth->frames, model);
    s.frames = copies[0].length;
    s.play_left = {}; s.play_right = {};
    s.play_frames = 0; s.play_rate = 0;
    if (to >= 8000 && to != model && size_t(length(to)) * 2 * sizeof(float) <= play_room) {
        lay_out(copies[1], s.play_left, s.play_right, growth->play_frames, to);
        s.play_frames = copies[1].length;
        s.play_rate = play_at;
    }
}

void SampleFill::add(const float* left, const float* right, uint32_t frames) {
    if (!sample) return;
    frames = std::min(frames, left_to_add);
    for (auto& c : copies) {
        if (!c.left) continue;
        if (!c.converter) {
            const uint32_t n = std::min(frames, c.length - c.written);
            std::copy(left, left + n, c.left->begin() + c.written);
            std::copy(right, right + n, c.right->begin() + c.written);
            c.written += n;
        } else {
            auto keep = [&](float l, float r) { if (c.written < c.length) { (*c.left)[c.written] = l; (*c.right)[c.written] = r; ++c.written; } };
            for (uint32_t n = 0; n < frames; ++n) c.converter->feed(left[n], right[n], keep);
        }
        c.landed->store(c.written, std::memory_order_release);
    }
    left_to_add -= frames;
}

void SampleFill::finish() {
    if (!sample) return;
    const bool whole = !left_to_add;
    for (auto& c : copies) {
        if (!c.left) continue;
        if (c.converter) { // it answers `half` frames late: silence after the last frame lets the end out (resample())
            auto keep = [&](float l, float r) { if (c.written < c.length) { (*c.left)[c.written] = l; (*c.right)[c.written] = r; ++c.written; } };
            for (unsigned n = 0; n <= SincResampler<2>::max_half && c.written < c.length; ++n) c.converter->feed(0.0f, 0.0f, keep);
        }
        if (whole) c.written = c.length; // what the conversion did not reach stays silent, as resample() pads it
        c.landed->store(c.written, std::memory_order_release);
        c.converter.reset();
    }
    (whole ? growth->whole : growth->cut).store(true, std::memory_order_release);
    copies = {};
    growth.reset();
    sample = nullptr;
}

void adopt(SampleData& out, std::vector<float> left, std::vector<float> right, unsigned rate, float play_at) {
    const auto frames = uint32_t(std::min(left.size(), right.size()));
    const auto model = unsigned(dsp::model_rate), to = unsigned(std::lround(play_at));
    out.source_rate = rate;
    if (rate == model) {
        playback_from(out, left.data(), right.data(), frames, rate, play_at);
        out.left = std::move(left); out.right = std::move(right);
    } else {
        resample(left.data(), right.data(), frames, rate, model, out.left, out.right);
        if (to == rate) { // the frames themselves play: moved, not copied
            left.resize(frames); right.resize(frames);
            out.play_left = std::move(left); out.play_right = std::move(right);
            out.play_frames = frames; out.play_rate = play_at;
        } else playback_from(out, left.data(), right.data(), frames, rate, play_at);
    }
    out.frames = uint32_t(out.left.size());
}

void resample(const float* left, const float* right, uint32_t frames, unsigned from, unsigned to,
              std::vector<float>& out_left, std::vector<float>& out_right) {
    out_left.clear(); out_right.clear();
    if (!frames || !from || !to) return;
    const auto length = size_t(std::llround(double(frames) * to / from));
    out_left.reserve(length); out_right.reserve(length);
    // The converter's output n sits at input n * from / to (frame_ring.h): nothing to drop at the start; it only
    // answers `half` frames late, so silence after the last frame lets the end out.
    auto converter = std::make_unique<SincResampler<2>>();
    converter->reset(from, to);
    auto keep = [&](float l, float r) { if (out_left.size() < length) { out_left.push_back(l); out_right.push_back(r); } };
    for (uint32_t n = 0; n < frames; ++n) converter->feed(left[n], right[n], keep);
    for (unsigned n = 0; n <= SincResampler<2>::max_half && out_left.size() < length; ++n) converter->feed(0.0f, 0.0f, keep);
    out_left.resize(length, 0.0f); out_right.resize(length, 0.0f);
}

void playback_from(SampleData& sample, const float* left, const float* right, uint32_t frames, unsigned rate, float play_at) {
    sample.play_left = {}; sample.play_right = {};
    sample.play_frames = 0; sample.play_rate = 0;
    const auto to = unsigned(std::lround(play_at));
    if (to < 8000 || to == unsigned(dsp::model_rate) || !frames) return;
    if (rate == to) { sample.play_left.assign(left, left + frames); sample.play_right.assign(right, right + frames); }
    else resample(left, right, frames, rate, to, sample.play_left, sample.play_right);
    sample.play_frames = uint32_t(sample.play_left.size());
    sample.play_rate = play_at;
}

void prepare_playback(SampleData& sample, float rate) {
    sample.play_left = {}; sample.play_right = {};
    sample.play_frames = 0; sample.play_rate = 0;
    const auto to = unsigned(std::lround(rate));
    if (sample.multi || !sample.frames || to == unsigned(dsp::model_rate) || to < 8000) return;
    resample(sample.left.data(), sample.right.data(), sample.frames, unsigned(dsp::model_rate), to, sample.play_left, sample.play_right);
    sample.play_frames = uint32_t(sample.play_left.size());
    sample.play_rate = rate;
}

bool read_flac_loop(const std::string& path, WavInfo& info) {
    std::ifstream file(path, std::ios::binary);
    char tag[4];
    if (!file.read(tag, 4) || std::memcmp(tag, "fLaC", 4)) return false;
    uint8_t head[4];
    while (file.read(reinterpret_cast<char*>(head), 4)) {
        const bool last = head[0] & 0x80;
        const unsigned type = head[0] & 0x7f, size = unsigned(head[1]) << 16 | unsigned(head[2]) << 8 | head[3];
        uint8_t body[72];
        if (type == 2 && size >= sizeof(body)) { // "riff", then the chunk: "smpl", its size, its 60 bytes
            if (!file.read(reinterpret_cast<char*>(body), sizeof(body))) return false;
            if (!std::memcmp(body, "riff", 4) && !std::memcmp(body + 4, "smpl", 4) && u32(body + 12 + 28) > 0 && u32(body + 12 + 48) > u32(body + 12 + 44)) {
                info.loop_start = u32(body + 12 + 44);
                info.loop_end = int64_t(u32(body + 12 + 48)) + 1;
                return true;
            }
            file.seekg(std::streamoff(size - sizeof(body)), std::ios::cur);
        } else {
            file.seekg(std::streamoff(size), std::ios::cur);
        }
        if (last) break;
    }
    return false;
}

bool read_mp3_head(const std::string& path, Mp3Head& head) {
    std::ifstream file(path, std::ios::binary);
    uint8_t d[512];
    if (!file.read(reinterpret_cast<char*>(d), 10)) return false;
    std::streamoff first = 0;
    if (!std::memcmp(d, "ID3", 3)) // an ID3v2 tag first: its size in 4 x 7 bits, a footer 10 more
        first = 10 + (std::streamoff(d[6] & 0x7f) << 21 | std::streamoff(d[7] & 0x7f) << 14 | std::streamoff(d[8] & 0x7f) << 7 | (d[9] & 0x7f))
              + (d[5] & 0x10 ? 10 : 0);
    file.seekg(first);
    if (!file.read(reinterpret_cast<char*>(d), sizeof(d))) return false;
    // The first frame's header: Layer III; MPEG 1 (11), 2 (10) or 2.5 (00); a CRC after it when bit 0 is clear.
    if (d[0] != 0xff || (d[1] & 0xe0) != 0xe0 || ((d[1] >> 1) & 3) != 1 || ((d[1] >> 3) & 3) == 1) return false;
    const bool mpeg1 = ((d[1] >> 3) & 3) == 3, mono = (d[3] >> 6) == 3;
    head = Mp3Head {};
    head.frame_samples = mpeg1 ? 1152 : 576;
    const unsigned side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const uint8_t* x = d + 4 + (d[1] & 1 ? 0 : 2) + side; // the Xing or Info tag, in place of the frame's audio
    if (std::memcmp(x, "Xing", 4) && std::memcmp(x, "Info", 4)) return true;
    const unsigned flags = unsigned(x[4]) << 24 | unsigned(x[5]) << 16 | unsigned(x[6]) << 8 | x[7];
    const uint8_t* lame = x + 8 + (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
    if (std::memcmp(lame, "LAME", 4) && std::memcmp(lame, "Lavc", 4) && std::memcmp(lame, "Lavf", 4)) return true;
    head.delay = unsigned(lame[21]) << 4 | lame[22] >> 4; // 12 bits each
    head.padding = unsigned(lame[22] & 15) << 8 | lame[23];
    head.gapless = true;
    return true;
}

namespace {
// A WAV header for `frames` stereo frames at `rate` Hz into `h` (room for 58 bytes): 24-bit PCM, 44 bytes; or 32-bit
// float, 58 (its fmt chunk carries the extension's size and a fact chunk follows, as formats other than PCM take).
// Returns its length.
// Stereo: 32-bit float (floating), else integer PCM of `width` bytes (3: 24-bit; 2: 16-bit, roadmap P4's mixdown).
unsigned wav_header(unsigned char* h, uint32_t frames, bool floating, unsigned rate, unsigned int_width = 3, unsigned channels = 2) {
    auto put = [&](unsigned at, uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) h[at + n] = uint8_t(v >> (8 * n)); };
    const unsigned width = floating ? 4 : int_width, align = channels * width;
    const uint32_t data = frames * align;
    std::memcpy(h, "RIFF", 4); std::memcpy(h + 8, "WAVEfmt ", 8);
    put(16, floating ? 18 : 16, 4); put(20, floating ? 3 : 1, 2); put(22, channels, 2); put(24, rate, 4); put(28, rate * align, 4);
    put(32, align, 2); put(34, 8 * width, 2);
    unsigned at = 36;
    if (floating) { put(36, 0, 2); std::memcpy(h + 38, "fact", 4); put(42, 4, 4); put(46, frames, 4); at = 50; }
    std::memcpy(h + at, "data", 4); put(at + 4, data, 4);
    at += 8;
    put(4, at - 8 + data, 4);
    return at;
}
// 24-bit, unless the sound goes past full scale (a resampled mix, a loop's layers can): then 32-bit float, so the
// file holds what plays instead of clipping it.
bool needs_float(const SampleData& s) {
    for (uint32_t n = 0; n < s.frames; ++n) if (std::fabs(s.left[n]) > 1.0f || std::fabs(s.right[n]) > 1.0f) return true;
    return false;
}
// A sample's 24-bit step as written, and the float read_wav makes of it again: the same scale (2^23) both ways, so a
// step written and read back is written as the same step again.
int32_t pcm24(float v) { return std::min<int32_t>(8388607, int32_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 8388608.0f))); }
float from_pcm24(int32_t v) { return float(int32_t(uint32_t(v) << 8)) / 2147483648.0f; }
// A float sample's four bytes, little-endian; what is not a number is written as silence.
void float_bytes(float v, unsigned char* out) {
    if (!std::isfinite(v)) v = 0;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    for (unsigned k = 0; k < 4; ++k) out[k] = uint8_t(bits >> (8 * k));
}
} // namespace

std::string write_wav(const std::string& path, const SampleData& s, unsigned rate) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return "cannot write";
    const bool floating = needs_float(s);
    unsigned char h[58];
    file.write(reinterpret_cast<const char*>(h), wav_header(h, s.frames, floating, rate));
    auto put = [&](uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) file.put(char(v >> (8 * n))); };
    for (uint32_t n = 0; n < s.frames; ++n)
        for (float v : {s.left[n], s.right[n]}) {
            if (!floating) { put(uint32_t(pcm24(v)), 3); continue; }
            unsigned char bytes[4];
            float_bytes(v, bytes);
            file.write(reinterpret_cast<const char*>(bytes), 4);
        }
    return file.good() ? std::string() : std::string("write failed");
}

void as_written(SampleData& s) {
    if (needs_float(s)) return;
    for (uint32_t n = 0; n < s.frames; ++n) { s.left[n] = from_pcm24(pcm24(s.left[n])); s.right[n] = from_pcm24(pcm24(s.right[n])); }
}

std::string keep_capture(SampleData& s, unsigned rate, const std::string& path, float play_at) {
    s.frames = uint32_t(std::min(s.left.size(), s.right.size()));
    as_written(s); // what plays is what the file holds: it reads back the same (SampleLibrary::refresh_playback)
    std::string failed = path.empty() ? std::string() : write_wav(path, s, rate);
    adopt(s, std::move(s.left), std::move(s.right), rate, play_at);
    return failed;
}

std::vector<unsigned char> WavStream::header(uint32_t frames) const {
    std::vector<unsigned char> h(std::max(58u, data_at));
    unsigned size = wav_header(h.data(), frames, bits == 32, rate, bits / 8, channels);
    if (data_at >= size + 8 && (data_at - size) % 2 == 0) { // a JUNK chunk between the format and the frames
        unsigned char data[8];
        std::memcpy(data, h.data() + size - 8, 8);
        const unsigned junk_at = size - 8, pad = data_at - junk_at - 16;
        auto put = [&](unsigned at, uint32_t v) { for (unsigned n = 0; n < 4; ++n) h[at + n] = uint8_t(v >> (8 * n)); };
        std::memcpy(h.data() + junk_at, "JUNK", 4);
        put(junk_at + 4, pad);
        std::fill(h.begin() + junk_at + 8, h.begin() + junk_at + 8 + pad, 0);
        std::memcpy(h.data() + data_at - 8, data, 8);
        uint32_t riff = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
        put(4, riff + (data_at - size));
        size = data_at;
    }
    h.resize(size);
    return h;
}

bool WavStream::open(const std::string& path, unsigned at_rate, unsigned at_bits, unsigned at_data, unsigned at_channels) {
    close();
    file = std::fopen(path.c_str(), "wb");
    written = 0; failed = !file; rate = at_rate; data_at = at_data;
    channels = std::clamp(at_channels, 1u, max_wav_channels);
    bits = at_bits == 16 || at_bits == 24 ? at_bits : 32;
    auto h = header(0);
    if (file && std::fwrite(h.data(), 1, h.size(), file) != h.size()) failed = true;
    return !failed;
}
bool WavStream::add(const float* interleaved, unsigned frames) {
    if (!file || failed) return false;
    if (bits != 32) { // roadmap P4: integer PCM, TPDF dithered (two uniform draws a sample, a step either way), then clipped
        const unsigned width = bits / 8;
        const double scale = bits == 16 ? 32768.0 : 8388608.0, top = scale - 1;
        unsigned char bytes[3 * 512];
        const unsigned run = 512 / channels;
        for (unsigned at = 0; at < frames; at += run) {
            unsigned n = std::min(run, frames - at);
            for (unsigned f = 0; f < channels * n; ++f) {
                float v = interleaved[channels * at + f];
                if (!std::isfinite(v)) v = 0;
                auto draw = [&] { noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; return double(noise) / 4294967296.0; };
                double d = draw() - draw(); // triangular, -1..1 of a step
                auto s = int32_t(std::clamp(std::floor(double(v) * scale + d + 0.5), -scale, top));
                for (unsigned k = 0; k < width; ++k) bytes[width * f + k] = uint8_t(uint32_t(s) >> (8 * k));
            }
            if (std::fwrite(bytes, 1, channels * width * n, file) != channels * width * n) { failed = true; return false; }
        }
        written += frames;
        return true;
    }
    // 32-bit float as it comes: a mix past full scale is kept whole, not clipped.
    unsigned char bytes[4 * 512];
    const unsigned run = 512 / channels;
    for (unsigned at = 0; at < frames; at += run) {
        unsigned n = std::min(run, frames - at);
        for (unsigned f = 0; f < channels * n; ++f) float_bytes(interleaved[channels * at + f], bytes + 4 * f);
        if (std::fwrite(bytes, 1, 4 * channels * n, file) != 4 * channels * n) { failed = true; return false; }
    }
    written += frames;
    return true;
}
bool WavStream::close() {
    if (!file) return !failed;
    auto h = header(written);
    if (std::fseek(file, 0, SEEK_SET) != 0 || std::fwrite(h.data(), 1, h.size(), file) != h.size()) failed = true;
    if (std::fclose(file) != 0) failed = true;
    file = nullptr;
    return !failed;
}

bool make_overview(const SampleData& s, Overview& out, std::array<Overview, 2>* sides) {
    out.fill(0);
    if (sides) { (*sides)[0].fill(0); (*sides)[1].fill(0); }
    uint32_t frames = std::min<uint32_t>(s.frames, uint32_t(std::min(s.left.size(), s.right.size())));
    if (!frames) return false;
    bool stereo = false;
    for (unsigned column = 0; column < out.size(); ++column) {
        unsigned from = unsigned(uint64_t(frames) * column / out.size()), to = unsigned(uint64_t(frames) * (column + 1) / out.size());
        float l = 0, r = 0;
        for (unsigned n = from; n < std::max(to, from + 1) && n < frames; ++n) {
            l = std::max(l, std::fabs(s.left[n]));
            r = std::max(r, std::fabs(s.right[n]));
            if (s.left[n] != s.right[n]) stereo = true;
        }
        auto byte = [](float v) { return uint8_t(std::lround(std::min(1.0f, v) * 255)); };
        out[column] = byte(std::max(l, r));
        if (sides) { (*sides)[0][column] = byte(l); (*sides)[1][column] = byte(r); }
    }
    return stereo;
}

} // namespace melodee
