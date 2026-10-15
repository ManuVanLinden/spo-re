# runtime/d3d9null -- the null Direct3D 9. Host-only tests: make test; make check-gen
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/d3d9null
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -I$(ROOT)/runtime/com/include -I$(ROOT)/runtime/com/tests -Iinclude -Isrc
SRC     := src/d3d9_adapter.c src/d3d9_device.c $(ROOT)/runtime/com/src/com.c
HDR     := include/d3d9null.h src/d3d9_int.h src/d3d9_slots.h $(ROOT)/runtime/com/include/sg_com.h $(ROOT)/runtime/include/spore_guest.h

all: $(OUT)/test_d3d9

$(OUT)/test_d3d9: $(SRC) $(ROOT)/runtime/com/tests/com_harness.c tests/test_d3d9.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(ROOT)/runtime/com/tests/com_harness.c tests/test_d3d9.c -o $@

test: $(OUT)/test_d3d9 check-gen
	$(OUT)/test_d3d9

check-gen:
	python3 -P gen/gen_slots.py --check

gen:
	python3 -P gen/gen_slots.py

clean:
	rm -rf $(OUT)

.PHONY: all test check-gen gen clean
