#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Write src/melodee/ui/boot_logo.cpp and boot_backdrop.cpp from the boot
animation's two 320 x 240 PNGs: the transparent logo and the backdrop behind
it. Neither image is covered by the AGPL (THIRD_PARTY.md, Boot art), so
each gets a file of its own, apart from the code.

The repository keeps source text only (base/tools/check_private_files.py),
so the art lives in the source as Base64 PNG, decoded once on the screen
thread (ui/boot.cpp). Only the image chunks are kept: no text, time or
colour-profile metadata comes along.

    python3 tools/boot_art.py LOGO.png BACKDROP.png
"""
from pathlib import Path
import base64
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "src/melodee/ui"
SIGNATURE = b"\x89PNG\r\n\x1a\n"
KEEP = {b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND"}


def image_chunks(path):
    data = Path(path).read_bytes()
    if not data.startswith(SIGNATURE):
        raise SystemExit(f"{path}: not a PNG")
    at, out = len(SIGNATURE), [SIGNATURE]
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[at + 8:at + 16])
            if (width, height) != (320, 240):
                raise SystemExit(f"{path}: {width} x {height}, the screen is 320 x 240")
        if kind in KEEP:
            body = data[at + 8:at + 8 + length]
            out.append(struct.pack(">I4s", length, kind) + body + struct.pack(">I", zlib.crc32(kind + body)))
        at += 12 + length
    return b"".join(out)


def literal(name, png, what):
    text = base64.b64encode(png).decode()
    lines = [f'    "{text[n:n + 100]}"' for n in range(0, len(text), 100)]
    return f"// {what}: {len(png)} bytes of PNG.\nconst char {name}[] =\n" + "\n".join(lines) + ";\n"


BACKDROP = ("// SPDX-License-Identifier: LicenseRef-Melodee-Backdrop\n"
            "// Copyright (C) Yasmine Elsen (https://www.smallcutekitty.net). All rights reserved.\n"
            "// Made for Kerem Kilic (Ellic Studio), who alone may use it.\n"
            "// The boot backdrop is not covered by the AGPL. A fork may keep this file, but no public\n"
            "// release of a fork may contain it; see THIRD_PARTY.md, Boot art.\n")
RESERVED = ("// SPDX-License-Identifier: LicenseRef-Melodee-Logo\n"
            "// Copyright (C) 2026 Kerem Kilic (Ellic Studio). All rights reserved.\n"
            "// The Melodee logo is not covered by the AGPL. A fork may keep this file, but no public\n"
            "// release of a fork may contain it; see THIRD_PARTY.md, Boot art.\n")


def write(name, header, body):
    out = UI / name
    out.write_text(header + "// Written by tools/boot_art.py; run it again rather than editing this file.\n"
                   '#include "melodee/ui/boot_art.h"\n\nnamespace melodee::ui {\n\n' + body + "\n} // namespace melodee::ui\n")
    print(f"wrote {out.relative_to(ROOT)}")


def main(logo, backdrop):
    write("boot_logo.cpp", RESERVED, literal("boot_logo_png", image_chunks(logo), "The Melodee logo on transparency"))
    write("boot_backdrop.cpp", BACKDROP, literal("boot_backdrop_png", image_chunks(backdrop), "The night city behind it"))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
