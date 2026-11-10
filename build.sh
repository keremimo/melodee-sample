#!/bin/sh
# Build Melodee for the MPC Sample (linux/arm64) in the base's build image,
# then run the tests, the sound self-test and the page previews.
set -eu
cd "$(dirname "$0")"
[ -f base/base.mk ] || git submodule update --init
python3 tools/dependencies.py
docker build --platform linux/arm64 -t sample-base-builder base/tools
docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
    sh -c 'make -j4 && python3 -m unittest discover -s tests -v && build/melodee --self-test build/test.wav && mkdir -p build/melodee-preview && build/melodee --melodee-preview build/melodee-preview'
