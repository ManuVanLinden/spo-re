/* k32_nls_weak.c -- weak "unimplemented" stand-ins for two NLS imports. Defining them here makes the generator
 * count them as implemented, so hle_imports.c has no weak stub for them; these stand-ins let binaries that link
 * runtime/hle without runtime/win32 (the hle and platform/guest test suites) link. runtime/win32's strong
 * w32_nls.c shims win in the game binary. */
#include "hle.h"
#include "hle_imports.h"

__attribute__((weak)) SG_IMPORT(kernel32, GetSystemDefaultLCID) {
    hle_unimplemented(ctx, mem, HLE_IMP_kernel32_GetSystemDefaultLCID);
    SG_RET(0);
}
__attribute__((weak)) SG_IMPORT(kernel32, GetLocaleInfoW) {
    hle_unimplemented(ctx, mem, HLE_IMP_kernel32_GetLocaleInfoW);
    SG_RET(16);
}
