// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// An SFZ's samples, whatever kind they are: WAV by the sampler's own reader
// (it also finds the loop a smpl chunk holds), FLAC, Ogg and AIFF by JUCE's,
// and MP3 (an MPC expansion's program previews, a pad's sample) by JUCE's too.
#include "melodee/shell/sample_decode.h"
#include "melodee/engine/dsp.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace melodee {
namespace {
bool juce_kind(const std::string& path) {
    auto extension = juce::File(path).getFileExtension().toLowerCase();
    return extension == ".flac" || extension == ".ogg" || extension == ".oga" || extension == ".aif" || extension == ".aiff" || extension == ".mp3";
}

// JUCE's MP3 decoder, as every one built on the reference filter bank, runs 529 frames late on top of the
// encoder's own delay; with a LAME tag both come off the start, the encoder's padding off the end. Its reader
// counts 1152 frames for every MPEG frame, twice what an MPEG 2 or 2.5 one (16-24 kHz) holds.
constexpr unsigned mp3_decoder_delay = 529;

juce::AudioFormatManager& formats() { // service thread only
    static auto* manager = [] {
        auto* m = new juce::AudioFormatManager;
        m->registerFormat(new juce::FlacAudioFormat, false);
        m->registerFormat(new juce::OggVorbisAudioFormat, false);
        m->registerFormat(new juce::AiffAudioFormat, false);
#if JUCE_USE_MP3AUDIOFORMAT
        m->registerFormat(new juce::MP3AudioFormat, false);
#endif
        return m;
    }();
    return *manager;
}
} // namespace

std::string read_audio(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info, float play_at) {
    if (!juce_kind(path)) return read_wav(path, out, max_seconds, info, play_at);
    AudioReader reader;
    auto error = reader.open(path, max_seconds, info);
    if (!error.empty()) return error;
    std::vector<float> l(reader.frames()), r(reader.frames());
    l.resize(reader.read(l.data(), r.data(), reader.frames()));
    r.resize(l.size());
    if (reader.failed()) return "cannot read it";
    if (l.empty()) return "no audio";
    // To the project's rate, band-limited, as read_wav does (roadmap P2).
    adopt(out, std::move(l), std::move(r), reader.rate(), play_at);
    return {};
}

struct AudioReader::Juce {
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> audio;
    unsigned channels = 0;
    juce::int64 start = 0;
    bool mp3 = false;
};

AudioReader::AudioReader() = default;
AudioReader::~AudioReader() = default;

std::string AudioReader::open(const std::string& path, unsigned max_seconds, WavInfo* info) {
    juce.reset();
    hz = count = done = 0;
    broken = false;
    if (!juce_kind(path)) {
        auto error = wav.open(path, max_seconds, info);
        if (error.empty()) { hz = wav.rate(); count = wav.frames(); }
        return error;
    }
    auto j = std::make_unique<Juce>();
    j->reader.reset(formats().createReaderFor(juce::File(path)));
    auto& reader = j->reader;
    if (!reader || reader->sampleRate < 8000 || reader->sampleRate > 192000 || !reader->numChannels) return "unsupported file";
    j->channels = std::min(reader->numChannels, 2u);
    juce::int64 length = reader->lengthInSamples;
    Mp3Head mp3;
    j->mp3 = juce::File(path).hasFileExtension("mp3") && read_mp3_head(path, mp3);
    if (j->mp3) {
        length = length / 1152 * mp3.frame_samples;
        if (mp3.gapless) {
            j->start = std::min<juce::int64>(mp3.delay + mp3_decoder_delay, length);
            length = std::min(length - j->start, length - mp3.delay - mp3.padding);
        }
    }
    const auto frames = std::min<juce::int64>(length, juce::int64(max_seconds * reader->sampleRate));
    if (frames <= 0) return "no audio";
    if (info) {
        info->channels = j->channels; info->rate = unsigned(reader->sampleRate);
        read_flac_loop(path, *info); // nothing to find in anything but a FLAC made from a looped WAV
    }
    hz = unsigned(std::lround(reader->sampleRate));
    count = uint32_t(frames);
    juce = std::move(j);
    return {};
}

uint32_t AudioReader::read(float* left, float* right, uint32_t frames) {
    frames = std::min(frames, count - done);
    if (!frames) return 0;
    if (!juce) {
        const uint32_t got = wav.read(left, right, frames);
        if (got < frames) count = done + got;
        done += got;
        return got;
    }
    auto& j = *juce;
    auto n = int(frames);
    j.audio.setSize(int(j.channels), n, false, false, true);
    // In order, so each piece goes on from where the last one ended: JUCE's readers carry on, never seeking.
    if (!j.reader->read(&j.audio, 0, n, j.start + done, true, j.channels > 1)) {
        // An MP3 without a frame count (no Xing tag) has its length guessed from its size; what is read past its
        // end comes back as zeros, which go, and the file ends there.
        if (j.mp3) while (n > 0 && j.audio.getSample(0, n - 1) == 0 && j.audio.getSample(int(j.channels - 1), n - 1) == 0) --n;
        else { broken = true; n = 0; }
        count = done + uint32_t(n);
    }
    const float* l = j.audio.getReadPointer(0);
    const float* r = j.audio.getReadPointer(int(j.channels - 1));
    std::copy(l, l + n, left);
    std::copy(r, r + n, right);
    done += uint32_t(n);
    return uint32_t(n);
}

} // namespace melodee
