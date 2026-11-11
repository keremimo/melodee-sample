# Melodee on the sample-base runtime: the base layers come from the base/
# submodule, Melodee and its main.cpp from src/. The build is base/base.mk.
BASE_SRC := base/src
TARGET := build/melodee
APP_SOURCES := $(wildcard src/*.cpp src/*/*.cpp src/*/*/*.cpp)
include base/base.mk

# JUCE's decoders, for an SFZ's FLAC, Ogg and AIFF samples. Melodee's alone,
# so the base stays as it is.
FORMATS := $(JUCE)/juce_audio_formats
FORMAT_FLAGS := -DJUCE_USE_MP3AUDIOFORMAT=0 -DJUCE_USE_WINDOWS_MEDIA_FORMAT=0 -DJUCE_USE_OPUS=0
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
