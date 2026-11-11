/* SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * The plug-in support pack (tools/plugin_support/build.py) inside Melodee's
 * binary, read by src/melodee/plugin/plugin_support.cpp. Assembled from the
 * repository root, where build/plugin-support.pack is. */
    .section .rodata
    .balign 16
    .global melodee_plugin_support
    .global melodee_plugin_support_end
melodee_plugin_support:
    .incbin "build/plugin-support.pack"
melodee_plugin_support_end:
    .section .note.GNU-stack,"",%progbits
