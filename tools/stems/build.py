#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Build the stem separator Melodee runs beside itself (src/melodee/shell/stems.h):

  melodee-stems         tools/stems/stems_engine.c, Melodee's own, on SpleeterRT's
                        network and STFT code (GPL-3.0, unchanged), linked with a
                        static OpenBLAS (BSD-3-Clause) built for the MPC's
                        Cortex-A55 cores
  spleeter-4stems.f16   Deezer's Spleeter 4-stem model (MIT) as that program reads
                        it (tools/stems/convert_model.py)
  licenses/             their licence texts and where their sources are (the
                        release zip carries them)

Run by the Makefile (`make stems`) inside Melodee's build image:
    python3 tools/stems/build.py build/stems
The sources come from build/deps/spleeterrt, build/deps/openblas and
build/deps/spleeter-4stems, which src/melodee/dependencies.py fetches at pinned
commits and checksums.

Both are files of their own in the release (autorun/melodee/stems/), not part of
Melodee's binary: 80 MB that would sit in RAM the whole session.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SPLEETER = ROOT / "build/deps/spleeterrt"
OPENBLAS = ROOT / "build/deps/openblas"
MODEL = ROOT / "build/deps/spleeter-4stems"
# SpleeterRT's network, its matrix products and its STFT; not its main.c (the driver is stems_engine.c).
SOURCES = ["spleeter.c", "gemm.c", "im2col_dilated.c", "codelet.c", "stftFix.c", "cpthread.c"]
# One core's kernels, C BLAS alone (no LAPACK, so no Fortran), a static library. Threaded as the network code
# expects: the driver asks for one OpenBLAS thread and calls sgemm from its own.
OPENBLAS_FLAGS = ["TARGET=CORTEXA55", "ONLY_CBLAS=1", "NO_LAPACK=1", "NO_FORTRAN=1", "NO_SHARED=1", "NUM_THREADS=4"]
CFLAGS = ["-O2", "-mcpu=cortex-a55"] # -O3 measured no faster on the MPC

MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


def revision(path):
    """The commit src/melodee/dependencies.py fetched and checked (git itself refuses another user's checkout)."""
    marker = path / "MELODEE_REVISION"
    if not marker.is_file():
        raise SystemExit(f"no {marker.relative_to(ROOT)}: run python3 tools/dependencies.py")
    return marker.read_text().strip()


def openblas(out):
    """The static library and its headers, into out/openblas."""
    prefix = out / "openblas"
    if (prefix / "lib/libopenblas.a").is_file() and (prefix / "revision").is_file() \
            and (prefix / "revision").read_text().strip() == revision(OPENBLAS):
        return prefix
    jobs = f"-j{os.cpu_count() or 2}"
    subprocess.run(["make", "-C", str(OPENBLAS), jobs, *OPENBLAS_FLAGS, "libs"], check=True)
    if prefix.exists():
        shutil.rmtree(prefix)
    subprocess.run(["make", "-C", str(OPENBLAS), *OPENBLAS_FLAGS, f"PREFIX={prefix}", "install"], check=True)
    (prefix / "revision").write_text(revision(OPENBLAS) + "\n")
    return prefix


def program(out, blas):
    source = SPLEETER / "Executable"
    objects = out / "obj"
    if objects.exists(): # each time: an object kept from an earlier run may have been made with other flags
        shutil.rmtree(objects)
    objects.mkdir(parents=True)
    # gemm.c includes Intel's <mkl.h> on anything but Android and macOS; this one hands it OpenBLAS's cblas instead.
    shim = out / "include"
    shim.mkdir(parents=True, exist_ok=True)
    (shim / "mkl.h").write_text("#include <cblas.h>\n")
    include = [f"-I{shim}", f"-I{blas / 'include'}", f"-I{source}"]
    made = []
    for name in SOURCES:
        target = objects / (name[:-2] + ".o")
        subprocess.run(["gcc", *CFLAGS, "-w", *include, "-c", str(source / name), "-o", str(target)], check=True)
        made.append(str(target))
    # dr_wav (the WAV reader and writer SpleeterRT carries) is a header: its code is compiled here.
    wav = objects / "dr_wav.c"
    wav.write_text('#define DR_WAV_IMPLEMENTATION\n#include "dr_wav.h"\n')
    subprocess.run(["gcc", *CFLAGS, "-w", *include, "-c", str(wav), "-o", str(objects / "dr_wav.o")], check=True)
    made.append(str(objects / "dr_wav.o"))
    entry = objects / "stems_engine.o"
    subprocess.run(["gcc", *CFLAGS, "-Wall", "-Wextra", "-Werror", *include, "-c", str(ROOT / "tools/stems/stems_engine.c"), "-o", str(entry)], check=True)
    binary = out / "melodee-stems"
    subprocess.run(["gcc", "-o", str(binary), str(entry), *made, str(blas / "lib/libopenblas.a"), "-lm", "-lpthread"], check=True)
    subprocess.run(["strip", str(binary)], check=True)
    print(f"{binary}: {binary.stat().st_size // 1024} KiB")


def model(out):
    """Deezer's checkpoint as the program reads it; made again when the converter changes."""
    target, converter = out / "spleeter-4stems.f16", ROOT / "tools/stems/convert_model.py"
    if target.is_file() and target.stat().st_mtime >= converter.stat().st_mtime:
        return
    subprocess.run([sys.executable, str(converter), str(MODEL), str(target)], check=True)


def licenses(out):
    folder = out / "licenses"
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    shutil.copyfile(SPLEETER / "LICENSE", folder / "SpleeterRT-GPL-3.0.txt")
    shutil.copyfile(OPENBLAS / "LICENSE", folder / "OpenBLAS-LICENSE.txt")
    (folder / "Spleeter-model-MIT.txt").write_text("MIT License\n\nCopyright (c) 2019-present, Deezer SA.\n\n" + MIT)
    (folder / "README.txt").write_text(
        "Melodee's STEMS runs a program of its own, melodee-stems, with the model spleeter-4stems.f16.\n\n"
        "The program is tools/stems/stems_engine.c (Melodee's own, AGPL 3.0) on the network and STFT code of\n"
        "SpleeterRT, by James Fung, under the GPL 3.0 (SpleeterRT-GPL-3.0.txt), unchanged:\n"
        f"  https://github.com/james34602/SpleeterRT commit {revision(SPLEETER)}\n"
        "  (its spleeter.c, gemm.c, im2col_dilated.c, codelet.c, stftFix.c and cpthread.c, and dr_wav by\n"
        "  David Reid, public domain or MIT No Attribution)\n"
        "OpenBLAS, under the BSD 3-clause licence (OpenBLAS-LICENSE.txt), is linked in:\n"
        f"  https://github.com/OpenMathLib/OpenBLAS commit {revision(OPENBLAS)}\n"
        "The model is the 4-stem model of Spleeter, trained by Deezer, under the MIT licence\n"
        "(Spleeter-model-MIT.txt), its weights laid out anew by tools/stems/convert_model.py:\n"
        f"  https://github.com/deezer/spleeter/releases/download/v1.4.0/4stems.tar.gz (SHA-256 {revision(MODEL)})\n"
        "The source of all of it is at those addresses and in Melodee's own, and is also available on\n"
        "request from the Melodee project for three years from each release.\n")


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/stems").resolve()
    for path in (SPLEETER, OPENBLAS, MODEL):
        revision(path)
    out.mkdir(parents=True, exist_ok=True)
    program(out, openblas(out))
    model(out)
    licenses(out)


if __name__ == "__main__":
    main()
