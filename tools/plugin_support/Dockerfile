# Melodee's build image: the base's (base/tools/Dockerfile) plus what the
# plug-in support pack is made from (tools/plugin_support/build.py): the
# armhf cross compiler and runtime for the 32-bit bridge, the GUI libraries
# whose export lists the no-op stubs copy, and the real libraries plug-ins
# use for sound that the MPC lacks (libsndfile and its codecs, libsamplerate, FFTW; the survey in
# tools/plugin_support/SURVEY.md says which).
FROM sample-base-builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-arm-linux-gnueabihf \
    libgl1 libx11-6 libxext6 libxrandr2 libxcursor1 libxrender1 libxinerama1 libxi6 libxfixes3 \
    libcairo2 libxcb1 libxcb-util1 libxcb-cursor0 libxcb-keysyms1 libxcb-xkb1 libxkbcommon-x11-0 \
    libsndfile1 libsamplerate0 libfftw3-double3 libfftw3-single3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
