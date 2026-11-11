// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/ui/paint.h"
#include <cstdint>

// Melodee's glyphs: small line drawings that stand in for words (a card, a
// computer, eject, the monitor, a cut, the effect families). Each is drawn on
// a 24 unit grid from SVG path data, so the screens and the design boards
// share one set. Screen thread (and the host preview tool) only.
namespace melodee::ui {

enum class Icon : uint8_t {
    None,
    Play, Stop, Rec, Back, Check, Cross, Plus, Shift, Power,
    Card, Laptop, Usb, Eject, Mic, Jack, Speaker, SpeakerOff,
    Loop, OneShot, Reverse, Trash, Undo, Folder, Plug, PlugBroken,
    Knob, Lock, Warn, Pads, Scissors, Wav, Pattern,
    Scale, Chromatic, Chord, Velocity, Fader, Ring, Push, Mpc,
    Note, Motion, ArrowUp, Space, Time, Modulation, Dirt, Tone, Dynamics,
    // Round 2: the engines, the transport's count, the sheets' keys and the rest of the boards' set.
    Minus, ArrowDown, ArrowRight, ChevronLeft, ChevronRight,
    Star, Saw, Sine, Fm, Chip, Drum, Din, Keyboard,
    Save, Copy, Flag, Magnet, Metro, Tie, Slide, Accent, Pin, Gear, Info,
    Cloud, Tape, Clock, Swap, Route, Bars, Sliders, Layers, Screen, File, Wheel,
    ArrowLeft, SpaceBar,
    Count
};

// Draws `icon` in a `size` square with its top left at x, y; lines `stroke`
// device pixels wide whatever the size.
void icon(Paint& p, Icon icon, float x, float y, float size, juce::Colour c, float stroke = 1.4f);

// SVG path data (absolute M L H V C Q A Z) as a JUCE path: the part of
// juce_gui_basics' Drawable::parseSVGPath the glyphs need, kept here so the
// app links juce_graphics alone.
juce::Path svg_path(const char* data);

} // namespace melodee::ui
