#!/bin/sh
# Build Melodee for the MPC Sample (linux/arm64) in its build image (the base's
# plus tools/plugin_support/Dockerfile),
# then run the tests, the sound self-test and the page previews.
set -eu
cd "$(dirname "$0")"
[ -f base/base.mk ] || git submodule update --init
python3 tools/dependencies.py
docker build --platform linux/arm64 -t sample-base-builder base/tools
# Melodee's own image on top: the plug-in support pack's cross compiler and GUI libraries.
docker build --platform linux/arm64 -t melodee-builder tools/plugin_support
docker run --rm --platform linux/arm64 -v "$PWD:/work" melodee-builder \
    sh -c 'make -j4 && python3 -m unittest discover -s tests -v && build/melodee --self-test build/test.wav && mkdir -p build/melodee-preview && build/melodee --melodee-preview build/melodee-preview'
