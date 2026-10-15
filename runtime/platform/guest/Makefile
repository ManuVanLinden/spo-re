# runtime/platform/guest -- the guest-facing USER32/GDI32/IMM32/XInput/DirectInput8 imports
# over runtime/platform/macos. Our own code only; no game code is built or run here.
#   make            build/platform-guest/libsgp.a (headless + AppKit back ends, platform layer)
#   make test       host-only fixture: a hand-written guest window procedure driven through the
#                   imports by the reference input script, plus the core event API checks
#   make check-gen  fail if src/sgp_list.h is stale
#   make nm         show that the strong sg_imp_* definitions win over runtime/hle's weak ones
ROOT    := $(abspath ../../..)
OUT     := $(ROOT)/build/platform-guest
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
INC     := -I$(ROOT)/runtime/include -I$(ROOT)/runtime/hle/include -I$(ROOT)/runtime/hle/gen \
           -I$(ROOT)/runtime/core/include -I$(ROOT)/runtime/platform/macos/include -Iinclude -Isrc
PM      := $(ROOT)/runtime/platform/macos
PMSRC   := $(PM)/src/sp_queue.c $(PM)/src/sp_input.c $(PM)/src/sp_keymap.c $(PM)/src/sp_gamepad_core.c
SRC     := $(wildcard src/*.c)
HDRS    := $(wildcard include/*.h src/*.h) $(wildcard $(PM)/include/*.h) $(ROOT)/runtime/include/spore_guest.h \
           $(ROOT)/runtime/core/include/sg_core.h
OBJ     := $(patsubst src/%.c,$(OUT)/%.o,$(filter-out src/sgp_backend_mac.c,$(SRC)))
PMOBJ   := $(patsubst $(PM)/src/%.c,$(OUT)/pm/%.o,$(PMSRC))
FW      := -framework AppKit -framework QuartzCore -framework Metal -framework CoreAudio -framework AudioToolbox \
           -framework GameController -framework CoreHaptics

all: $(OUT)/libsgp.a

$(OUT)/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(INC) $(CFLAGS) -c $< -o $@
$(OUT)/pm/%.o: $(PM)/src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src $(CFLAGS) -c $< -o $@
$(OUT)/sgp_backend_mac.o: src/sgp_backend_mac.c $(HDRS)
	$(CC) $(INC) $(CFLAGS) -c $< -o $@
$(OUT)/pm/sp_window.o: $(PM)/src/sp_window.m $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src -fobjc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter -c $< -o $@

$(OUT)/pm/sp_gamepad.o: $(PM)/src/sp_gamepad.m $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src -fobjc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter -c $< -o $@

# Headless library (tests): no AppKit.
$(OUT)/libsgp_headless.a: $(OBJ) $(PMOBJ)
	rm -f $@ && ar rcs $@ $^
# Full library: + AppKit back end and window host.
$(OUT)/libsgp.a: $(OBJ) $(PMOBJ) $(OUT)/sgp_backend_mac.o $(OUT)/pm/sp_window.o $(OUT)/pm/sp_gamepad.o
	rm -f $@ && ar rcs $@ $^

LIBHLE  := $(ROOT)/build/hle/libhle.a
LIBCORE := $(ROOT)/build/core/libcore.a
deps:
	$(MAKE) -C $(ROOT)/runtime/hle $(LIBHLE)
	$(MAKE) -C $(ROOT)/runtime/core $(LIBCORE)

TOBJ := $(OUT)/tests/fixture.o $(OUT)/tests/test_guest.o
$(OUT)/tests/%.o: tests/%.c $(HDRS) tests/fixture.h
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src -I$(PM)/tests -DSG_DISPATCH_CONST= $(CFLAGS) -c $< -o $@
$(OUT)/tests/script_runner.o: $(PM)/tests/script_runner.c
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src -I$(PM)/tests $(CFLAGS) -c $< -o $@

# Link order: our strong sg_imp_* first, then libcore (strong core shims), then libhle (weak stubs).
$(OUT)/guest_tests: $(TOBJ) $(OUT)/tests/script_runner.o $(OUT)/libsgp_headless.a deps
	$(CC) $(CFLAGS) $(TOBJ) $(OUT)/tests/script_runner.o -Wl,-force_load,$(OUT)/libsgp_headless.a $(LIBCORE) $(LIBHLE) \
	  -o $@ -lm -lpthread

# The XInput imports over a fake gamepad back end.
$(OUT)/xinput_tests: $(OUT)/tests/fixture.o $(OUT)/tests/test_xinput.o $(OUT)/libsgp_headless.a deps
	$(CC) $(CFLAGS) $(OUT)/tests/fixture.o $(OUT)/tests/test_xinput.o -Wl,-force_load,$(OUT)/libsgp_headless.a $(LIBCORE) $(LIBHLE) \
	  -o $@ -lm -lpthread

test: $(OUT)/guest_tests $(OUT)/xinput_tests
	cd $(OUT) && perl -e 'alarm 120; exec @ARGV' ./guest_tests $(PM)/tests/input-script.txt $(PM)/tests/expected-wm.log $(PM)/tests/compare_logs.sh
	cd $(OUT) && perl -e 'alarm 60; exec @ARGV' ./xinput_tests

# AppKit smoke: the same imports over the real back end; a window shows for about a second.
$(OUT)/guest_gui: $(TOBJ) $(OUT)/tests/script_runner.o $(OUT)/libsgp.a deps
	$(CC) $(CFLAGS) $(TOBJ) $(OUT)/tests/script_runner.o -Wl,-force_load,$(OUT)/libsgp.a $(LIBCORE) $(LIBHLE) \
	  -o $@ -lm -lpthread $(FW)
check-gui: $(OUT)/guest_gui
	cd $(OUT) && perl -e 'alarm 60; exec @ARGV' ./guest_gui --gui

check-gen:
	python3 gen_list.py --check

nm: $(OUT)/guest_tests
	@echo "libhle's definitions are weak; ours are strong; the linked fixture keeps ours (one T per symbol):"
	@nm -m $(LIBHLE) | grep -E 'weak external _sg_imp_user32_(PeekMessageW|CreateWindowExW)$$' | sort -u
	@nm -m $(OUT)/libsgp.a | grep -E ' external _sg_imp_user32_(PeekMessageW|CreateWindowExW)$$' | grep -v weak | grep -v undefined
	@sed -n 's/^SGP_SHIM(\([a-z0-9_]*\), \([A-Za-z0-9_]*\), .*/_sg_imp_\1_\2/p' src/sgp_list.h | sort > $(OUT)/ours.txt
	@nm -m $(OUT)/guest_tests | awk '/__TEXT,__text/ && / external _sg_imp_/ && !/weak/ {print $$NF}' | sort > $(OUT)/strong.txt
	@echo "imports in sgp_list.h: $$(wc -l < $(OUT)/ours.txt | tr -d ' '); of them linked as strong (non-weak) text symbols: $$(comm -12 $(OUT)/ours.txt $(OUT)/strong.txt | wc -l | tr -d ' ')"

clean:
	rm -rf $(OUT)

.PHONY: all deps test check-gui check-gen nm clean
