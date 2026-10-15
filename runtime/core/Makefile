# runtime/core -- runtime core library and its fixture tests.
#   make            build build/core/libcore.a
#   make test       build and run the fixture tests (our own tables and functions; no game code)
#   make test-san   the same under ASan + UBSan
#   make test-tsan  the thread tests under ThreadSanitizer
#   make test-hle   link the core against runtime/hle and check the shim registration
#   make test-dirwatch  runtime/win32's directory watcher over the real core events (tests/dirwatch)
ROOT    := $(abspath ../..)
OUT     ?= $(ROOT)/build/core
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -Iinclude -Isrc
SRC     := $(filter-out src/core_hle_glue.c src/core_boot.c src/core_overrides.c,$(wildcard src/*.c))
# Strong sg_imp_* overrides (src/core_overrides.c) are compiled in once runtime/hle's generated
# stubs are weak (tools/hle/gen_thunks.py emits them so). Override with SG_CORE_OVERRIDE=0|1.
HLE_GEN ?= $(ROOT)/runtime/hle/gen/hle_imports.c
SG_CORE_OVERRIDE ?= $(shell grep -q '__attribute__((weak)) SG_IMPORT' $(HLE_GEN) && echo 1 || echo 0)
OBJ     := $(patsubst %.c,$(OUT)/%.o,$(SRC))
TSRC    := $(wildcard tests/*.c)
TOBJ    := $(patsubst %.c,$(OUT)/%.o,$(TSRC))
HDRS    := include/sg_core.h src/core_internal.h $(ROOT)/runtime/include/spore_guest.h tests/fixture.h

all: $(OUT)/libcore.a

$(OUT)/src/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/tests/%.o: tests/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DSG_DISPATCH_CONST= $(CFLAGS) -c $< -o $@

$(OUT)/src/core_hle_glue.o: src/core_hle_glue.c $(HDRS) $(ROOT)/runtime/hle/include/hle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -I$(ROOT)/runtime/hle/include $(CFLAGS) -c $< -o $@

$(OUT)/src/core_boot.o: src/core_boot.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/src/core_overrides.o: src/core_overrides.c $(HDRS) $(HLE_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DSG_CORE_OVERRIDE_HLE_STUBS=$(SG_CORE_OVERRIDE) $(CFLAGS) -c $< -o $@

$(OUT)/libcore.a: $(OBJ) $(OUT)/src/core_hle_glue.o $(OUT)/src/core_boot.o $(OUT)/src/core_overrides.o
	rm -f $@ && ar rcs $@ $^

$(OUT)/core_tests: $(TOBJ) $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ -lm -lpthread

test: $(OUT)/core_tests
	perl -e 'alarm 300; exec @ARGV' $(OUT)/core_tests

test-san:
	$(MAKE) OUT=$(ROOT)/build/core/san CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" test

test-tsan:
	$(MAKE) OUT=$(ROOT)/build/core/tsan CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=thread" $(ROOT)/build/core/tsan/core_tests
	TSAN_OPTIONS=halt_on_error=1 perl -e 'alarm 300; exec @ARGV' $(ROOT)/build/core/tsan/core_tests threads

# Link check against runtime/hle's import table (no game code): builds runtime/hle, then a small program that
# calls sg_core_register_hle_shims + hle_install_imports on a scratch window.
test-hle: $(OUT)/libcore.a
	$(MAKE) -C $(ROOT)/runtime/hle
	@mkdir -p $(OUT)/hle
	$(CC) $(CPPFLAGS) -I$(ROOT)/runtime/hle/include $(CFLAGS) tests-hle/test_hle_glue.c $(OUT)/libcore.a \
	  $(ROOT)/build/hle/libhle.a -o $(OUT)/hle/test_hle_glue -lm
	perl -e 'alarm 60; exec @ARGV' $(OUT)/hle/test_hle_glue

test-dirwatch:
	$(MAKE) -C tests/dirwatch test

clean:
	rm -rf $(OUT)

.PHONY: all test test-san test-tsan test-hle test-dirwatch clean
