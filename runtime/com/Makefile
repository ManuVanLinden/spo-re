# runtime/com -- guest-visible COM machinery. Host-only tests: make test
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/com
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -Iinclude -Itests
SRC     := src/com.c
HDR     := include/sg_com.h tests/com_harness.h $(ROOT)/runtime/include/spore_guest.h

all: $(OUT)/test_com

$(OUT)/test_com: $(SRC) tests/com_harness.c tests/test_com.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) tests/com_harness.c tests/test_com.c -o $@

test: $(OUT)/test_com
	$(OUT)/test_com

clean:
	rm -rf $(OUT)

.PHONY: all test clean
