// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace melodee {
namespace {
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
} // namespace

std::string read_wav(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info) {
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
    // To 44.1 kHz, linearly; enough for one-shots and loops alike.
    double ratio = double(rate) / 44100.0;
    unsigned length = unsigned(frames / ratio);
    out.left.assign(length, 0.0f); out.right.assign(length, 0.0f); out.frames = length;
    for (unsigned n = 0; n < length; ++n) {
        double at = n * ratio;
        unsigned i = unsigned(at);
        float frac = float(at - i);
        unsigned j = std::min(i + 1, frames - 1);
        for (unsigned ch = 0; ch < 2; ++ch) {
            unsigned c = std::min(ch, channels - 1);
            float v = sample(i, c) + (sample(j, c) - sample(i, c)) * frac;
            (ch ? out.right : out.left)[n] = v;
        }
    }
    return {};
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

std::string write_wav(const std::string& path, const SampleData& s) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return "cannot write";
    auto put = [&](uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) file.put(char(v >> (8 * n))); };
    uint32_t data = s.frames * 6;
    file.write("RIFF", 4); put(36 + data, 4); file.write("WAVEfmt ", 8);
    put(16, 4); put(1, 2); put(2, 2); put(44100, 4); put(44100 * 6, 4); put(6, 2); put(24, 2);
    file.write("data", 4); put(data, 4);
    for (uint32_t n = 0; n < s.frames; ++n)
        for (float v : {s.left[n], s.right[n]}) put(uint32_t(int32_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 8388607.0f))), 3);
    return file.good() ? std::string() : std::string("write failed");
}

namespace {
void wav_header(unsigned char* h, uint32_t frames) {
    auto put = [&](unsigned at, uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) h[at + n] = uint8_t(v >> (8 * n)); };
    uint32_t data = frames * 6;
    std::memcpy(h, "RIFF", 4); put(4, 36 + data, 4); std::memcpy(h + 8, "WAVEfmt ", 8);
    put(16, 16, 4); put(20, 1, 2); put(22, 2, 2); put(24, 44100, 4); put(28, 44100 * 6, 4); put(32, 6, 2); put(34, 24, 2);
    std::memcpy(h + 36, "data", 4); put(40, data, 4);
}
} // namespace

bool WavStream::open(const std::string& path) {
    close();
    file = std::fopen(path.c_str(), "wb");
    written = 0; failed = !file;
    unsigned char h[44];
    wav_header(h, 0);
    if (file && std::fwrite(h, 1, sizeof(h), file) != sizeof(h)) failed = true;
    return !failed;
}
bool WavStream::add(const float* interleaved, unsigned frames) {
    if (!file || failed) return false;
    unsigned char bytes[6 * 256];
    for (unsigned at = 0; at < frames; at += 256) {
        unsigned n = std::min(256u, frames - at);
        for (unsigned f = 0; f < 2 * n; ++f) {
            auto v = uint32_t(int32_t(std::lround(std::clamp(interleaved[2 * at + f], -1.0f, 1.0f) * 8388607.0f)));
            bytes[3 * f] = uint8_t(v); bytes[3 * f + 1] = uint8_t(v >> 8); bytes[3 * f + 2] = uint8_t(v >> 16);
        }
        if (std::fwrite(bytes, 1, 6 * n, file) != 6 * n) { failed = true; return false; }
    }
    written += frames;
    return true;
}
bool WavStream::close() {
    if (!file) return !failed;
    unsigned char h[44];
    wav_header(h, written);
    if (std::fseek(file, 0, SEEK_SET) != 0 || std::fwrite(h, 1, sizeof(h), file) != sizeof(h)) failed = true;
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
