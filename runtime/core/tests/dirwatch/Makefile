# runtime/core/tests/dirwatch -- runtime/win32's directory watcher over the real core events.
# A separate binary because the core suite does not link runtime/win32 and runtime/win32's suite
# does not link runtime/core. Links the three libraries as the game binary links their objects.
#   make test      build build/core/dirwatch/dirwatch_core_tests and run it (about 1 s)
ROOT    := $(abspath ../../../..)
OUT     := $(ROOT)/build/core/dirwatch
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(ROOT)/runtime/include -I$(ROOT)/runtime/core/include -I$(ROOT)/runtime/hle/include \
            -I$(ROOT)/runtime/hle/gen -I$(ROOT)/runtime/win32/include -I$(ROOT)/runtime/win32/src
LIBCORE := $(ROOT)/build/core/libcore.a
LIBW32  := $(ROOT)/build/win32/libw32.a
LIBHLE  := $(ROOT)/build/hle/libhle.a

all: $(OUT)/dirwatch_core_tests

deps:
	$(MAKE) -C $(ROOT)/runtime/hle $(LIBHLE)
	$(MAKE) -C $(ROOT)/runtime/core $(LIBCORE)
	$(MAKE) -C $(ROOT)/runtime/win32 $(LIBW32)

$(OUT)/test_dirwatch_core.o: test_dirwatch_core.c
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) -DSG_DISPATCH_CONST= $(CFLAGS) -c $< -o $@

# Link order as in the image: strong w32 and core definitions before libhle's weak stubs.
$(OUT)/dirwatch_core_tests: $(OUT)/test_dirwatch_core.o deps
	$(CC) $(CFLAGS) $(OUT)/test_dirwatch_core.o $(LIBW32) $(LIBCORE) $(LIBHLE) -o $@ -lm -lpthread

test: $(OUT)/dirwatch_core_tests
	cd $(OUT) && perl -e 'alarm 60; exec @ARGV' ./dirwatch_core_tests

clean:
	rm -rf $(OUT)

.PHONY: all deps test clean
