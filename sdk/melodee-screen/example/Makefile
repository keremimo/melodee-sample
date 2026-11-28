# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# Screen Example (screen_example.cpp): one library with both entry points.
#   make            screen-example.clap and screen-example.so for this machine (aarch64 Linux for the MPC Sample)
#   make armv7      screen-example-armv7.so, the VST2 for 32-bit ARM (Melodee runs it in its bridge)
# CLAP's headers (github.com/free-audio/clap, MIT) from CLAP_INCLUDE.
CLAP_INCLUDE ?= ../../../build/deps/clap/include
ARMV7_CXX ?= arm-linux-gnueabihf-g++
FLAGS := -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -I.. -I$(CLAP_INCLUDE)

all: screen-example.clap screen-example.so

screen-example.so: screen_example.cpp ../melodee_screen.h
	$(CXX) $(FLAGS) screen_example.cpp -o $@

screen-example.clap: screen-example.so
	cp screen-example.so $@

armv7: screen-example-armv7.so

screen-example-armv7.so: screen_example.cpp ../melodee_screen.h
	$(ARMV7_CXX) $(FLAGS) screen_example.cpp -o $@

clean:
	rm -f screen-example.clap screen-example.so screen-example-armv7.so

.PHONY: all armv7 clean
