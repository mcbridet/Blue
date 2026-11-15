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

LIB_SRC := src/adb.c src/audio.c src/awacs.c src/bootinfo.c src/raster.c src/rave.c src/cdaudio.c src/cdimage.c src/cdrom.c src/clip.c src/clock.c src/cuda.c src/dbdma.c src/devtree.c src/display.c src/disk.c src/drives.c src/floppy.c src/gl.c src/glarray.c src/glattrib.c src/glfront.c src/gllight.c src/gllist.c src/glpixel.c src/gltest.c src/gltex.c src/glu.c src/gpu.c src/hfs.c src/hfsread.c src/install.c src/jit.c src/jit_a64.c src/jit_x86.c src/lzss.c src/macio.c src/mem.c src/mmio.c src/mp.c src/ndrv.c src/net.c src/nk.c src/nvram.c src/ohci.c src/opengl.c src/parcels.c src/pci.c src/perf.c src/png.c src/ppc.c src/ppc_fp.c src/priv.c src/rom.c src/rsrc.c src/rtas.c src/scc.c src/share.c src/surface.c src/tools.c src/via.c src/video.c
IMGUI_SRC := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o) $(BUILD)/ui.o $(IMGUI_SRC:third_party/imgui/%.cpp=$(BUILD)/imgui/%.o)
TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%.o)

all: $(BUILD)/syshm

$(BUILD)/syshm: $(BUILD)/main.o $(LIB_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm -ldl -lpthread

$(BUILD)/syshm-tests: $(TEST_OBJ) $(LIB_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm -ldl -lpthread

$(BUILD)/%.o: src/%.c $(wildcard src/*.h src/*.inc) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h third_party/imgui/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -Wall -Wextra -c -o $@ $<

$(BUILD)/imgui/%.o: third_party/imgui/%.cpp $(wildcard third_party/imgui/*.h) | $(BUILD)/imgui
	$(CXX) $(filter-out -Werror,$(CXXFLAGS)) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c $(wildcard src/*.h tests/*.h) | $(BUILD)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/tests $(BUILD)/imgui:
	mkdir -p $@

test: $(BUILD)/syshm-tests
	./$(BUILD)/syshm-tests

clean:
	rm -rf $(BUILD)

.PHONY: all test clean

# FP emulation reads the host's IEEE flags and uses dynamic rounding modes
$(BUILD)/ppc_fp.o: CFLAGS += -frounding-math
