# Plug-in library survey

What ARM Linux plug-ins need from the system, against what the MPC Sample
has, to decide which libraries Melodee's plug-in support pack carries
(`build.py`). Taken 2026-11-11.

## What was looked at

43 archives, 265 shared libraries, 40 ARM plug-in binaries (VST2 / VST3):

- Open MPC (open-mpc.github.io): the two smallest builds of every author
  with aarch64 builds (20 archives, 13 authors) and of every author with
  three or more armv7-only builds (20 archives). Every catalog sha256
  matched.
- LSP Plugins 1.2.37 (Linux aarch64), Wolf Shaper 1.0.2 (linux arm64),
  Ildaeil 1.3 (linux aarch64); DPF-Plugins 1.7 and Dragonfly Reverb 3.2.10
  from the first tests.
- Not available as ARM Linux VST builds on their release pages: Surge XT,
  Vitalium / DISTRHO-Ports, Dexed, OB-Xf, Odin 2, Airwindows Consolidated,
  ChowDSP. x42 ships LV2 only, which Melodee does not host. Cardinal
  (1 GB) was left out.

The MPC (AC50 1.3.0.12): glibc 2.39, libstdc++ from GCC 13 (GLIBCXX 3.4.32,
CXXABI 1.3.14), and among others libasound, libcurl, libssl, libz, libbz2,
liblzma, libpng16, libfreetype, libfontconfig, libexpat, libglib,
libgobject, libxml2, libmpg123, libuuid, libudev, libdbus. No libGL, X11,
xcb, cairo, GTK, libsndfile, FFTW or libsamplerate.

## Findings

- Beyond what the MPC has and the stubs already covered (libGL and the X11
  family), the plug-ins asked for two libraries, both in LSP:
  `libcairo.so.2` (its editor) and `libsndfile.so.1` (reading audio files:
  impulse responses, samples).
- No plug-in asks for a newer glibc or C++ runtime than the MPC has
  (aarch64; the newest asked: glibc 2.29, GLIBCXX 3.4.29) or the bridge
  carries (armv7: Debian bookworm, glibc 2.36, GLIBCXX 3.4.30). A 64-bit
  bridge with its own runtime is not needed for these.
- Open MPC builds need only libc, libm, libdl, libpthread, libstdc++ and
  libgcc_s; armv7 ones the same, which the bridge's runtime has.
- Ildaeil and Wolf Shaper need nothing missing.

## What the pack carries since

- Stubs (no-op, GUI only), added: `libcairo.so.2`, and what VSTGUI's Linux
  editors link though no surveyed plug-in used VSTGUI: `libxcb.so.1`,
  `libxcb-util.so.1`, `libxcb-cursor.so.0`, `libxcb-keysyms.so.1`,
  `libxcb-xkb.so.1`, `libxkbcommon-x11.so.0`.
- Real libraries (sound): `libsndfile.so.1` with the codecs it links that
  the MPC lacks: `libFLAC.so.12`, `libogg.so.0`, `libvorbis.so.0`,
  `libvorbisenc.so.2`, `libopus.so.0`, `libmp3lame.so.0` (Debian bookworm
  arm64; the MPC's own `libmpg123` serves the rest). 3 MB in the pack.

Checked on the MPC: LSP's VST3 bundle, which failed before on the two
missing libraries, now opens and runs as an effect (1.1 % of a block), as
does Wolf Shaper (1.6 %).

## Still open

- Done since: a bundle holding many plug-ins (LSP's VST3: 198) lists them
  one by one in the picker, and any of them loads (checked with LSP's
  Flanger Stereo, 2.5 % of a block).
- Not added on speculation: FFTW, libsamplerate, GTK. If a plug-in needs
  one, its page names the missing library ("Could not open").
