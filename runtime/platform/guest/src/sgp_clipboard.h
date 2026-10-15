/*
 * sgp_clipboard.h -- the USER32 clipboard over NSPasteboard. Internal to runtime/platform.
 *
 * What the game does (SporeApp.exe, EAClipboard, 0x92abb0..0x92b3b0) [obs, Ghidra]:
 *   - 0x92b220 registers "EAClipboardClass" (wndproc 0x92b0f0), creates the hidden "EAClipboard" window and
 *     makes it a clipboard viewer (SetClipboardViewer at 0x92b30a), keeping the returned next viewer.
 *   - 0x92b0f0 handles WM_CHANGECBCHAIN (0x30d) and WM_DRAWCLIPBOARD (0x308). On WM_DRAWCLIPBOARD it reads the
 *     text (0x92b020: OpenClipboard(0), IsClipboardFormatAvailable(CF_UNICODETEXT), GetClipboardData,
 *     GlobalLock/GlobalSize/GlobalUnlock, CloseClipboard) into the game's own clipboard store, then forwards the
 *     message to the next viewer. That store is what the game's text fields paste from, so outside text only
 *     reaches the game through WM_DRAWCLIPBOARD.
 *   - 0x92ae20 puts an item into the game's store; a text item (type 3) is also written out by 0x92ac90
 *     (OpenClipboard(0), EmptyClipboard, GlobalAlloc(GMEM_MOVEABLE), SetClipboardData(CF_UNICODETEXT),
 *     CloseClipboard). Re-entry is guarded by two counters, so the WM_DRAWCLIPBOARD Windows sends for the
 *     game's own copy is ignored.
 *   - CF_UNICODETEXT (13) is the only format used; RegisterClipboardFormatA/W are not imported. Other formats are
 *     carried anyway (bytes under a private pasteboard type) so the shims are complete.
 *
 * Host side: NSPasteboard through runtime/platform/macos/src/sp_clipboard.m (strong sp_mac_pb_*). Weak
 * stand-ins in sgp_clipboard.c report "no pasteboard" when that file is not linked, and the shims fall back to
 * the back end's clip_get/clip_set (text only, no viewer notifications), today's behaviour. With the headless
 * back end (tests, SPORE_HEADLESS=1) or SPORE_CLIPBOARD=private the clipboard is an in-process store.
 *
 * Viewer notification: the pump (PeekMessageW/A in sgp_msg.c) calls sgp_clip_poll(), which
 * reads the pasteboard's change count at most every SPORE_CLIPBOARD_POLL_MS (default 250; 0 = never) and posts
 * WM_DRAWCLIPBOARD to the viewer when another application changed it. Our own writes are not echoed.
 *
 * No game code.
 */
#ifndef SGP_CLIPBOARD_H
#define SGP_CLIPBOARD_H

#include <stdint.h>

#include "spore_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest handlers (the SG_IMPORT shims in sgp_msg.c tail-call these) ------------------------ */
#define SGP_CLIP_FN(name) void sgp_clip_##name(sg_ctx *SG_RESTRICT ctx, uint8_t *mem)
SGP_CLIP_FN(OpenClipboard);
SGP_CLIP_FN(CloseClipboard);
SGP_CLIP_FN(EmptyClipboard);
SGP_CLIP_FN(IsClipboardFormatAvailable);
SGP_CLIP_FN(GetClipboardData);
SGP_CLIP_FN(SetClipboardData);
SGP_CLIP_FN(SetClipboardViewer);
SGP_CLIP_FN(ChangeClipboardChain);

/* Called by the pump on the window thread. Cheap when nothing changed. */
void sgp_clip_poll(void);

/* ---- host pasteboard (runtime/platform/macos/src/sp_clipboard.m; weak stand-ins otherwise) ---- */
/* Format 13 (CF_UNICODETEXT) is UTF-16LE text without the terminating NUL, stored as the pasteboard's string;
 * any other format is raw bytes under the private type "org.spore-native.clipboard.cf<decimal>". */
long     sp_mac_pb_seq(void);                                    /* change count; -1: no pasteboard */
int      sp_mac_pb_clear(void);                                  /* 1 on success */
int      sp_mac_pb_put(uint32_t fmt, const void *p, uint32_t n); /* adds a format; 1 on success */
uint32_t sp_mac_pb_get(uint32_t fmt, void *buf, uint32_t cap);   /* size in bytes (0: none); copies min(cap) */
int      sp_mac_pb_has(uint32_t fmt);
/* Tests: use a named pasteboard instead of the general one (NULL: back to the general pasteboard). */
void     sp_mac_pb_use_named(const char *name);

/* ---- tests ---------------------------------------------------------------------------------- */
/* Which store the shims use: 'm' NSPasteboard, 'p' in-process, 'l' the back end's text-only hooks. */
char sgp_clip_mode(void);
/* Forgets the viewer and the cached handles, and re-reads the environment (tests). */
void sgp_clip_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* SGP_CLIPBOARD_H */
