# Melodee on the sample-base runtime: the base layers come from the base/
# submodule, Melodee and its main.cpp from src/. The build is base/base.mk.
BASE_SRC := base/src
TARGET := build/melodee
APP_SOURCES := $(wildcard src/*.cpp src/*/*.cpp src/*/*/*.cpp)
include base/base.mk
