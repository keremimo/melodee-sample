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
    std::unique_ptr<juce::AudioFormatReader> reader(formats().createReaderFor(juce::File(path)));
    if (!reader || reader->sampleRate < 8000 || reader->sampleRate > 192000 || !reader->numChannels) return "unsupported file";
    const unsigned channels = std::min(reader->numChannels, 2u);
    juce::int64 start = 0, length = reader->lengthInSamples;
    Mp3Head mp3;
    const bool is_mp3 = juce::File(path).hasFileExtension("mp3") && read_mp3_head(path, mp3);
    if (is_mp3) {
        length = length / 1152 * mp3.frame_samples;
        if (mp3.gapless) {
            start = std::min<juce::int64>(mp3.delay + mp3_decoder_delay, length);
            length = std::min(length - start, length - mp3.delay - mp3.padding);
        }
    }
    auto frames = int(std::min<juce::int64>(length, juce::int64(max_seconds * reader->sampleRate)));
    if (frames <= 0) return "no audio";
    juce::AudioBuffer<float> audio(int(channels), frames);
    if (!reader->read(&audio, 0, frames, start, true, channels > 1)) {
        // An MP3 without a frame count (no Xing tag) has its length guessed from its size; what is read past its
        // end comes back as zeros, which go.
        if (!is_mp3) return "cannot read it";
        while (frames > 0 && audio.getSample(0, frames - 1) == 0 && audio.getSample(int(channels - 1), frames - 1) == 0) --frames;
        if (frames <= 0) return "no audio";
    }
    if (info) {
        info->channels = channels; info->rate = unsigned(reader->sampleRate);
        read_flac_loop(path, *info); // nothing to find in anything but a FLAC made from a looped WAV
    }
    // To the project's rate, band-limited, as read_wav does (roadmap P2).
    const float* l = audio.getReadPointer(0);
    const float* r = audio.getReadPointer(int(channels - 1));
    const auto rate = unsigned(std::lround(reader->sampleRate));
    adopt(out, std::vector<float>(l, l + frames), std::vector<float>(r, r + frames), rate, play_at);
    return {};
}

} // namespace melodee
