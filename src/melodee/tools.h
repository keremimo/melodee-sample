// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <optional>

// Melodee's one-shot host tools, for build checks and design review. None
// opens the MPC's audio, display or control surface.
//   --melodee-preview DIR [PAGE...]  draws pages to DIR/<page>.png at 320 x 240
//                                    and DIR/<page>@2x.png at the boards' 640 x 480.
//                                    Pages: stage keys sounds edit soundmap mod touch mix steps roll
//                                    notesheet motion patterns song songzoom
//                                    songclip songshift length rack sends bus fxpick
//                                    pads samples trim chop levels record
//                                    project filesheet tempo arp chord capture
//                                    pad play envelope filter follow cloud shift exit
//                                    Fails when text is drawn under 11 px, wider than
//                                    its box, or a header's two sides meet.
//   --melodee-sound FILE.wav         plays the starting project for two bars and
//                                    checks it sounds and stays inside full scale
//   --self-test FILE.wav             the same check, under the name device.py and
//                                    the SD boot run before leaving the stock app
//   --melodee-boot DIR               draws the boot animation to DIR/boot-NNN.png, 30 a second
//   --melodee-bench ROUNDS [PAGE...] times drawing each page, ROUNDS draws apiece
//   --melodee-dsp-bench [KEY=VALUE...] a busy project rendered a block a period at real-time priority, with
//                                    the render workers: block times and CPU (src/melodee/dsp_bench.cpp)
//   --melodee-print-spike MODE [KEY=VALUE...] freeze plan F0: a track rendered by a second engine host
//                                    (equal, speed, plugin, stream; src/melodee/print_spike.cpp)
//   --mpc-import FILE.xpj|.xpm|.xty  reads an MPC Sample project or an MPC program and prints what came across
//   --mpc-install FILE.xpn DIR       installs an MPC expansion into DIR/<its title>
//   --instrument-read FILE           reads an .sfz, .xpm, .xty or .exs as the sample library does
//                                    and prints its zones, their samples found
//   --melodee-monkey [SEED COUNT MOVES]  random presses, holds and releases of every control
//                                    (src/melodee/monkey.cpp); fails when a layer, a note, a
//                                    held step or pattern outlives the hand that held it
// Returns the exit code, or nothing when argv names neither.
namespace melodee {
std::optional<int> run_tool(int argc, char** argv);
int monkey(unsigned first, unsigned seeds, unsigned moves); // --melodee-monkey: seeds first.., the failures' moves printed
int dsp_bench(int argc, char** argv); // --melodee-dsp-bench
int print_spike(int argc, char** argv); // --melodee-print-spike
}
