/* sgp_imm.c -- IMM32 over the text-input state (dead keys and input methods through the game view's
 * NSTextInputClient; docs/design/platform-macos.md section 12).
 *
 * The game's IME code (UI/IME/IMEServer, 0x84a3a1-0x84bc85) asks ImmGetContext for every operation and
 * takes its no-IME path when it gets NULL; with a context it handles WM_IME_STARTCOMPOSITION /
 * WM_IME_COMPOSITION / WM_IME_ENDCOMPOSITION itself (listener 0x84b6e0, which marks them handled so they
 * never reach DefWindowProc) and reads GCS_COMPATTR, GCS_COMPSTR, GCS_RESULTSTR and GCS_CURSORPOS with
 * ImmGetCompositionStringW (0x84ae20). It closes the IME (ImmSetOpenStatus FALSE) when a composition starts
 * and no text field wants it (0x84ba1a), completes compositions with ImmNotifyIME(NI_COMPOSITIONSTR,
 * CPS_COMPLETE) for fields that take no inline composition (0x84b8ac), and cancels with CPS_CANCEL
 * (0x84a581). SPORE_TEXT_INPUT=0: ImmGetContext NULL (no IME, the game's no-IME path). */
#include "sgp_int.h"
#include "sp_macos.h"

/* The text-input core, compiled here once for every binary with the guest layer (see its BUILD NOTE). */
#include "../../macos/src/sp_textinput.inc"

static int sgp_himc_ok(uint32_t himc) { return himc && (himc & 0xFFFF0000u) == 0x494D0000u && sp_ti_enabled(); }

SG_IMPORT(imm32, ImmDisableTextFrameService) { SG_RETURN_STDCALL(1, 4); }
SG_IMPORT(imm32, ImmGetContext) { SG_RETURN_STDCALL(sp_ti_context(SG_ARG(0)), 4); }
SG_IMPORT(imm32, ImmReleaseContext) { SG_RETURN_STDCALL(SG_ARG(1) != 0, 8); }
SG_IMPORT(imm32, ImmGetOpenStatus) { SG_RETURN_STDCALL(sgp_himc_ok(SG_ARG(0)) ? sp_ti_open_status() : 0, 4); }
SG_IMPORT(imm32, ImmSetOpenStatus) {
    SG_RETURN_STDCALL(sgp_himc_ok(SG_ARG(0)) ? sp_ti_set_open((int)SG_ARG(1)) : 0, 8);
}
SG_IMPORT(imm32, ImmGetConversionStatus) {
    int ok = sgp_himc_ok(SG_ARG(0));
    if (SG_ARG(1)) sg_st32(mem, SG_ARG(1), ok ? sp_ti_conversion() : 0);
    if (SG_ARG(2)) sg_st32(mem, SG_ARG(2), 0);           /* IME_SMODE_NONE */
    SG_RETURN_STDCALL(ok, 12);
}
SG_IMPORT(imm32, ImmSetConversionStatus) { SG_RETURN_STDCALL(sgp_himc_ok(SG_ARG(0)), 12); }
/* LONG ImmGetCompositionStringW(HIMC, DWORD index, LPVOID buf, DWORD bytes): sizes in bytes; bytes 0 asks
 * for the size; GCS_CURSORPOS and GCS_DELTASTART return the value itself. */
SG_IMPORT(imm32, ImmGetCompositionStringW) {
    uint32_t himc = SG_ARG(0), index = SG_ARG(1), buf = SG_ARG(2), bytes = SG_ARG(3);
    if (!sgp_himc_ok(himc)) SG_RETURN_STDCALL((uint32_t)-2, 16);   /* IMM_ERROR_GENERAL */
    if (!buf) bytes = 0;
    SG_RETURN_STDCALL((uint32_t)sp_ti_comp_string(index, bytes ? SG_HOSTPTR(mem, buf) : NULL, bytes), 16);
}
/* The game sets the composition string only to re-show its own copy (0x84ac60); a macOS input method's
 * marked text cannot be replaced from outside, so this fails and the game keeps the IM's string. */
SG_IMPORT(imm32, ImmSetCompositionStringW) { SG_RETURN_STDCALL(0, 24); }
SG_IMPORT(imm32, ImmGetCandidateListW) { SG_RETURN_STDCALL(0, 16); }   /* the system IM shows its own list */
SG_IMPORT(imm32, ImmNotifyIME) {
    SG_RETURN_STDCALL(sgp_himc_ok(SG_ARG(0)) ? sp_ti_notify(SG_ARG(1), SG_ARG(2), SG_ARG(3)) : 0, 16);
}
SG_IMPORT(imm32, ImmSimulateHotKey) { SG_RETURN_STDCALL(0, 8); }
