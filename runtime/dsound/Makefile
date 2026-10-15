# runtime/dsound -- guest DirectSound. Host-only tests: make test
#   test_dsound     silent back end (no platform audio linked)
#   test_dsound_sp  runtime/platform/macos's sp_dsound.c behind a fake output device
ROOT    := $(abspath ../..)
OUT     := $(ROOT)/build/dsound
PLAT    := $(ROOT)/runtime/platform/macos
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -I$(ROOT)/runtime/com/include -I$(ROOT)/runtime/com/tests -I$(PLAT)/include
SRC     := src/dsound.c $(ROOT)/runtime/com/src/com.c $(ROOT)/runtime/com/tests/com_harness.c tests/test_dsound.c
HDR     := $(ROOT)/runtime/com/include/sg_com.h $(ROOT)/runtime/include/spore_guest.h $(PLAT)/include/sp_dsound.h

all: $(OUT)/test_dsound $(OUT)/test_dsound_sp

$(OUT)/test_dsound: $(SRC) $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) -o $@

$(OUT)/test_dsound_sp: $(SRC) $(PLAT)/src/sp_dsound.c $(HDR)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -I$(PLAT)/src $(CFLAGS) -DWITH_SP -DSP_DSOUND_NO_COREAUDIO $(SRC) $(PLAT)/src/sp_dsound.c -o $@

test: all
	$(OUT)/test_dsound
	$(OUT)/test_dsound_sp

clean:
	rm -rf $(OUT)

.PHONY: all test clean
