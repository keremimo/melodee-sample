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
    std::ifstream file(path, std::ios::binary);
    if (!file) return "cannot open";
    uint8_t head[12];
    if (!file.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) return "not a WAV file";
    unsigned format = 0, channels = 0, rate = 0, bits = 0;
    std::vector<uint8_t> data;
    while (file) {
        uint8_t chunk[8];
        if (!file.read(reinterpret_cast<char*>(chunk), 8)) break;
        uint32_t size = u32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            std::vector<uint8_t> fmt(size);
            if (size < 16 || !file.read(reinterpret_cast<char*>(fmt.data()), size)) return "broken format chunk";
            format = u16(&fmt[0]); channels = u16(&fmt[2]); rate = u32(&fmt[4]); bits = u16(&fmt[14]);
            if (format == 0xfffe && size >= 26) format = u16(&fmt[24]); // WAVE_FORMAT_EXTENSIBLE: the sub-format
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (!format) return "data before format";
            uint64_t limit = uint64_t(max_seconds) * rate * channels * (bits / 8);
            auto after = file.tellg() + std::streamoff(uint64_t(size) + (size & 1));
            data.resize(size_t(std::min<uint64_t>(size, limit)));
            file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
            data.resize(size_t(file.gcount()));
            if (!info) break;
            file.clear(); // a smpl chunk may follow the audio
            file.seekg(after);
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
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 192000) return "unsupported channels or rate";
    unsigned bytes = bits / 8, frames = unsigned(data.size() / (bytes * channels));
    if (!frames) return "no audio";
    if (info) { info->channels = channels; info->rate = rate; }
    auto sample = [&](unsigned frame, unsigned channel) {
        const uint8_t* p = &data[(size_t(frame) * channels + channel) * bytes];
        if (floating) { float f; std::memcpy(&f, p, 4); return std::isfinite(f) ? f : 0.0f; }
        if (bits == 16) return int16_t(u16(p)) / 32768.0f;
        if (bits == 24) return int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) / 2147483648.0f;
        return int32_t(u32(p)) / 2147483648.0f;
    };
    // To 44.1 kHz, band-limited (roadmap P2; it was linear): straight through when it is at that rate already.
    std::vector<float> l(frames), r(frames);
    for (unsigned n = 0; n < frames; ++n) { l[n] = sample(n, 0); r[n] = sample(n, std::min(1u, channels - 1)); }
    adopt(out, std::move(l), std::move(r), rate, play_at);
    return {};
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

