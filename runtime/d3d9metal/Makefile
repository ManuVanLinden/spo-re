# runtime/d3d9metal -- the Metal back end of the guest Direct3D 9. GUI tests (a window appears):
#   make test          runtime/d3d9null's suite with SPORE_D3D9=metal, the readback test, the present/window test,
#                      and the conformance test (tests/conformance.cpp: a Direct3D 9 test app, on our own declarations)
# src/ is the device layer, shader/ the shader translator, pipeline/ the state and draws. By default the
# back end is built with pipeline/*.mm, pipeline/*.m, shader/*.c and shader/*.mm (the real draws, as the game
# links it); WITH_PIPELINE=0 builds the back end alone (draws are stubs; the conformance test is then skipped).
# Needs only the Command Line Tools (clang, -framework Metal); no Xcode.
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/d3d9metal
CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
OBJCFLAGS := -fno-objc-arc
CXXFLAGS := -std=c++17 -fno-exceptions -fno-rtti
CPPFLAGS += -I$(ROOT)/runtime/include -I$(ROOT)/runtime/com/include -I$(ROOT)/runtime/com/tests \
            -I$(ROOT)/runtime/d3d9null/include -I$(ROOT)/runtime/d3d9null/src -Iinclude -DSG_REPO_ROOT='"$(ROOT)"'
LDFLAGS += -framework Metal -framework QuartzCore -framework AppKit -framework Foundation
HDR     := include/d3d9metal_int.h include/d3d9metal.h src/mt_priv.h $(ROOT)/runtime/d3d9null/include/d3d9null.h \
           $(ROOT)/runtime/com/include/sg_com.h $(ROOT)/runtime/d3d9null/src/d3d9_int.h

