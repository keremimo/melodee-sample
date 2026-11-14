# Melodee on the sample-base runtime: the base layers come from the base/
# submodule, Melodee and its main.cpp from src/. The build is base/base.mk.
BASE_SRC := base/src
TARGET := build/melodee
APP_SOURCES := $(wildcard src/*.cpp src/*/*.cpp src/*/*/*.cpp)
include base/base.mk

# The version shown on the Credits page and used to compare updates
# (shell/update.cpp): ./release.sh passes its VERSION, build.sh passes git
# describe, plain make uses "dev". The stamp file changes only when the
# version changes, so update.cpp is recompiled only then.
MELODEE_VERSION ?= dev
build/gen/melodee_version: FORCE
	@mkdir -p build/gen
	@printf '%s\n' '$(MELODEE_VERSION)' > $@.new
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi
build/obj/melodee/shell/update.o: build/gen/melodee_version
build/obj/melodee/shell/update.o: CPPFLAGS += -DMELODEE_VERSION='"$(MELODEE_VERSION)"'
.PHONY: FORCE
FORCE:

# The web page's files (src/melodee/web/web_bundle.h): its own pages, the
# quickstart and the user guide as HTML, and the images they show, in one
# file the binary carries.
WEB_INPUTS := tools/web_bundle.py $(wildcard src/melodee/web/static/*) docs/QUICKSTART.md docs/GUIDE.md \
	$(wildcard docs/guide/*.md) $(wildcard docs/images/*/*.png docs/images/*.png)
build/gen/web_bundle.bin: $(WEB_INPUTS)
	@mkdir -p build/gen
	python3 tools/web_bundle.py $@
build/obj/melodee/web/web_bundle.o: build/gen/web_bundle.bin

# JUCE's decoders, for an SFZ's FLAC, Ogg and AIFF samples and an MPC
# expansion's MP3 previews. Melodee's alone, so the base stays as it is.
FORMATS := $(JUCE)/juce_audio_formats
FORMAT_FLAGS := -DJUCE_USE_MP3AUDIOFORMAT=1 -DJUCE_USE_WINDOWS_MEDIA_FORMAT=0 -DJUCE_USE_OPUS=0
FORMAT_OBJECTS := build/obj/juce_audio_formats.o build/obj/juce_audio_formats_flac_1.o build/obj/juce_audio_formats_flac_2.o build/obj/juce_audio_formats_ogg.o
OBJECTS += $(FORMAT_OBJECTS)
$(TARGET): $(FORMAT_OBJECTS)
build/obj/juce_audio_formats.o: $(FORMATS)/juce_audio_formats.cpp
	@mkdir -p build/obj
	$(CXX) $(CPPFLAGS) $(FORMAT_FLAGS) $(CXXFLAGS) -w -c $< -o $@
build/obj/juce_audio_formats_%.o: $(FORMATS)/juce_audio_formats_%.c
	@mkdir -p build/obj
	$(CC) $(CPPFLAGS) $(FORMAT_FLAGS) $(CFLAGS) -w -c $< -o $@
build/obj/melodee/shell/sample_decode.o: CPPFLAGS += $(FORMAT_FLAGS)

# Plug-ins (docs/MELODEE.md, Plug-ins): JUCE's headless plug-in hosting with
# VST3 and LV2 (lilv and its libraries, bundled in JUCE; VST2, CLAP and the
# armv7 bridge are Melodee's own, src/melodee/plugin/; CLAP's headers from
# build/deps/clap, src/melodee/dependencies.py), and
# the support pack (GUI stubs, the bridge and its armhf runtime) linked into
# the binary. The pack needs Melodee's build image (tools/plugin_support/Dockerfile).
HOSTING := $(JUCE)/juce_audio_processors_headless
CPPFLAGS += -DJUCE_PLUGINHOST_VST3=1 -DJUCE_PLUGINHOST_LV2=1 -I$(HOSTING)/format_types/VST3_SDK -Ibuild/deps/clap/include
LV2_SDK := $(HOSTING)/format_types/LV2_SDK
LV2_INCLUDES := $(addprefix -I$(LV2_SDK),/ /lv2 /serd /sord /sord/src /sratom /lilv /lilv/src)
PLUGIN_OBJECTS := build/obj/juce_audio_processors_headless.o build/obj/juce_audio_processors_headless_lv2_libs.o build/obj/plugin_support_pack.o
OBJECTS += $(PLUGIN_OBJECTS)
$(TARGET): $(PLUGIN_OBJECTS)
# Makefile too: a hosting flag changed here builds them again.
build/obj/juce_audio_processors_headless.o build/obj/juce_audio_processors_headless_lv2_libs.o: build/obj/%.o: $(HOSTING)/%.cpp Makefile
	@mkdir -p build/obj
	$(CXX) $(CPPFLAGS) $(LV2_INCLUDES) $(CXXFLAGS) -w -c $< -o $@
build/plugin-support.pack: tools/plugin_support/build.py tools/plugin_support/make_stubs.py tools/plugin_support/Dockerfile \
        src/melodee/plugin/bridge32/vst2_bridge32.cpp src/melodee/plugin/vst2_abi.h src/melodee/plugin/vst2_bridge_protocol.h
	python3 tools/plugin_support/build.py build/plugin-support $@
build/obj/plugin_support_pack.o: src/melodee/plugin/bridge32/support_pack.S build/plugin-support.pack
	@mkdir -p build/obj
	$(CC) -c $< -o $@
