#!/bin/sh
# Build, strip and package a Melodee release: ./release.sh v0.1.0-alpha.1
# writes dist/Melodee-MPC-Sample-<version>.zip and its .sha256.
set -eu
cd "$(dirname "$0")"
[ $# -ge 1 ] || { echo "usage: ./release.sh VERSION [--allow-dirty]" >&2; exit 2; }
MELODEE_VERSION=$1 ./build.sh
docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
    sh -c 'strip -o build/melodee.release build/melodee && build/melodee.release --self-test build/release-test.wav'
python3 tools/make_release.py "$@"