WITH_PIPELINE ?= 1
SRC_MM  := src/mt_d3d9.mm src/mt_resources.mm src/mt_present.mm
SRC_M   := src/mt_mac.m
ifeq ($(WITH_PIPELINE),1)
SRC_MM  += $(wildcard pipeline/*.mm) $(wildcard shader/*.mm)
SRC_M   += $(wildcard pipeline/*.m)
SRC_C   := $(wildcard pipeline/*.c) $(wildcard shader/*.c)
CPPFLAGS += -Ipipeline -Ishader
endif
OBJ     := $(patsubst %.mm,$(OUT)/obj/%.o,$(SRC_MM)) $(patsubst %.m,$(OUT)/obj/%.o,$(SRC_M)) $(patsubst %.c,$(OUT)/obj/%.o,$(SRC_C))

# the null device and COM layer; its Direct3DCreate9 picks the Metal back end for SPORE_D3D9=metal or unset
NULLSRC := $(ROOT)/runtime/d3d9null/src/d3d9_adapter.c $(ROOT)/runtime/d3d9null/src/d3d9_device.c \
           $(ROOT)/runtime/com/src/com.c $(ROOT)/runtime/com/tests/com_harness.c
# our own test shaders (tests/shaders/*.asm), assembled with tools/shaders/d3dbc.py
GEN     := $(OUT)/gen
SHADERS := $(GEN)/test_shaders.h

ALL     := $(OUT)/test_d3d9_metal $(OUT)/test_readback $(OUT)/test_present
ifeq ($(WITH_PIPELINE),1)
ALL     += $(OUT)/test_conformance
endif
all: $(ALL)

$(OUT)/obj/%.o: %.mm $(HDR)
	@mkdir -p $(dir $@)
	clang++ $(CXXFLAGS) $(OBJCFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/obj/%.o: %.m $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/obj/%.o: %.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(SHADERS): tests/gen_shaders.py $(wildcard tests/shaders/*.asm)
	@mkdir -p $(GEN)
	python3 -P tests/gen_shaders.py tests/shaders $@ >/dev/null
$(OUT)/obj/test_window.o: tests/test_window.m include/d3d9metal.h
	@mkdir -p $(dir $@)
	$(CC) -fobjc-arc $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/obj/null_libs.a: $(NULLSRC) $(HDR)
	@mkdir -p $(OUT)/obj/null
	cd $(OUT)/obj/null && $(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) -c $(NULLSRC)
	rm -f $@ && ar rcs $@ $(OUT)/obj/null/*.o

# runtime/d3d9null's own suite, unchanged, linked with this back end (SPORE_D3D9 picks it at run time)
$(OUT)/test_d3d9_metal: $(ROOT)/runtime/d3d9null/tests/test_d3d9.c tests/test_null_suite.c $(OBJ) $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) -Dmain=null_suite_main -c $(ROOT)/runtime/d3d9null/tests/test_d3d9.c -o $(OUT)/obj/test_d3d9_null.o
	clang++ $(CPPFLAGS) $(CFLAGS) -x c tests/test_null_suite.c -x none $(OUT)/obj/test_d3d9_null.o $(OBJ) $(OUT)/obj/test_window.o \
	    $(OUT)/obj/null_libs.a $(LDFLAGS) -o $@
$(OUT)/test_readback: tests/test_readback.c $(SHADERS) $(OBJ) $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a
	clang++ $(CPPFLAGS) -I$(GEN) $(CFLAGS) -x c -std=c11 tests/test_readback.c -x none $(OBJ) \
	    $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a $(LDFLAGS) -o $@
# The conformance test (C++ against our own tests/d3d9_host.h, through the guest vtables by tests/conformance_shim.cpp)
CONF_CXX := clang++ -std=c++17 $(CPPFLAGS) -I$(GEN) $(CFLAGS) -Wno-unused-variable
$(OUT)/test_conformance: tests/conformance.cpp tests/conformance_shim.cpp tests/d3d9_host.h tests/testutil.h $(SHADERS) $(OBJ) \
                         $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a
	$(CONF_CXX) -c tests/conformance.cpp -o $(OUT)/obj/conformance.o
	$(CONF_CXX) -c tests/conformance_shim.cpp -o $(OUT)/obj/conformance_shim.o
	clang++ $(OUT)/obj/conformance.o $(OUT)/obj/conformance_shim.o $(OBJ) $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a \
	    $(LDFLAGS) -o $@
$(OUT)/test_present: tests/test_present.mm $(OBJ) $(OUT)/obj/test_window.o $(OUT)/obj/null_libs.a
	clang++ $(CXXFLAGS) $(OBJCFLAGS) $(CPPFLAGS) $(CFLAGS) tests/test_present.mm $(OBJ) $(OUT)/obj/test_window.o \
	    $(OUT)/obj/null_libs.a $(LDFLAGS) -o $@

# The tests keep their pipeline archive out of the game's (build/d3d9metal/pso-archive/)
test: export SPORE_METAL_PSO_ARCHIVE = $(OUT)/test-pso-archive.metalar
test: all
	cd $(OUT) && SPORE_D3D9=metal perl -e 'alarm 120; exec @ARGV' ./test_d3d9_metal; rc=$$?; echo "test_d3d9_metal exit $$rc (the unchanged d3d9null suite, SPORE_D3D9=metal; 6 known failures)"; [ $$rc -lt 128 ] || { echo "test_d3d9_metal CRASHED (signal $$((rc-128)))"; exit $$rc; }
	cd $(OUT) && SPORE_D3D9=null perl -e 'alarm 120; exec @ARGV' ./test_d3d9_metal >/dev/null 2>$(OUT)/null-control.log; echo "same binary, SPORE_D3D9=null: exit $$?"; tail -1 $(OUT)/null-control.log
	cd $(OUT) && SPORE_D3D9=metal perl -e 'alarm 120; exec @ARGV' ./test_readback $(OUT); rc=$$?; echo "test_readback exit $$rc"; [ $$rc -lt 128 ] || { echo "test_readback CRASHED (signal $$((rc-128)))"; exit $$rc; }
	cd $(OUT) && SPORE_D3D9=metal perl -e 'alarm 120; exec @ARGV' ./test_present $(OUT)
ifeq ($(WITH_PIPELINE),1)
	@mkdir -p $(OUT)/conformance
	cd $(OUT)/conformance && SPORE_D3D9=metal perl -e 'alarm 120; exec @ARGV' ../test_conformance .
else
	@echo "test_conformance skipped (WITH_PIPELINE=0: the draws are stubs)"
endif

# The shader-compile hitch replay (no game, no window): tests/replay_hitch.m over a private copy of the
# translation cache, before/after x cold/warm. About 4 minutes.
HITCH_SRC := tests/replay_hitch.m pipeline/pl_shader.m shader/d3d9msl.c shader/d3d9msl_cache.c
$(OUT)/replay_hitch: $(HITCH_SRC) pipeline/pl_shader.h include/d3d9metal_int.h shader/d3d9msl.h
	@mkdir -p $(OUT)
	$(CC) $(OBJCFLAGS) $(CPPFLAGS) -Ipipeline -Ishader $(CFLAGS) $(HITCH_SRC) -framework Metal -framework Foundation -o $@
hitch: $(OUT)/replay_hitch
	sh tests/replay_hitch.sh $(OUT) $(HITCH_SCALE) $(HITCH_EXTRA)

clean:
	rm -rf $(OUT)/obj $(OUT)/gen $(OUT)/test_d3d9_metal $(OUT)/test_readback $(OUT)/test_present $(OUT)/test_conformance \
	    $(OUT)/conformance

.PHONY: all test clean hitch
