# clipboard.mk -- a private link of the guest platform layer with the clipboard shims in sgp_clipboard.c.
# Copies src/sgp_msg.c (whose shims forward to sgp_clipboard.c and whose pump polls) under build/platform-guest-clipboard/, links it
# with every other src/*.c, and runs
#   clip_tests        test_clipboard.c over the in-process store (headless, no AppKit)
#   clip_tests_mac    the same plus a NAMED NSPasteboard (sp_clipboard.m; the general pasteboard is not touched)
#   guest_tests_clipboard  the unchanged guest suite (test_guest.c + the 204-line replay) against the patched layer
# Usage (from the top of the source tree):  make -f runtime/platform/guest/tests/clipboard.mk test
ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
G       := $(ROOT)/runtime/platform/guest
PM      := $(ROOT)/runtime/platform/macos
OUT     := $(ROOT)/build/platform-guest-clipboard
CC      ?= clang
CFLAGS  := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
INC     := -I$(ROOT)/runtime/include -I$(ROOT)/runtime/hle/include -I$(ROOT)/runtime/hle/gen \
           -I$(ROOT)/runtime/core/include -I$(PM)/include -I$(G)/include -I$(G)/src -I$(PM)/src -I$(PM)/tests -I$(G)/tests
# OVR=dir: a file dir/<name>.c replaces src/<name>.c (e.g. HEAD copies of files another package has half-edited:
#   mkdir -p build/platform-guest-clipboard/head && git show HEAD:runtime/platform/guest/src/sgp_imm.c > build/platform-guest-clipboard/head/sgp_imm.c).
OVR     ?= $(OUT)/none
pick     = $(if $(wildcard $(OVR)/$(notdir $(1))),$(OVR)/$(notdir $(1)),$(1))
GSRC    := $(foreach f,$(filter-out $(G)/src/sgp_msg.c $(G)/src/sgp_backend_mac.c,$(wildcard $(G)/src/*.c)),$(call pick,$(f))) \
           $(OUT)/p/sgp_msg.c $(PM)/src/sp_queue.c $(PM)/src/sp_input.c $(PM)/src/sp_keymap.c $(PM)/src/sp_gamepad_core.c
LIBS    := $(ROOT)/build/core/libcore.a $(ROOT)/build/hle/libhle.a -lm -lpthread

.PHONY: test patched
patched:
	@mkdir -p $(OUT)/p
	@cp $(G)/src/sgp_msg.c $(OUT)/p/sgp_msg.c

$(OUT)/clip_tests: patched
	$(MAKE) -C $(G) deps >/dev/null
	$(CC) $(INC) $(CFLAGS) -DSG_DISPATCH_CONST= $(GSRC) $(G)/tests/fixture.c $(G)/tests/test_clipboard.c $(LIBS) -o $@
$(OUT)/clip_tests_mac: patched
	$(CC) $(INC) $(CFLAGS) -DSG_DISPATCH_CONST= -c $(PM)/src/sp_clipboard.m -fobjc-arc -o $(OUT)/sp_clipboard.o
	$(CC) $(INC) $(CFLAGS) -DSG_DISPATCH_CONST= $(GSRC) $(OUT)/sp_clipboard.o $(G)/tests/fixture.c \
	  $(G)/tests/test_clipboard.c $(LIBS) -framework AppKit -o $@
$(OUT)/guest_tests_clipboard: patched
	$(CC) $(INC) $(CFLAGS) -DSG_DISPATCH_CONST= $(GSRC) $(G)/tests/fixture.c $(G)/tests/test_guest.c \
	  $(PM)/tests/script_runner.c $(LIBS) -o $@

test: $(OUT)/clip_tests $(OUT)/clip_tests_mac $(OUT)/guest_tests_clipboard
	cd $(OUT) && perl -e 'alarm 60; exec @ARGV' ./clip_tests
	cd $(OUT) && perl -e 'alarm 60; exec @ARGV' ./clip_tests_mac --mac
	cd $(OUT) && perl -e 'alarm 120; exec @ARGV' ./guest_tests_clipboard $(PM)/tests/input-script.txt \
	  $(PM)/tests/expected-wm.log $(PM)/tests/compare_logs.sh
