# SPDX-License-Identifier: AGPL-3.0-only
# lo-fly for Melodee (README.md). Builds into build/lo-fly/ from lo-fly's dsp/ at a commit (LOFLY_REV, default
# HEAD), or from a copy of its working tree (LOFLY_REV=WORKTREE).
#   make            the MPC Sample's lo-fly folder (aarch64 Linux, glibc 2.28, through zig) and the Mac bundle
#   make mpc        build/lo-fly/mpc/lo-fly/: lo-fly.clap and an empty Presets folder
#   make mac        build/lo-fly/mac/lo-fly.clap (a macOS bundle, for clap-validator)
#   make screens    build/lo-fly/screens/*.png: the panel's pages, drawn by the plug-in itself
#   make bench      render and draw times on this machine
#   make presets    the presets preset discovery finds, each loaded and played
#   make validate   clap-validator on the Mac bundle
ROOT := ../..
LOFLY_DIR ?= $(ROOT)/../lo-fly
LOFLY_REV ?= HEAD
OUT ?= $(ROOT)/build/lo-fly
CLAP_INCLUDE ?= $(ROOT)/build/deps/clap/include
# National Park (SIL OFL 1.1), from Google Fonts' repository at a fixed commit: `make fonts`
FONT_DIR ?= $(OUT)/fonts
GFONTS_REV := 83c62e30d87daa70216dab6b984d00d20d96d585
NP_URL := https://raw.githubusercontent.com/google/fonts/$(GFONTS_REV)/ofl/nationalpark
PYTHON ?= python3
ZIG ?= zig
CXX_MAC ?= clang++
VALIDATOR ?= $(LOFLY_DIR)/.tools/clap-validator/bin/clap-validator