namespace {
// A WAV header for `frames` stereo frames at `rate` Hz into `h` (room for 58 bytes): 24-bit PCM, 44 bytes; or 32-bit
// float, 58 (its fmt chunk carries the extension's size and a fact chunk follows, as formats other than PCM take).
// Returns its length.
// Stereo: 32-bit float (floating), else integer PCM of `width` bytes (3: 24-bit; 2: 16-bit, roadmap P4's mixdown).
unsigned wav_header(unsigned char* h, uint32_t frames, bool floating, unsigned rate, unsigned int_width = 3) {
    auto put = [&](unsigned at, uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) h[at + n] = uint8_t(v >> (8 * n)); };
    const unsigned width = floating ? 4 : int_width, align = 2 * width;
    const uint32_t data = frames * align;
    std::memcpy(h, "RIFF", 4); std::memcpy(h + 8, "WAVEfmt ", 8);
    put(16, floating ? 18 : 16, 4); put(20, floating ? 3 : 1, 2); put(22, 2, 2); put(24, rate, 4); put(28, rate * align, 4);
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

bool WavStream::open(const std::string& path, unsigned at_rate, unsigned at_bits) {
    close();
    file = std::fopen(path.c_str(), "wb");
    written = 0; failed = !file; rate = at_rate;
    bits = at_bits == 16 || at_bits == 24 ? at_bits : 32;
    unsigned char h[58];
    unsigned size = wav_header(h, 0, bits == 32, rate, bits / 8);
    if (file && std::fwrite(h, 1, size, file) != size) failed = true;
    return !failed;
}
bool WavStream::add(const float* interleaved, unsigned frames) {
    if (!file || failed) return false;
    if (bits != 32) { // roadmap P4: integer PCM, TPDF dithered (two uniform draws a sample, a step either way), then clipped
        const unsigned width = bits / 8;
        const double scale = bits == 16 ? 32768.0 : 8388608.0, top = scale - 1;
        unsigned char bytes[6 * 256];
        for (unsigned at = 0; at < frames; at += 256) {
            unsigned n = std::min(256u, frames - at);
            for (unsigned f = 0; f < 2 * n; ++f) {
                float v = interleaved[2 * at + f];
                if (!std::isfinite(v)) v = 0;
                auto draw = [&] { noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; return double(noise) / 4294967296.0; };
                double d = draw() - draw(); // triangular, -1..1 of a step
                auto s = int32_t(std::clamp(std::floor(double(v) * scale + d + 0.5), -scale, top));
                for (unsigned k = 0; k < width; ++k) bytes[width * f + k] = uint8_t(uint32_t(s) >> (8 * k));
            }
            if (std::fwrite(bytes, 1, 2 * width * n, file) != 2 * width * n) { failed = true; return false; }
        }
        written += frames;
        return true;
    }
    // 32-bit float as it comes: a mix past full scale is kept whole, not clipped.
    unsigned char bytes[8 * 256];
    for (unsigned at = 0; at < frames; at += 256) {
        unsigned n = std::min(256u, frames - at);
        for (unsigned f = 0; f < 2 * n; ++f) float_bytes(interleaved[2 * at + f], bytes + 4 * f);
        if (std::fwrite(bytes, 1, 8 * n, file) != 8 * n) { failed = true; return false; }
    }
    written += frames;
    return true;
}
bool WavStream::close() {
    if (!file) return !failed;
    unsigned char h[58];
    unsigned size = wav_header(h, written, bits == 32, rate, bits / 8);
    if (std::fseek(file, 0, SEEK_SET) != 0 || std::fwrite(h, 1, size, file) != size) failed = true;
    if (std::fclose(file) != 0) failed = true;
    file = nullptr;
    return !failed;
}

void make_overview(const SampleData& s, Overview& out) {
    out.fill(0);
    if (!s.frames) return;
    for (unsigned column = 0; column < out.size(); ++column) {
        unsigned from = unsigned(uint64_t(s.frames) * column / out.size()), to = unsigned(uint64_t(s.frames) * (column + 1) / out.size());
        float peak = 0;
        for (unsigned n = from; n < std::max(to, from + 1) && n < s.frames; ++n) peak = std::max({peak, std::fabs(s.left[n]), std::fabs(s.right[n])});
        out[column] = uint8_t(std::lround(std::min(1.0f, peak) * 255));
    }
}

// Onsets: the level in 5 ms windows; a hit is a window much louder than the
// ones before it. Strength is how sharp the rise was.
unsigned find_hits(const SampleData& s, std::array<Hit, max_hits>& out) {
    constexpr unsigned window = 220;
    unsigned windows = s.frames / window;
    std::vector<float> level(windows);
    for (unsigned w = 0; w < windows; ++w) {
        float sum = 0;
        for (unsigned n = w * window; n < (w + 1) * window; ++n) sum += s.left[n] * s.left[n] + s.right[n] * s.right[n];
        level[w] = std::sqrt(sum / (2 * window));
    }
    std::vector<Hit> found;
    found.push_back({0, 255});
    unsigned quiet_until = 0;
    for (unsigned w = 4; w < windows; ++w) {
        float before = (level[w - 1] + level[w - 2] + level[w - 3] + level[w - 4]) / 4;
        float rise = level[w] - before;
        if (w < quiet_until || rise <= 0.01f || level[w] < before * 1.6f) continue;
        float strength = std::min(1.0f, rise / (level[w] + 1e-4f));
        found.push_back({w * window, uint8_t(std::lround(strength * 255))});
        quiet_until = w + 10; // 50 ms between hits
    }
    if (found.size() > out.size()) { // keep the strongest, then put them back in time order
        std::sort(found.begin() + 1, found.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        found.resize(out.size());
        std::sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.frame < b.frame; });
    }
    std::copy(found.begin(), found.end(), out.begin());
    return unsigned(found.size());
}

} // namespace melodee
