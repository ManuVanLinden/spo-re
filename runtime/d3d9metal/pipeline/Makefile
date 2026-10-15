# runtime/d3d9metal/pipeline -- the pipeline package's own tests (the package itself is compiled into the
# Metal back end by runtime/d3d9metal's Makefile and tools/link/build.py).
#   make test     test_pl_maps (host-only mapping tables) and test_pl_gpu (draws on the GPU against a stub device,
#                 no device-layer code; a 64x64 offscreen target, no window), test_pl_archive (the
#                 pipeline archive across launches, each launch a child process; no window)
ROOT := $(abspath ../../..)
OUT  := $(ROOT)/build/d3d9metal/pipeline
CF   := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
INC  := -I$(ROOT)/runtime/include -I$(ROOT)/runtime/com/include -I$(ROOT)/runtime/d3d9null/include \
        -I$(ROOT)/runtime/d3d9null/src -I$(ROOT)/runtime/d3d9metal/include -I$(OUT) -DSG_REPO_ROOT='"$(ROOT)"'
SRC  := pl_maps.c pl_draw.m pl_shader.m $(wildcard ../shader/d3d9msl*.c)
HDR  := pl_int.h pl_shader.h $(ROOT)/runtime/d3d9metal/include/d3d9metal_int.h ../shader/d3d9msl.h

all: $(OUT)/test_pl_maps $(OUT)/test_pl_gpu $(OUT)/test_pl_archive

$(OUT)/test_pl_maps: pl_maps.c tests/test_pl_maps.c pl_int.h
	@mkdir -p $(OUT)
	clang $(CF) pl_maps.c tests/test_pl_maps.c -o $@
$(OUT)/pl_test_shaders.h: $(wildcard tests/shaders/*.asm)
	@mkdir -p $(OUT)
	python3 -P $(ROOT)/runtime/d3d9metal/tests/gen_shaders.py tests/shaders $@
$(OUT)/test_pl_gpu: $(SRC) tests/test_pl_gpu.m $(HDR) $(OUT)/pl_test_shaders.h
	clang -fno-objc-arc $(CF) $(INC) $(SRC) tests/test_pl_gpu.m -framework Metal -framework Foundation -o $@
$(OUT)/test_pl_archive: $(SRC) tests/test_pl_archive.m $(HDR) $(OUT)/pl_test_shaders.h
	clang -fno-objc-arc $(CF) $(INC) pl_shader.m $(wildcard ../shader/d3d9msl*.c) tests/test_pl_archive.m \
	    -framework Metal -framework Foundation -o $@

test: all
	perl -e 'alarm 30; exec @ARGV' $(OUT)/test_pl_maps
	SPORE_METAL_PSO_ARCHIVE=$(OUT)/test-pso-archive.metalar perl -e 'alarm 60; exec @ARGV' $(OUT)/test_pl_gpu $(OUT)/test_pl_gpu.ppm
	perl -e 'alarm 300; exec @ARGV' $(OUT)/test_pl_archive $(OUT)/archive-test

clean:
	rm -rf $(OUT)/test_pl_maps $(OUT)/test_pl_gpu $(OUT)/test_pl_archive $(OUT)/archive-test $(OUT)/pl_test_shaders.h

.PHONY: all test clean
