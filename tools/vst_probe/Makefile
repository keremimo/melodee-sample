# vst_probe, JUCE's headless VST3 host as a command-line probe for the MPC.
# Run from the repository root inside the build image, after Melodee's build
# has compiled JUCE: it reuses those juce_core/juce_events/juce_audio_basics
# objects and compiles only juce_audio_processors_headless itself.
#   make -f tools/vst_probe/Makefile
JUCE := build/deps/JUCE/modules
OUT := build/vst_probe
CXX ?= g++
CPPFLAGS := -I$(JUCE) -I$(JUCE)/juce_audio_processors_headless/format_types/VST3_SDK \
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG \
    -DJUCE_PLUGINHOST_VST3=1
CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
JUCE_OBJECTS := build/obj/juce_core.o build/obj/juce_core_CompilationTime.o build/obj/juce_core_zlib.o \
    build/obj/juce_events.o build/obj/juce_audio_basics.o
OBJECTS := $(OUT)/obj/juce_audio_processors_headless.o $(OUT)/obj/vst_probe.o $(OUT)/obj/vst2_instance.o $(OUT)/obj/vst2_bridge_instance.o

$(OUT)/vst_probe: $(OBJECTS) $(JUCE_OBJECTS)
	$(CXX) $^ -ldl -pthread -lrt -o $@
$(OUT)/obj/juce_audio_processors_headless.o: $(JUCE)/juce_audio_processors_headless/juce_audio_processors_headless.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -w -c $< -o $@
$(OUT)/obj/%.o: tools/vst_probe/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -c $< -o $@
-include $(OBJECTS:.o=.d)
