# runtime/win32 -- CRT start-up, guest heap and VM, modules, environment, files, registry, paths.
#   make            build build/win32/libw32.a
#   make test       build and run the host-only unit tests (no game code; links build/hle/libhle.a)
#   make gen        regenerate src/w32_shims.h from the W32_IMPORT definitions
#   make check-gen  fail if src/w32_shims.h is stale
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/win32
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -I$(ROOT)/runtime/hle/include -I$(ROOT)/runtime/hle/gen -Iinclude -Isrc
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst %.c,$(OUT)/%.o,$(SRC))
TSRC    := $(wildcard tests/*.c)
TOBJ    := $(patsubst %.c,$(OUT)/%.o,$(TSRC))
HDRS    := include/w32.h src/w32_int.h src/w32_json.h src/w32_shims.h $(ROOT)/runtime/include/spore_guest.h \
           $(ROOT)/runtime/hle/include/hle.h
LIBHLE  := $(ROOT)/build/hle/libhle.a
# Strong sg_imp_* forwarders (src/w32_overrides.c) only when runtime/hle's generated stubs are weak
# (detected below from gen/hle_imports.c). Override with W32_OVERRIDE=0|1.
HLE_GEN := $(ROOT)/runtime/hle/gen/hle_imports.c
W32_OVERRIDE ?= $(shell grep -q '__attribute__((weak)) SG_IMPORT' $(HLE_GEN) && echo 1 || echo 0)
CPPFLAGS += -DW32_OVERRIDE_HLE_STUBS=$(W32_OVERRIDE)

all: $(OUT)/libw32.a

$(OUT)/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/src/w32_overrides.o: $(HLE_GEN)

$(OUT)/libw32.a: $(OBJ)
	rm -f $@ && ar rcs $@ $^

$(LIBHLE): FORCE
	$(MAKE) -C $(ROOT)/runtime/hle $(LIBHLE)

# The runtime objects are linked as objects, not through libw32.a: an archive member is pulled only when a symbol
# is still undefined, and libhle.a's weak stubs (loaded early through the import table) would otherwise satisfy a
# shim that lives only in a new file (such as w32_crypt.c). The game link does the same (tools/link/build.py).
$(OUT)/w32_tests: $(TOBJ) $(OBJ) $(LIBHLE)
	$(CC) $(CFLAGS) $(TOBJ) $(OBJ) $(LIBHLE) -o $@ -lm -lpthread

test: $(OUT)/w32_tests
	cd $(OUT) && ./w32_tests

# Same tests with a case-sensitive APFS image attached, so the case-insensitive fallback scan runs.
test-cs: $(OUT)/w32_tests
	@test -f $(OUT)/cs.dmg || hdiutil create -size 16m -fs "Case-sensitive APFS" -volname w32cs -ov $(OUT)/cs.dmg >/dev/null
	@mkdir -p $(OUT)/csmnt && (mount | grep -q "$(OUT)/csmnt" || hdiutil attach -nobrowse -mountpoint $(OUT)/csmnt $(OUT)/cs.dmg >/dev/null)
	cd $(OUT) && SPORE_W32_CS_DIR=$(OUT)/csmnt ./w32_tests; rc=$$?; hdiutil detach $(OUT)/csmnt >/dev/null; exit $$rc

gen:
	python3 $(ROOT)/tools/win32/gen_shims.py

check-gen:
	python3 $(ROOT)/tools/win32/gen_shims.py --check

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all test test-cs gen check-gen clean FORCE
