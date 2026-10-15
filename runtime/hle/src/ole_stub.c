/* ole_stub.c -- the six ole32 imports, as a PC on which OLE drag and drop never delivers a drop
 * (docs/design/platform-macos.md 10.11). Strong definitions; they replace the generated
 * "unimplemented" stubs.
 *
 * [obs] App init (0x6bbb30, under 0x7e6850, right after the window is created) calls
 * OleInitialize(NULL) at 0x6bbb3a, then CoLockObjectExternal(target, TRUE, TRUE) and
 * RegisterDragDrop(hwnd, target) for its IDropTarget. Without a drop source nothing ever calls the
 * target, so all three succeed and keep nothing; RevokeDragDrop, OleUninitialize and
 * ReleaseStgMedium (shutdown, and a drop that never arrives) succeed too.
 *
 * These six are weak. runtime/platform/guest/src/sgp_dragdrop.c defines strong ones (the drop target is
 * kept and Finder drops reach it) when hle.h says HLE_OLE32_WEAK. Translated code calls sg_imp_ole32_* directly
 * (SG_CALL), so the override has to be by symbol, not through the import table. A link without
 * runtime/platform keeps these. */
#include "hle.h"
#include "hle_imports.h"

#define S_OK 0u

static int ole_inits;

__attribute__((weak)) SG_IMPORT(ole32, OleInitialize) {
    int n = __atomic_fetch_add(&ole_inits, 1, __ATOMIC_SEQ_CST);
    hle_log(HLE_LOG_INFO, "OleInitialize(0x%x) = %s", SG_ARG(0), n ? "S_FALSE" : "S_OK");
    SG_RETURN_STDCALL(n ? 1u /* S_FALSE: already initialised on this thread */ : S_OK, 4);
}
__attribute__((weak)) SG_IMPORT(ole32, OleUninitialize) {
    __atomic_fetch_sub(&ole_inits, 1, __ATOMIC_SEQ_CST);
    SG_RETURN_STDCALL(0, 0);
}
__attribute__((weak)) SG_IMPORT(ole32, CoLockObjectExternal) {
    hle_log(HLE_LOG_INFO, "CoLockObjectExternal(0x%x, %u, %u) = S_OK (no-op)", SG_ARG(0), SG_ARG(1), SG_ARG(2));
    SG_RETURN_STDCALL(S_OK, 12);
}
__attribute__((weak)) SG_IMPORT(ole32, RegisterDragDrop) {
    hle_log(HLE_LOG_INFO, "RegisterDragDrop(hwnd 0x%x, target 0x%x) = S_OK (no drops are delivered)", SG_ARG(0), SG_ARG(1));
    SG_RETURN_STDCALL(S_OK, 8);
}
__attribute__((weak)) SG_IMPORT(ole32, RevokeDragDrop) { SG_RETURN_STDCALL(S_OK, 4); }
__attribute__((weak)) SG_IMPORT(ole32, ReleaseStgMedium) { SG_RETURN_STDCALL(0, 4); }
