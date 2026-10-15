# runtime/hle -- HLE shim library and its host-only unit tests.
#   make            build build/hle/libhle.a
#   make test       build and run the unit tests (host C only; no game code)
#   make gen        regenerate gen/ from data/imports.json and tools/hle/sigs.py
#   make check-gen  fail if gen/ is stale
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/hle
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -Iinclude -Igen
SRC     := $(wildcard src/*.c) gen/hle_imports.c
OBJ     := $(patsubst %.c,$(OUT)/%.o,$(SRC))
TSRC    := $(wildcard tests/*.c)
TOBJ    := $(patsubst %.c,$(OUT)/%.o,$(TSRC))

all: $(OUT)/libhle.a

$(OUT)/%.o: %.c include/hle.h gen/hle_imports.h $(ROOT)/runtime/include/spore_guest.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/libhle.a: $(OBJ)
	rm -f $@ && ar rcs $@ $^

$(OUT)/hle_tests: $(TOBJ) $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ -lm

test: $(OUT)/hle_tests
	$(OUT)/hle_tests

gen:
	python3 $(ROOT)/tools/hle/gen_thunks.py

check-gen:
	python3 $(ROOT)/tools/hle/gen_thunks.py --check

clean:
	rm -rf $(OUT)

.PHONY: all test gen check-gen clean
