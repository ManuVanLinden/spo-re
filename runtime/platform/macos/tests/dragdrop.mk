# The drag and drop host case, layered on the package Makefile:
#   make -C runtime/platform/macos -f Makefile -f tests/dragdrop.mk check-dragdrop
# Opens one small window for about a second. The dropped files are two of the game's own cards, read in place
# from build/harness/sandbox (never copied); without any, the test makes two empty .png files under /tmp.
$(BUILD)/test_dragdrop: $(CORE) src/sp_window.m src/sp_dragdrop.m tests/test_dragdrop.m | $(BUILD)
	$(CC) $(OBJCFLAGS) $(INC) $^ $(FW) -o $@
check-dragdrop: $(BUILD)/test_dragdrop
	perl -e 'alarm 60; exec @ARGV' $(BUILD)/test_dragdrop $(abspath ../../../build/harness/sandbox)
.PHONY: check-dragdrop
