# The macOS platform layer. Our own code only; nothing from the game is built here.
CC      ?= clang
BUILD   ?= ../../../build/platform-macos
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
OBJCFLAGS ?= -fobjc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter
INC     = -Iinclude -Isrc -Itests
CORE    = src/sp_queue.c src/sp_input.c src/sp_keymap.c
DSOUND  = src/sp_dsound.c src/sp_coreaudio.c
FW      = -framework AppKit -framework QuartzCore -framework Metal -framework CoreAudio -framework AudioToolbox

all: $(BUILD)/script_headless $(BUILD)/test_core $(BUILD)/sphost $(BUILD)/audio_drift $(BUILD)/test_gamepad $(BUILD)/padtest

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/script_headless: $(CORE) tests/script_runner.c tests/script_headless.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

$(BUILD)/test_core: $(CORE) src/sp_dsound.c tests/test_core.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -DSP_DSOUND_NO_COREAUDIO $^ -o $@

$(BUILD)/sphost: $(CORE) $(DSOUND) src/sp_window.m tests/script_runner.c tests/fake_dac.c tests/hostapp.m | $(BUILD)
	$(CC) $(OBJCFLAGS) $(INC) $^ $(FW) -o $@

$(BUILD)/audio_drift: src/sp_dsound.c src/sp_coreaudio.c tests/fake_dac.c tests/audio_drift.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $^ -framework CoreAudio -framework AudioToolbox -o $@

# Game controllers: the four XInput users with a fake back end (headless), and the GameController back end
# with virtual pads plus whatever real pad is attached (no window; padtest --seconds N --rumble to feel it).
PADFW   = -framework Foundation -framework GameController -framework CoreHaptics
$(BUILD)/test_gamepad: src/sp_gamepad_core.c tests/test_gamepad.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $^ -o $@ -lpthread
$(BUILD)/padtest: src/sp_gamepad_core.c src/sp_gamepad.m tests/padtest.m | $(BUILD)
	$(CC) $(OBJCFLAGS) $(INC) $^ $(PADFW) -o $@
check-pad: $(BUILD)/test_gamepad $(BUILD)/padtest
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/test_gamepad
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/padtest --seconds 2

# Headless checks (no window, no audio device).
check: $(BUILD)/script_headless $(BUILD)/test_core $(BUILD)/test_gamepad
	$(BUILD)/test_core
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/test_gamepad
	$(BUILD)/script_headless tests/input-script.txt $(BUILD)/headless-wm.log
	tests/compare_logs.sh tests/expected-wm.log $(BUILD)/headless-wm.log

# AppKit run: opens a window for a few seconds and replays the script through real NSEvents.
check-gui: $(BUILD)/sphost
	$(BUILD)/sphost --script tests/input-script.txt --log $(BUILD)/gui-wm.log
	tests/compare_logs.sh tests/expected-wm.log $(BUILD)/gui-wm.log
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --display-test
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --display-test --retina
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --display-test --no-fullscreen
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --safe-area-test
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --safe-area-test --retina
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/sphost --text-test

# sp_mac_open_url (browser links). check-open opens nothing; check-open-real opens https://example.com once.
$(BUILD)/test_open: src/sp_open.m tests/test_open.m | $(BUILD)
	$(CC) $(OBJCFLAGS) $(INC) $^ -framework AppKit -o $@

check-open: $(BUILD)/test_open
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/test_open

check-open-real: $(BUILD)/test_open
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/test_open --real

clean:
	rm -rf $(BUILD)

.PHONY: all check check-gui check-pad check-open check-open-real clean
