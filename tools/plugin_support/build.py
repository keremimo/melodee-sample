#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Make the plug-in support pack Melodee carries inside its binary
(src/melodee/plugin/plugin_support.h):

  stubs/            no-op GUI libraries (make_stubs.py)
  bridge32/vst2_bridge32   the 32-bit bridge (src/melodee/plugin/bridge32/)
  bridge32/armhf/   Debian bookworm's armhf loader, glibc, libstdc++ and
                    libgcc_s, which the bridge and armv7 plug-ins run on
  libs/             real aarch64 libraries plug-ins use for sound and the MPC
                    lacks: libsndfile and its codecs, libsamplerate and
                    FFTW (single and double, with their threads
                    builds) (SURVEY.md), loaded
                    before any plug-in
  licenses/         their Debian copyright files, the LGPL and GPL texts and
                    the package versions (the release zip carries them too)

Run by the Makefile inside Melodee's build image (tools/plugin_support/Dockerfile):
    python3 tools/plugin_support/build.py build/plugin-support build/plugin-support.pack
The pack: "MPS1", a file count, then each file's name length, name, mode,
size and bytes (little-endian 32-bit words).
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ARMHF_LIB = Path("/usr/arm-linux-gnueabihf/lib")
RUNTIME = ["ld-linux-armhf.so.3", "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
           "libstdc++.so.6", "libgcc_s.so.1"]


def bridge(out):
    compiler = shutil.which("arm-linux-gnueabihf-g++")
    if not compiler:
        raise SystemExit("no arm-linux-gnueabihf-g++: build in Melodee's image (tools/plugin_support/Dockerfile)")
    (out / "armhf").mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'src'}",
                    "-o", str(out / "vst2_bridge32"), str(ROOT / "src/melodee/plugin/bridge32/vst2_bridge32.cpp"), "-ldl"],
                   check=True)
    for name in RUNTIME:
        shutil.copyfile(ARMHF_LIB / name, out / "armhf" / name)
        (out / "armhf" / name).chmod(0o755)


# Real libraries for plug-ins' sound the MPC lacks (tools/plugin_support/SURVEY.md): libsndfile (LGPL-2.1+) and the
# codecs it links that the MPC has not (FLAC, Ogg, Vorbis, Opus: BSD; LAME: LGPL-2.0+). libmpg123 the MPC has.
# libsamplerate (BSD-2-Clause) and FFTW 3 (GPL-2+), double and single precision with their threads builds; not the
# OpenMP builds, which need libgomp. Loaded in this order, each before what needs it.
AARCH64_LIB = Path("/usr/lib/aarch64-linux-gnu")
REAL = ["libogg.so.0", "libFLAC.so.12", "libvorbis.so.0", "libvorbisenc.so.2", "libopus.so.0", "libmp3lame.so.0",
        "libsndfile.so.1", "libsamplerate.so.0", "libfftw3.so.3", "libfftw3_threads.so.3", "libfftw3f.so.3",
        "libfftw3f_threads.so.3"]
REAL_PACKAGES = ["libogg0", "libflac12", "libvorbis0a", "libvorbisenc2", "libopus0", "libmp3lame0", "libsndfile1",
                 "libsamplerate0", "libfftw3-double3", "libfftw3-single3"]


def real_libs(out):
    out.mkdir(parents=True, exist_ok=True)
    for n, name in enumerate(REAL):
        shutil.copyfile(AARCH64_LIB / name, out / f"{n:02d}-{name}") # the number keeps the order
        (out / f"{n:02d}-{name}").chmod(0o755)


# The armhf runtime's terms: glibc under the LGPL, libstdc++ and libgcc_s under the GPL with the GCC Runtime Library
# Exception (in their copyright files). Sources: these package versions on snapshot.debian.org.
PACKAGES = ["libc6-armhf-cross", "libstdc++6-armhf-cross", "libgcc-s1-armhf-cross"]


def licenses(out):
    out.mkdir(parents=True, exist_ok=True)
    versions = []
    for package in PACKAGES + REAL_PACKAGES:
        shutil.copyfile(Path("/usr/share/doc") / package / "copyright", out / f"{package}.copyright")
        version = subprocess.run(["dpkg-query", "-W", "-f=${Version}", package], capture_output=True, text=True, check=True).stdout
        versions.append(f"{package} {version}")
    for text in ["LGPL-2.1", "GPL-2", "GPL-3"]:
        shutil.copyfile(Path("/usr/share/common-licenses") / text, out / f"{text}.txt")
    (out / "README.txt").write_text(
        "Melodee carries Debian bookworm's armhf runtime for 32-bit plug-ins (the loader, glibc,\n"
        "libstdc++ and libgcc_s), unchanged. glibc is under the LGPL 2.1 or later; libstdc++ and\n"
        "libgcc_s under the GPL 3 with the GCC Runtime Library Exception (see the copyright files).\n"
        "It also carries Debian bookworm's aarch64 libsndfile (LGPL 2.1 or later) with libFLAC, libogg,\n"
        "libvorbis, libvorbisenc, libopus (BSD-style) and libmp3lame (LGPL 2 or later), libsamplerate\n"
        "(BSD 2-clause) and FFTW 3 (GPL 2 or later), unchanged.\n"
        "Their source is these Debian packages' source, on snapshot.debian.org:\n" + "".join(f"  {v}\n" for v in versions) +
        "It is also available on request from the Melodee project for three years from each release.\n")


def pack(folder, target):
    files = sorted(p for p in folder.rglob("*") if p.is_file())
    data = bytearray(b"MPS1") + struct.pack("<I", len(files))
    for path in files:
        name = path.relative_to(folder).as_posix().encode()
        body = path.read_bytes()
        data += struct.pack("<I", len(name)) + name + struct.pack("<II", path.stat().st_mode & 0o777, len(body)) + body
    target.write_bytes(bytes(data))
    print(f"{target}: {len(files)} files, {len(data) // 1024} KiB")


def main():
    folder = Path(sys.argv[1] if len(sys.argv) > 1 else "build/plugin-support")
    target = Path(sys.argv[2] if len(sys.argv) > 2 else "build/plugin-support.pack")
    if folder.exists():
        shutil.rmtree(folder)
    subprocess.run([sys.executable, str(ROOT / "tools/plugin_support/make_stubs.py"), str(folder / "stubs")], check=True)
    bridge(folder / "bridge32")
    real_libs(folder / "libs")
    licenses(folder / "licenses")
    pack(folder, target)


if __name__ == "__main__":
    main()
