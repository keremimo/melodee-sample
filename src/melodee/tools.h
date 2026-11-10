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
//                                    clipsheet length rack sends bus fxpick
//                                    pads samples trim chop levels record
//                                    project filesheet tempo arp chord capture
//                                    pad play envelope filter follow cloud shift exit
//                                    Fails when text is drawn under 11 px, wider than
//                                    its box, or a header's two sides meet.
//   --melodee-sound FILE.wav         plays the starting project for two bars and
//                                    checks it sounds and stays inside full scale
//   --melodee-bench ROUNDS [PAGE...] times drawing each page, ROUNDS draws apiece
// Returns the exit code, or nothing when argv names neither.
namespace melodee {
std::optional<int> run_tool(int argc, char** argv);
}
