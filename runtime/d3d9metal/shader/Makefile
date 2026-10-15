# runtime/d3d9metal/shader -- the run-time D3D9 bytecode -> MSL translator. Host-only, no window.
#   make            libd3d9msl.a, d3d9msl_tool, test_d3d9msl under build/d3d9metal/shader/
#   make test       crosscheck against tools/shaders/msl.py (fixtures), C-library fixtures through the Metal
#                   compiler, GPU render checks of the emitted conventions, the cache, and test_semantics.py's
#                   GPU semantics on the C library's output
#   make corpus     every unique game shader (extracted by tools/shaders, git-ignored) translated by the C library,
#                   cross-checked against msl.py and compiled, in 4 chunks with per-chunk logs (tests/corpus.sh)
ROOT    := $(abspath ../../..)
OUT     := $(ROOT)/build/d3d9metal/shader
CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I. -DSG_REPO_ROOT='"$(ROOT)"'
SRC     := d3d9msl.c d3d9msl_cache.c
OBJ     := $(SRC:%.c=$(OUT)/%.o)
TO      := perl -e 'alarm 300; exec @ARGV'

all: $(OUT)/libd3d9msl.a $(OUT)/d3d9msl_tool $(OUT)/test_d3d9msl $(OUT)/mslcheck $(OUT)/mslrun

$(OUT)/%.o: %.c d3d9msl.h
	@mkdir -p $(OUT)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/libd3d9msl.a: $(OBJ)
	ar rcs $@ $^
$(OUT)/d3d9msl_tool: tests/d3d9msl_tool.c $(OUT)/libd3d9msl.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $< $(OUT)/libd3d9msl.a -o $@
$(OUT)/test_d3d9msl: tests/test_d3d9msl.m $(OUT)/libd3d9msl.a
	$(CC) -fobjc-arc $(CPPFLAGS) $(CFLAGS) $< $(OUT)/libd3d9msl.a -framework Metal -framework Foundation -o $@
$(OUT)/mslcheck: $(ROOT)/tools/shaders/mslcheck/mslcheck.m
	$(CC) -fobjc-arc -O2 $< -framework Metal -framework Foundation -o $@
$(OUT)/mslrun: $(ROOT)/tools/shaders/mslcheck/mslrun.m
	$(CC) -fobjc-arc -O2 $< -framework Metal -framework Foundation -o $@

test: all
	cd $(ROOT) && $(TO) python3 -P runtime/d3d9metal/shader/tests/crosscheck.py $(OUT)/d3d9msl_tool $(OUT)/xfix
	$(TO) $(OUT)/test_d3d9msl $(OUT) $(OUT)/xfix/blobs
	cd $(ROOT) && $(TO) python3 -P tools/shaders/test_shaders.py --metal $(OUT)/mslcheck
	cd $(ROOT) && $(TO) python3 -P tools/shaders/test_semantics.py $(OUT)/mslrun
	cd $(ROOT) && $(TO) python3 -P tools/shaders/test_semantics.py $(OUT)/mslrun --impl $(OUT)/d3d9msl_tool

corpus: all
	cd $(ROOT) && sh runtime/d3d9metal/shader/tests/corpus.sh $(OUT)

clean:
	rm -rf $(OUT)/*.o $(OUT)/*.a $(OUT)/d3d9msl_tool $(OUT)/test_d3d9msl $(OUT)/xfix $(OUT)/cache-test

.PHONY: all test corpus clean
