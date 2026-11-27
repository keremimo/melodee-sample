# Melodee on the sample-base runtime: the base layers come from the base/
# submodule, the engine, model and files from the core/ submodule
# (ellic-core, shared with melodee-move), Melodee's screens and main.cpp
# from src/. The build is base/base.mk.
BASE_SRC := base/src
TARGET := build/melodee
APP_SOURCES := $(wildcard src/*.cpp src/*/*.cpp src/*/*/*.cpp)
include base/base.mk

# The core's objects sit beside Melodee's own in build/obj/melodee/, as
# when they lived in src/ (layout test: no file in both).
CORE := core
include core/core.mk
CORE_OBJECTS := $(patsubst core/src/%.cpp,build/obj/%.o,$(CORE_SOURCES))
OBJECTS += $(CORE_OBJECTS)
$(TARGET): $(CORE_OBJECTS)
CPPFLAGS += $(CORE_CPPFLAGS)
build/obj/%.o: core/src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -c $< -o $@
# An object built before the core moved to core/ names its source in src/:
# forgotten, so that it is built again from the core.
STALE := $(shell grep -lsE '(^|[ :])src/[^ ]*\.cpp' $(CORE_OBJECTS:.o=.d))
$(foreach d,$(STALE),$(shell rm -f $(d) $(d:.d=.o)))
-include $(CORE_OBJECTS:.o=.d)

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

# The MPC 3 export's template (the core's tools/mpc3_template.py), carried in the binary.
build/obj/melodee/shell/mpc3_export.o: $(CORE_TEMPLATE)
build/obj/melodee/shell/mpc3_export.o: CXXFLAGS += $(CORE_ASFLAGS)

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
# build/deps/clap, src/melodee/dependencies.py; the plug-in screen header,
# sdk/melodee-screen, MIT), and
# the support pack (GUI stubs, the bridge and its armhf runtime) linked into
# the binary. The pack needs Melodee's build image (tools/plugin_support/Dockerfile).
HOSTING := $(JUCE)/juce_audio_processors_headless
CPPFLAGS += -DJUCE_PLUGINHOST_VST3=1 -DJUCE_PLUGINHOST_LV2=1 -I$(HOSTING)/format_types/VST3_SDK -Ibuild/deps/clap/include -Isdk/melodee-screen
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
        src/melodee/plugin/bridge32/vst2_bridge32.cpp src/melodee/plugin/vst2_abi.h src/melodee/plugin/vst2_bridge_protocol.h \
        sdk/melodee-screen/melodee_screen.h
	python3 tools/plugin_support/build.py build/plugin-support $@
build/obj/plugin_support_pack.o: src/melodee/plugin/bridge32/support_pack.S build/plugin-support.pack
	@mkdir -p build/obj
	$(CC) -c $< -o $@

# Stems (src/melodee/shell/stems.h): the separating program and its model, files of their own in the release, not
# linked into Melodee (tools/stems/build.py: stems_engine.c on SpleeterRT's network code and OpenBLAS, and Deezer's
# 4-stem model, from build/deps). `make stems` builds them; the first time takes a few minutes.
STEMS := build/stems/melodee-stems
.PHONY: stems
stems: $(STEMS)
$(STEMS): tools/stems/build.py tools/stems/stems_engine.c tools/stems/convert_model.py
	python3 tools/stems/build.py build/stems

# Freeze with a plug-in that plays by itself (tests/clap_fixture.cpp built with MELODEE_TEST_SEQUENCER), through
# PRINT, the rack and the engine (tests/test_plugin_hosts.py runs it).
build/test-transport-freeze: tests/transport_freeze.cpp $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror $< $(filter-out build/obj/main.o,$(OBJECTS)) $(LDLIBS) -o $@
