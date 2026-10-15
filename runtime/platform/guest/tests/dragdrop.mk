# The drag and drop test, layered on the package Makefile (its variables and libsgp_headless.a):
#   make -C runtime/platform/guest -f Makefile -f tests/dragdrop.mk test-dragdrop
# Host-only; reads the game's own creation cards in place from build/harness/sandbox (never copies them).
# A private sgp_dragdrop.o built with -DHLE_OLE32_WEAK=1 carries the strong sg_imp_ole32_* (what the game gets with
# runtime/hle's ole32 stubs weak); it comes first, so libsgp_headless.a's copy and
# ole_stub.o in libhle.a are not pulled. Linked without -force_load, so only the archive members needed come in.
$(OUT)/tests/sgp_dragdrop_ole.o: src/sgp_dragdrop.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(INC) -DHLE_OLE32_WEAK=1 $(CFLAGS) -c $< -o $@
$(OUT)/tests/test_dragdrop.o: tests/test_dragdrop.c $(HDRS) tests/fixture.h
	@mkdir -p $(dir $@)
	$(CC) $(INC) -I$(PM)/src -I$(PM)/tests -DSG_DISPATCH_CONST= $(CFLAGS) -c $< -o $@
$(OUT)/dragdrop_tests: $(OUT)/tests/test_dragdrop.o $(OUT)/tests/sgp_dragdrop_ole.o $(OUT)/tests/fixture.o $(OUT)/libsgp_headless.a deps
	$(CC) $(CFLAGS) $(OUT)/tests/test_dragdrop.o $(OUT)/tests/sgp_dragdrop_ole.o $(OUT)/tests/fixture.o $(OUT)/libsgp_headless.a \
	  $(LIBCORE) $(LIBHLE) -o $@ -lm -lpthread
	@nm -m $@ | grep -q 'external _sg_imp_ole32_RegisterDragDrop$$' && ! nm -m $@ | grep -q 'weak external _sg_imp_ole32_RegisterDragDrop' \
	  && echo "dragdrop_tests: sg_imp_ole32_RegisterDragDrop is sgp_dragdrop.c's (strong)"
test-dragdrop: $(OUT)/dragdrop_tests
	SPORE_REPO=$(ROOT) perl -e 'alarm 60; exec @ARGV' $(OUT)/dragdrop_tests
.PHONY: test-dragdrop
