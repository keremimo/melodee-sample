#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Build vst2_bridge32 for armhf and gather the armhf runtime it and armv7
# plug-ins need (Debian bookworm: glibc 2.36, libstdc++ from GCC 12) into
# build/vst_probe/bridge32/. Run inside the base's build image from the
# repository root:
#   docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
#       sh tools/vst_probe/build_bridge32.sh
# The runtime is Debian's (LGPL glibc, GPL-with-runtime-exception libstdc++
# and libgcc_s); ship its licences and a source offer with any release.
set -eu
OUT=build/vst_probe/bridge32
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends g++-arm-linux-gnueabihf >/dev/null
mkdir -p "$OUT/armhf"
arm-linux-gnueabihf-g++ -std=c++17 -O2 -Wall -Wextra -o "$OUT/vst2_bridge32" \
    tools/vst_probe/vst2_bridge32.cpp -ldl
LIB=/usr/arm-linux-gnueabihf/lib
for name in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
        libstdc++.so.6 libgcc_s.so.1; do
    cp -L "$LIB/$name" "$OUT/armhf/$name"
done
du -sh "$OUT/armhf" "$OUT/vst2_bridge32"
