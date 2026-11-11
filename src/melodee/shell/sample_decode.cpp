// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// An SFZ's samples, whatever kind they are: WAV by the sampler's own reader
// (it also finds the loop a smpl chunk holds), FLAC, Ogg and AIFF by JUCE's.
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
    return extension == ".flac" || extension == ".ogg" || extension == ".oga" || extension == ".aif" || extension == ".aiff";
}

juce::AudioFormatManager& formats() { // service thread only
    static auto* manager = [] {
        auto* m = new juce::AudioFormatManager;
        m->registerFormat(new juce::FlacAudioFormat, false);
        m->registerFormat(new juce::OggVorbisAudioFormat, false);
        m->registerFormat(new juce::AiffAudioFormat, false);
        return m;
    }();
    return *manager;
}
} // namespace

std::string read_audio(const std::string& path, SampleData& out, unsigned max_seconds, WavInfo* info) {
    if (!juce_kind(path)) return read_wav(path, out, max_seconds, info);
    std::unique_ptr<juce::AudioFormatReader> reader(formats().createReaderFor(juce::File(path)));
    if (!reader || reader->sampleRate < 8000 || reader->sampleRate > 192000 || !reader->numChannels) return "unsupported file";
    const unsigned channels = std::min(reader->numChannels, 2u);
    const auto frames = int(std::min<juce::int64>(reader->lengthInSamples, juce::int64(max_seconds * reader->sampleRate)));
    if (frames <= 0) return "no audio";
    juce::AudioBuffer<float> audio(int(channels), frames);
    if (!reader->read(&audio, 0, frames, 0, true, channels > 1)) return "cannot read it";
    if (info) { info->channels = channels; info->rate = unsigned(reader->sampleRate); }
    // To the project's rate, linearly, as read_wav does.
    const double ratio = reader->sampleRate / double(dsp::model_rate);
    const auto length = uint32_t(frames / ratio);
    out.left.assign(length, 0.0f); out.right.assign(length, 0.0f); out.frames = length;
    for (uint32_t n = 0; n < length; ++n) {
        double at = n * ratio;
        int i = int(at);
        float frac = float(at - i);
        int j = std::min(i + 1, frames - 1);
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float* side = audio.getReadPointer(int(std::min(ch, channels - 1)));
            (ch ? out.right : out.left)[n] = side[i] + (side[j] - side[i]) * frac;
        }
    }
    return {};
}

} // namespace melodee
