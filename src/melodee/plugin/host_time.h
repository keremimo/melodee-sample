// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Melodee's transport as a hosted plug-in hears it, read from the play head
// the rack gives every plug-in (PluginHandle, plugin/plugin_rack.cpp). VST3
// and LV2 plug-ins read that play head through JUCE; the VST2 host, the armv7
// bridge and the CLAP host pass this on in their own format. With no play
// head: stopped at the top, 120 BPM, 4/4.
#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <cstdint>

namespace melodee {

struct HostTime {
    bool playing = false;
    double ppq = 0, barStart = 0, bpm = 120, seconds = 0; // ppq and barStart: quarter notes from the song's top
    int64_t samples = 0;                                   // the same place, in the plug-in's frames
    int numerator = 4, denominator = 4;
};

inline HostTime hostTime (juce::AudioPlayHead* head)
{
    HostTime t;
    if (head == nullptr)
        return t;
    const auto position = head->getPosition();
    if (! position)
        return t;
    t.playing = position->getIsPlaying();
    if (const auto v = position->getPpqPosition()) t.ppq = *v;
    if (const auto v = position->getPpqPositionOfLastBarStart()) t.barStart = *v;
    if (const auto v = position->getBpm(); v && *v > 0) t.bpm = *v;
    if (const auto v = position->getTimeInSeconds()) t.seconds = *v;
    if (const auto v = position->getTimeInSamples()) t.samples = *v;
    if (const auto v = position->getTimeSignature(); v && v->numerator > 0 && v->denominator > 0)
    {
        t.numerator = v->numerator;
        t.denominator = v->denominator;
    }
    return t;
}

} // namespace melodee