ifeq ($(LOFLY_REV),WORKTREE)
# The working tree's dsp/ as it is now, copied again on every make (times kept, so only changed files rebuild).
SRC := $(OUT)/lofly-worktree
DSP := $(basename $(notdir $(wildcard $(LOFLY_DIR)/dsp/src/*.cpp)))
EXPORT_WHEN := FORCE
else
REV := $(shell git -C $(LOFLY_DIR) rev-parse --short=12 $(LOFLY_REV))
SRC := $(OUT)/lofly-$(REV)
DSP := $(basename $(notdir $(filter %.cpp,$(shell git -C $(LOFLY_DIR) ls-tree --name-only $(REV) dsp/src/))))
EXPORT_WHEN :=
endif
OWN := plugin panel

INCLUDES := -I$(SRC)/dsp/include -I$(ROOT)/sdk/melodee-screen -I$(CLAP_INCLUDE) -I$(OUT)/gen
FLAGS := -std=c++20 -O2 -MMD -MP -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DNDEBUG $(INCLUDES)
WARN := -Wall -Wextra -Wno-unused-parameter
MPC_FLAGS := -target aarch64-linux-gnu.2.28 -mcpu=cortex_a55 -Wno-nullability-completeness

MAC_OBJ := $(addprefix $(OUT)/mac/obj/,$(addsuffix .o,$(DSP) $(OWN)))
MPC_OBJ := $(addprefix $(OUT)/mpc/obj/,$(addsuffix .o,$(DSP) $(OWN)))
BUNDLE := $(OUT)/mac/lo-fly.clap

all: mpc mac
mpc: $(OUT)/mpc/lo-fly/lo-fly.clap
mac: $(BUNDLE)/Contents/MacOS/lo-fly $(BUNDLE)/Contents/Info.plist

$(SRC)/.exported: $(EXPORT_WHEN)
	@test -d $(LOFLY_DIR)/dsp || (echo "lo-fly not found at $(LOFLY_DIR)"; exit 1)
	mkdir -p $(SRC)
ifeq ($(LOFLY_REV),WORKTREE)
	rsync -a --delete $(LOFLY_DIR)/dsp/ $(SRC)/dsp/
else
	git -C $(LOFLY_DIR) archive $(REV) dsp | tar -x -C $(SRC)
endif
	touch $@
$(SRC)/dsp/src/%.cpp: | $(SRC)/.exported ;
.PRECIOUS: $(SRC)/dsp/src/%.cpp

$(OUT)/gen/fonts.h: fonts.py $(FONT_DIR)/NationalPark.ttf
	$(PYTHON) fonts.py $(FONT_DIR)/NationalPark.ttf $@

fonts: $(FONT_DIR)/NationalPark.ttf
$(FONT_DIR)/NationalPark.ttf:
	@mkdir -p $(FONT_DIR)
	curl -sfL -o $(FONT_DIR)/NationalPark-OFL.txt $(NP_URL)/OFL.txt
	curl -sfL -o $@ '$(NP_URL)/NationalPark%5Bwght%5D.ttf'

HEADERS := store.h panel.h $(ROOT)/sdk/melodee-screen/melodee_screen.h

$(OUT)/mac/obj/%.o: $(SRC)/dsp/src/%.cpp | $(SRC)/.exported
	@mkdir -p $(@D)
	$(CXX_MAC) $(FLAGS) -c $< -o $@
$(OUT)/mac/obj/%.o: %.cpp $(HEADERS) $(OUT)/gen/fonts.h | $(SRC)/.exported
	@mkdir -p $(@D)
	$(CXX_MAC) $(FLAGS) $(WARN) -c $< -o $@
$(OUT)/mpc/obj/%.o: $(SRC)/dsp/src/%.cpp | $(SRC)/.exported
	@mkdir -p $(@D)
	$(ZIG) c++ $(MPC_FLAGS) $(FLAGS) -c $< -o $@
$(OUT)/mpc/obj/%.o: %.cpp $(HEADERS) $(OUT)/gen/fonts.h | $(SRC)/.exported
	@mkdir -p $(@D)
	$(ZIG) c++ $(MPC_FLAGS) $(FLAGS) $(WARN) -c $< -o $@

# -z nodelete: the library stays mapped after the host's dlclose. Its statically linked libc++ leaves exit handlers
# behind that would otherwise call into unmapped code when the host exits (a crash, seen with any zig-built CLAP).
$(OUT)/mpc/lo-fly/lo-fly.clap: $(MPC_OBJ) lo-fly.map $(OUT)/mpc/lo-fly/FONTS.txt
	@mkdir -p $(@D)/Presets
	$(ZIG) c++ $(MPC_FLAGS) -shared -Wl,--version-script=lo-fly.map -Wl,-z,nodelete -Wl,-s $(MPC_OBJ) -o $@
# The panel's type is drawn from National Park: its licence goes with the plug-in.
$(OUT)/mpc/lo-fly/FONTS.txt: $(FONT_DIR)/NationalPark.ttf
	@mkdir -p $(@D)
	{ echo "lo-fly's panel draws its text from National Park, under the SIL Open Font License 1.1."; \
	  echo; cat $(FONT_DIR)/NationalPark-OFL.txt; } > $@

$(BUNDLE)/Contents/MacOS/lo-fly: $(MAC_OBJ)
	@mkdir -p $(@D)
	$(CXX_MAC) -bundle $(MAC_OBJ) -o $@
$(BUNDLE)/Contents/Info.plist: Info.plist
	@mkdir -p $(@D)
	cp Info.plist $@

$(OUT)/mac/preview: preview.cpp $(MAC_OBJ)
	$(CXX_MAC) $(FLAGS) $(WARN) preview.cpp $(MAC_OBJ) -o $@

screens: $(OUT)/mac/preview
	@mkdir -p $(OUT)/screens
	$(OUT)/mac/preview $(OUT)/screens
bench: $(OUT)/mac/preview
	$(OUT)/mac/preview --bench
presets: $(OUT)/mac/preview
	$(OUT)/mac/preview --presets
validate: mac
	$(VALIDATOR) validate $(BUNDLE)

clean:
	rm -rf $(OUT)

-include $(MAC_OBJ:.o=.d) $(MPC_OBJ:.o=.d)
# The compiler writes the .d files; make never builds one.
%.d: ;

FORCE:
.PHONY: all mpc mac fonts screens bench presets validate clean FORCE
