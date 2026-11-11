# vst_probe, a command-line VST3 / VST2 host for the MPC on Melodee's plug-in
# code (src/melodee/plugin/). Run from the repository root inside Melodee's
# build image after Melodee's own build: it reuses its JUCE objects, hosting
# included, and its plug-in support (build/plugin-support: stubs, bridge32).
#   make -f tools/vst_probe/Makefile
JUCE := build/deps/JUCE/modules
OUT := build/vst_probe
CXX ?= g++
CPPFLAGS := -Isrc -I$(JUCE) -I$(JUCE)/juce_audio_processors_headless/format_types/VST3_SDK \
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG \
    -DJUCE_PLUGINHOST_VST3=1
CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
JUCE_OBJECTS := build/obj/juce_core.o build/obj/juce_core_CompilationTime.o build/obj/juce_core_zlib.o \
    build/obj/juce_events.o build/obj/juce_audio_basics.o build/obj/juce_audio_processors_headless.o \
    build/obj/melodee/plugin/vst2_instance.o build/obj/melodee/plugin/vst2_bridge_instance.o
OBJECTS := $(OUT)/obj/vst_probe.o

$(OUT)/vst_probe: $(OBJECTS) $(JUCE_OBJECTS)
	$(CXX) $^ -ldl -pthread -lrt -o $@
$(OUT)/obj/%.o: tools/vst_probe/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -c $< -o $@
-include $(OBJECTS:.o=.d)
# Its support beside it, as the probe looks for it.
support: $(OUT)/vst_probe
	rm -rf $(OUT)/stubs $(OUT)/bridge32 && cp -r build/plugin-support/stubs build/plugin-support/bridge32 $(OUT)/
.PHONY: support
