CC ?= cc
CXX ?= c++
CFLAGS ?= -O3 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE
# The menu (src/ui.cpp) and Dear ImGui (third_party/imgui, built without our warnings): C++, without exceptions or RTTI
CXXFLAGS ?= -O3 -g
IMGUI_FLAGS := -std=c++17 -fno-exceptions -fno-rtti -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
	       -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -Ithird_party/imgui
LDFLAGS ?= -g
BUILD := build

LIB_SRC := src/adb.c src/audio.c src/awacs.c src/bootinfo.c src/raster.c src/rave.c src/cdaudio.c src/cdimage.c src/cdrom.c src/chooser.c src/clip.c src/clock.c src/cuda.c src/dbdma.c src/density.c src/devtree.c src/display.c src/disk.c src/drives.c src/env.c src/floppy.c src/font.c src/gl.c src/glarray.c src/glattrib.c src/gleval.c src/glfront.c src/gllight.c src/gllist.c src/glpixel.c src/glselect.c src/gltest.c src/gltex.c src/glu.c src/gpu.c src/hfs.c src/hfsread.c src/hostlib.c src/icon.c src/install.c src/isp.c src/jit.c src/jit_a64.c src/jit_x86.c src/lzss.c src/macio.c src/mem.c src/mmio.c src/mp.c src/ndrv.c src/net.c src/nk.c src/nvram.c src/ofnvram.c src/ohci.c src/opengl.c src/parcels.c src/pad.c src/pci.c src/perf.c src/pippin.c src/png.c src/ppc.c src/ppc_fp.c src/priv.c src/rom.c src/rsrc.c src/rtas.c src/scc.c src/share.c src/startup.c src/surface.c src/tools.c src/via.c src/video.c src/welcome.c
IMGUI_SRC := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o) $(BUILD)/ui.o $(IMGUI_SRC:third_party/imgui/%.cpp=$(BUILD)/imgui/%.o)
TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%.o)

# (src/version.h's)
VERSION := $(shell sed -n 's/^\#define BLUE_VERSION "\(.*\)"/\1/p' src/version.h)

all: $(BUILD)/blue

# On macOS, an Info.plist in the executable names the program Blue (in the menu bar and the Dock), the file
# chooser is AppKit's (src/chooser_mac.m), the menu bar Blue's own (src/menubar_mac.m), displays' physical
# sizes are CoreGraphics' (src/density.c), and Blue.app's libraries are found in its Frameworks
# (src/hostlib.c, and @rpath for theirs). The oldest macOS it runs on is MACOSX_DEPLOYMENT_TARGET's.
ifeq ($(shell uname -s),Darwin)
export MACOSX_DEPLOYMENT_TARGET ?= 11.0
INFO_PLIST := $(BUILD)/Info.plist
INFO_PLIST_FLAGS := -Wl,-sectcreate,__TEXT,__info_plist,$(INFO_PLIST) -Wl,-rpath,@executable_path/../Frameworks
OBJCFLAGS ?= -O2 -g
LIB_OBJ += $(BUILD)/chooser_mac.o $(BUILD)/menubar_mac.o
HOST_LIBS := -framework AppKit -framework CoreGraphics
endif

$(BUILD)/blue: $(BUILD)/main.o $(LIB_OBJ) $(INFO_PLIST)
	$(CXX) $(LDFLAGS) $(INFO_PLIST_FLAGS) -o $@ $(BUILD)/main.o $(LIB_OBJ) -lm -ldl -lpthread $(HOST_LIBS)

# art/Info.plist with the version and the oldest macOS filled in
$(BUILD)/Info.plist: art/Info.plist src/version.h | $(BUILD)
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@MACOS@/$(MACOSX_DEPLOYMENT_TARGET)/g' art/Info.plist > $@

# Blue.app (build/Blue.app), with SDL2 and libslirp and what they need from DEPS (Homebrew's prefix, or
# tools/macos-deps.sh's), ad-hoc signed; and Blue-<arch>.dmg (arm64, x86_64), with it and a link to Applications
DEPS ?= $(shell brew --prefix 2>/dev/null)
app: $(BUILD)/blue $(INFO_PLIST)
	tools/package-macos.sh app $(BUILD)/blue $(INFO_PLIST) "$(DEPS)" $(BUILD)/Blue.app

dmg: app
	tools/package-macos.sh dmg $(BUILD)/Blue.app $(BUILD)/Blue-$$(lipo -archs $(BUILD)/blue).dmg

# Blue-<arch>.AppImage (Linux): see tools/package-linux.sh
appimage:
	tools/package-linux.sh

version:
	@echo $(VERSION)

$(BUILD)/blue-tests: $(TEST_OBJ) $(LIB_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm -ldl -lpthread $(HOST_LIBS)

$(BUILD)/%.o: src/%.c $(wildcard src/*.h src/*.inc) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.m $(wildcard src/*.h) | $(BUILD)
	$(CC) $(OBJCFLAGS) -fobjc-arc -Wall -Wextra -c -o $@ $<

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h third_party/imgui/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -Wall -Wextra -c -o $@ $<

$(BUILD)/imgui/%.o: third_party/imgui/%.cpp $(wildcard third_party/imgui/*.h) | $(BUILD)/imgui
	$(CXX) $(filter-out -Werror,$(CXXFLAGS)) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c $(wildcard src/*.h tests/*.h) | $(BUILD)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/tests $(BUILD)/imgui:
	mkdir -p $@

test: $(BUILD)/blue-tests
	./$(BUILD)/blue-tests

clean:
	rm -rf $(BUILD)

.PHONY: all test clean app dmg appimage version

# FP emulation reads the host's IEEE flags and uses dynamic rounding modes
$(BUILD)/ppc_fp.o: CFLAGS += -frounding-math
