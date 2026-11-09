// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Rubik (SIL Open Font License 1.1, The Rubik Project Authors), embedded so
// the binary carries Melodee's typeface to the MPC and to host previews.
// src/melodee/dependencies.py fetches the pinned file; the assembler reads
// it from the build folder (make runs at the repository root).
#include "melodee/ui/rubik_data.h"

asm(R"(
    .section .rodata
    .balign 16
    .global melodee_rubik_start
melodee_rubik_start:
    .incbin "build/deps/rubik/Rubik-VF.ttf"
    .global melodee_rubik_end
melodee_rubik_end:
    .previous
)");
