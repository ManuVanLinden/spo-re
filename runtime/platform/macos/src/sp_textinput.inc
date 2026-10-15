/*
 * sp_textinput.inc - the text input core: dead keys and input methods -> WM_CHAR / WM_IME_*, and the
 * state the IMM32 imports answer from. Plain C, no AppKit; the AppKit half (NSTextInputClient on the
 * game view) is in sp_window.m. docs/design/platform-macos.md section 12.
 *
 * BUILD NOTE: this file is compiled exactly once per binary by #include, never from a build list:
 *   - runtime/platform/guest/src/sgp_imm.c includes it (the game image via tools/link/build.py, and both
 *     guest libraries via the guest Makefile wildcard over src);
 *   - runtime/platform/macos/tests/hostapp.m includes it (sphost, which links no guest code).
 * sp_window.m carries weak stand-ins for every function it calls here, so a binary without this core
 * (another package's test app) still links, with keys going straight to translate_key. Do not add this file to
 * a source list: that would define every symbol twice.
 *
 * Model (Windows behaviour, with the evidence in section 12 of platform-macos.md):
 *   - Dead keys (a keyboard layout's marked text, e.g. Option-e then e) are a layout feature on Windows:
 *     the dead key's WM_KEYDOWN carries no WM_CHAR and the next key's WM_KEYDOWN carries the composed
 *     character (WM_CHAR 0xE9). No WM_IME_* message.
 *   - Input methods (Japanese, Chinese, Korean ...): keys the IM consumes arrive as WM_KEYDOWN
 *     VK_PROCESSKEY (0xE5) without a character, then WM_IME_STARTCOMPOSITION once,
 *     WM_IME_COMPOSITION(GCS_COMPSTR|...) per change, and on commit WM_IME_COMPOSITION(GCS_RESULTSTR)
 *     then WM_IME_ENDCOMPOSITION. The game's IME listener (0x84b6e0) consumes all three itself and reads
 *     the strings with ImmGetCompositionStringW; a window procedure that passes WM_IME_COMPOSITION to
 *     DefWindowProc gets WM_IME_CHAR and then WM_CHAR per UTF-16 unit (sp_ti_def_window_proc).
 *   - Text an input source inserts without composing (Option-a = "å" etc.) uses today's path when it is
 *     what NSEvent.characters already said, so plain typing is byte-identical to the reference-script replay;
 *     otherwise the key-down carries the inserted UTF-16 units (a surrogate pair is two WM_CHARs).
 * All messages go to the input queue in order (the key-down first), as Windows' IME generates them
 * while the key-down is translated. Everything runs on the window (main) thread.
 */
#include <stdlib.h>
#include <string.h>

#include "sp_internal.h"
#include "sp_macos.h"

#ifndef WM_IME_STARTCOMPOSITION
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION   0x010E
#define WM_IME_COMPOSITION      0x010F
#endif
#ifndef WM_IME_CHAR
#define WM_IME_CHAR             0x0286
#endif
#ifndef VK_PROCESSKEY
#define VK_PROCESSKEY           0xE5
#endif

#define TI_MAX 256
#define TI_PENDING 64

static struct {
    int init, enabled, trace;
    int kind, force_kind;          /* SP_TI_LAYOUT / SP_TI_IM; force_kind -1 = follow the source */
    int open;                      /* the IME open status the game sees (ImmGet/SetOpenStatus) */
    int comp_kind;                 /* kind of the current marked text */
    int started;                   /* WM_IME_STARTCOMPOSITION sent, no WM_IME_ENDCOMPOSITION yet */
    uint16_t comp[TI_MAX];
    uint8_t attr[TI_MAX];
    int comp_len, sel_loc, sel_len;
    uint16_t result[TI_MAX];
    int result_len;
    int discarding;                /* inside the view's discardMarkedText: ignore its callbacks */
    void (*discard)(void);
    /* the key being interpreted */
    int in_key, k_ops, k_ime, k_had_marked;
    uint16_t k_ins[TI_MAX];
    int k_ins_len;
    sp_qent k_pending[TI_PENDING];
    int k_npending;
    char source[128];
} T = {.force_kind = -1, .open = 1};

static void ti_init(void)
{
    if (T.init) return;
    T.init = 1;
    const char *e = getenv("SPORE_TEXT_INPUT");
    T.enabled = !(e && e[0] == '0');
    e = getenv("SPORE_TEXT_INPUT_TRACE");
    T.trace = e && e[0] && e[0] != '0';
}

#define TI_TRACE(...) do { if (T.trace) { fprintf(stderr, "textinput: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int ti_kind(void) { return T.force_kind >= 0 ? T.force_kind : T.kind; }

int sp_ti_enabled(void) { ti_init(); return T.enabled; }

void sp_ti_set_source(int kind, const char *id)
{
    ti_init();
    if (id && strncmp(id, T.source, sizeof T.source - 1) != 0) {
        snprintf(T.source, sizeof T.source, "%s", id);
        T.open = 1;                                 /* a newly chosen input source is open, as on Windows */
        TI_TRACE("source %s (%s)", id, kind == SP_TI_IM ? "input method" : "keyboard layout");
    }
    T.kind = kind;
}

void sp_ti_force_kind(int kind) { ti_init(); T.force_kind = kind; }
void sp_ti_set_discard_hook(void (*fn)(void)) { T.discard = fn; }

void sp_ti_reset(void)
{
    void (*d)(void) = T.discard;
    int init = T.init, en = T.enabled, tr = T.trace;
    memset(&T, 0, sizeof T);
    T.force_kind = -1; T.open = 1; T.discard = d;
    T.init = init; T.enabled = en; T.trace = tr;
}

/* ---- messages -------------------------------------------------------------------------------------- */
static void ti_fill(sp_qent *e, uint32_t msg, uint32_t w, int32_t l)
{
    memset(e, 0, sizeof *e);
    e->m.hwnd = sp__key_target();
    e->m.message = msg; e->m.wParam = w; e->m.lParam = l;
    e->m.time = sp_tick_ms();
    if (e->m.hwnd) {
        int32_t x = 0, y = 0;
        sp_get_cursor_client(e->m.hwnd, &x, &y);
        sp__client_to_screen(e->m.hwnd, &x, &y);
        e->m.pt.x = x; e->m.pt.y = y;
    }
}

static void ti_msg(uint32_t msg, uint32_t w, int32_t l)
{
    sp_qent e;
    ti_fill(&e, msg, w, l);
    if (!e.m.hwnd) return;
    TI_TRACE("%s w=0x%x l=0x%x%s", sp_message_name(msg), w, (uint32_t)l, T.in_key ? " (after the key-down)" : "");
    if (T.in_key) {
        if (T.k_npending < TI_PENDING) T.k_pending[T.k_npending++] = e;
        T.k_ime = 1;
    } else {
        sp__enqueue_input(&e);
    }
}

static void ti_chars(const uint16_t *s, int n)    /* WM_CHAR per UTF-16 unit, not tied to a key-down */
{
    for (int i = 0; i < n; i++) {
        sp_qent e;
        ti_fill(&e, WM_CHAR, s[i], 1);
        if (e.m.hwnd) sp__enqueue_input(&e);
    }
}

/* ---- composition ----------------------------------------------------------------------------------- */
static void ti_cancel(void)
{
    T.comp_len = 0; T.sel_loc = T.sel_len = 0;
    if (T.started) {
        T.started = 0;
        ti_msg(WM_IME_COMPOSITION, 0, 0);          /* the composition string is gone */
        ti_msg(WM_IME_ENDCOMPOSITION, 0, 0);
    }
}

void sp_ti_set_marked(const uint16_t *s, int n, int sel_loc, int sel_len)
{
    ti_init();
    if (T.discarding) return;
    if (T.in_key) T.k_ops++;
    if (n <= 0 || !s) { ti_cancel(); return; }
    if (n > TI_MAX) n = TI_MAX;
    if (!T.comp_len && !T.started) T.comp_kind = ti_kind();
    memcpy(T.comp, s, (size_t)n * 2);
    T.comp_len = n;
    if (sel_loc < 0 || sel_loc > n) sel_loc = n;
    if (sel_len < 0 || sel_loc + sel_len > n) sel_len = 0;
    T.sel_loc = sel_loc; T.sel_len = sel_len;
    /* ATTR_INPUT 0 while nothing is converted; the IM's selected clause is ATTR_TARGET_CONVERTED 1 and
     * the rest ATTR_CONVERTED 2 once it shows a selection. */
    for (int i = 0; i < n; i++) T.attr[i] = sel_len ? ((i >= sel_loc && i < sel_loc + sel_len) ? 1 : 2) : 0;
    if (T.comp_kind != SP_TI_IM) { TI_TRACE("dead key, %d unit(s) marked", n); return; }
    if (!T.started) { T.started = 1; ti_msg(WM_IME_STARTCOMPOSITION, 0, 0); }
    ti_msg(WM_IME_COMPOSITION, s[n - 1], SP_GCS_COMPSTR | SP_GCS_COMPATTR | SP_GCS_COMPCLAUSE | SP_GCS_CURSORPOS |
                                         SP_GCS_DELTASTART);
}

void sp_ti_insert(const uint16_t *s, int n)
{
    ti_init();
    if (T.discarding) return;
    if (T.in_key) T.k_ops++;
    if (n > TI_MAX) n = TI_MAX;
    if (T.started) {
        if (n <= 0 || !s) { ti_cancel(); return; }
        memcpy(T.result, s, (size_t)n * 2);
        T.result_len = n;
        T.comp_len = 0; T.sel_loc = T.sel_len = 0;
        T.started = 0;
        ti_msg(WM_IME_COMPOSITION, s[0], SP_GCS_RESULTSTR | SP_GCS_RESULTCLAUSE);
        ti_msg(WM_IME_ENDCOMPOSITION, 0, 0);
        return;
    }
    T.comp_len = 0; T.sel_loc = T.sel_len = 0;      /* a dead key is consumed by the text it produced */
    if (n <= 0 || !s) return;
    if (T.in_key) {
        for (int i = 0; i < n && T.k_ins_len < TI_MAX; i++) T.k_ins[T.k_ins_len++] = s[i];
    } else {
        TI_TRACE("%d unit(s) inserted outside a key", n);
        ti_chars(s, n);
    }
}

void sp_ti_unmark(void)
{
    ti_init();
    if (T.discarding || !T.comp_len) return;
    uint16_t tmp[TI_MAX];
    int n = T.comp_len;
    memcpy(tmp, T.comp, (size_t)n * 2);
    sp_ti_insert(tmp, n);
}

int sp_ti_marked(uint16_t *out, int max, int *sel_loc, int *sel_len)
{
    if (out && max > 0) memcpy(out, T.comp, (size_t)(T.comp_len < max ? T.comp_len : max) * 2);
    if (sel_loc) *sel_loc = T.sel_loc;
    if (sel_len) *sel_len = T.sel_len;
    return T.comp_len;
}

static void ti_discard_view(void)
{
    if (!T.discard) return;
    T.discarding = 1;
    T.discard();
    T.discarding = 0;
}

/* ---- keys ------------------------------------------------------------------------------------------ */
static int ti_special(uint16_t kvk)       /* keys the game handles itself: never offered while not composing */
{
    switch (kvk) {
    case 0x24: case 0x4C: case 0x30: case 0x33: case 0x75: case 0x35:                 /* Return Enter Tab BS Del Esc */
    case 0x7B: case 0x7C: case 0x7D: case 0x7E:                                       /* arrows */
    case 0x73: case 0x77: case 0x74: case 0x79: case 0x72: case 0x47:                 /* Home End PgUp PgDn Help Clear */
    case 0x7A: case 0x78: case 0x63: case 0x76: case 0x60: case 0x61: case 0x62:       /* F1-F7 */
    case 0x64: case 0x65: case 0x6D: case 0x67: case 0x6F: case 0x69: case 0x6B:       /* F8-F14 */
    case 0x71: case 0x6A: case 0x40: case 0x4F: case 0x50: case 0x5A:                 /* F15-F20 */
        return 1;
    default: return 0;
    }
}

int sp_ti_wants_key(uint16_t kvk, int cmd_or_ctrl)
{
    ti_init();
    if (!T.enabled || cmd_or_ctrl) return 0;
    if (kvk == 0x68) T.open = 1;                   /* the JIS Kana key turns the input method back on */
    if (T.comp_len || T.started) {
        if (T.comp_kind != SP_TI_IM && !T.started && ti_special(kvk)) {
            T.comp_len = 0;                        /* a pending dead key is dropped by the game's own keys */
            ti_discard_view();
            return 0;
        }
        return 1;
    }
    if (ti_special(kvk)) return 0;
    if (ti_kind() == SP_TI_IM && !T.open) return 0;  /* the game closed the IME: plain keys */
    return 1;
}

void sp_ti_key_begin(void)
{
    ti_init();
    T.in_key = 1;
    T.k_ops = T.k_ime = 0;
    T.k_ins_len = 0;
    T.k_npending = 0;
    T.k_had_marked = T.comp_len > 0 || T.started;
}

static void ti_processkey(uint16_t kvk, int action)
{
    sp_keyinfo k;
    sp_qent e;
    int known = sp_keymap_lookup(kvk, sp__cfg(), &k);
    int prev = action == SP_KEY_REPEAT || (known && (sp__async[k.vk] & 0x80));
    ti_fill(&e, WM_KEYDOWN, VK_PROCESSKEY,
            (int32_t)(1u | ((uint32_t)(known ? k.scan : 0) << 16) | ((uint32_t)(known ? k.ext : 0) << 24) |
                      (prev ? 1u << 30 : 0)));
    if (known && k.vk_specific == k.vk) {
        e.key_vk = k.vk; e.key_vk_specific = k.vk_specific;
        e.key_change = prev ? 0 : +1;
        if (!prev) sp__keystate_apply(sp__async, k.vk, k.vk_specific, 1);
    }
    if (e.m.hwnd) sp__enqueue_input(&e);
}

int sp_ti_key_end(uint16_t kvk, int action, const uint16_t *chars, int n)
{
    T.in_key = 0;
    if (T.k_ime || T.started || (T.k_had_marked && T.comp_kind == SP_TI_IM)) {
        /* the input method took the key: VK_PROCESSKEY, then what it generated */
        ti_processkey(kvk, action);
        for (int i = 0; i < T.k_npending; i++) sp__enqueue_input(&T.k_pending[i]);
        T.k_npending = 0;
        if (T.k_ins_len) ti_chars(T.k_ins, T.k_ins_len);
        TI_TRACE("key 0x%02x -> VK_PROCESSKEY (input method)", kvk);
        return 1;
    }
    if (!T.k_ops) return 0;                        /* nothing inserted or marked: today's path */
    if (!T.k_had_marked && !T.comp_len && T.k_ins_len == n && n > 0 &&
        !memcmp(T.k_ins, chars, (size_t)n * 2))
        return 0;                                  /* exactly NSEvent.characters: today's path, byte for byte */
    /* a dead key (no character now) or the text it composed (é), or other inserted text */
    int first = T.k_ins_len < 4 ? T.k_ins_len : 4;
    sp_input_key(kvk, action, T.k_ins, first, T.k_ins, first);
    if (T.k_ins_len > first) ti_chars(T.k_ins + first, T.k_ins_len - first);
    TI_TRACE("key 0x%02x -> %d unit(s)%s", kvk, T.k_ins_len, T.comp_len ? ", dead key pending" : "");
    return 1;
}

/* ---- IMM32 ----------------------------------------------------------------------------------------- */
uint32_t sp_ti_context(sp_hwnd h)
{
    ti_init();
    if (!T.enabled || !h || !sp_is_window(h)) return 0;
    return 0x494D0000u | (h & 0xFFFFu);            /* "IM" + the window: one pseudo HIMC per window */
}

int sp_ti_open_status(void) { ti_init(); return T.enabled && ti_kind() == SP_TI_IM && T.open; }

int sp_ti_set_open(int open)
{
    ti_init();
    T.open = open ? 1 : 0;
    TI_TRACE("ImmSetOpenStatus(%d)", T.open);
    if (!T.open && (T.started || T.comp_len)) { ti_discard_view(); ti_cancel(); }
    return 1;
}

uint32_t sp_ti_conversion(void) { return sp_ti_open_status() ? 1u /* IME_CMODE_NATIVE */ : 0u; }

int sp_ti_notify(uint32_t action, uint32_t index, uint32_t value)
{
    ti_init();
    (void)value;
    if (action != SP_NI_COMPOSITIONSTR) return 0;  /* candidate-window requests: the system IM owns its window */
    if (!T.comp_len && !T.started) return 0;
    if (index == SP_CPS_COMPLETE) {
        uint16_t tmp[TI_MAX];
        int n = T.comp_len;
        memcpy(tmp, T.comp, (size_t)n * 2);
        ti_discard_view();
        if (T.started) {
            if (n) sp_ti_insert(tmp, n); else ti_cancel();
        } else {
            T.comp_len = 0;
            ti_chars(tmp, n);                      /* a dead key completes as itself */
        }
        TI_TRACE("ImmNotifyIME(CPS_COMPLETE): %d unit(s)", n);
        return 1;
    }
    if (index == SP_CPS_CANCEL) {
        ti_discard_view();
        ti_cancel();
        TI_TRACE("ImmNotifyIME(CPS_CANCEL)");
        return 1;
    }
    return 0;                                      /* CPS_CONVERT / CPS_REVERT: not offered by macOS IMs */
}

static int32_t ti_copy(const void *src, uint32_t need, void *buf, uint32_t bytes)
{
    if (!bytes) return (int32_t)need;
    if (bytes > need) bytes = need;
    if (buf && bytes) memcpy(buf, src, bytes);
    return (int32_t)bytes;
}

int32_t sp_ti_comp_string(uint32_t index, void *buf, uint32_t bytes)
{
    ti_init();
    int comp = T.started ? T.comp_len : 0;         /* a dead key is not an IME composition */
    uint32_t clause[2];
    switch (index) {
    case SP_GCS_COMPSTR: return ti_copy(T.comp, (uint32_t)comp * 2, buf, bytes & ~1u);
    case SP_GCS_COMPATTR: return ti_copy(T.attr, (uint32_t)comp, buf, bytes);
    case SP_GCS_COMPCLAUSE:
        if (!comp) return 0;
        clause[0] = 0; clause[1] = (uint32_t)comp;
        return ti_copy(clause, 8, buf, bytes & ~3u);
    case SP_GCS_CURSORPOS: return comp ? T.sel_loc : 0;
    case SP_GCS_DELTASTART: return 0;
    case SP_GCS_RESULTSTR: return ti_copy(T.result, (uint32_t)T.result_len * 2, buf, bytes & ~1u);
    case SP_GCS_RESULTCLAUSE:
        if (!T.result_len) return 0;
        clause[0] = 0; clause[1] = (uint32_t)T.result_len;
        return ti_copy(clause, 8, buf, bytes & ~3u);
    case 0x0001: case 0x0002: case 0x0004: case 0x0200: case 0x0400:
        return 0;                                  /* reading strings: macOS input methods do not expose them */
    default:
        return -2;                                 /* IMM_ERROR_GENERAL */
    }
}

/* DefWindowProc's part (Microsoft Learn, WM_IME_COMPOSITION and WM_IME_CHAR): an unhandled result string
 * becomes WM_IME_CHAR per character, and an unhandled WM_IME_CHAR becomes WM_CHAR. Returns 1 when it
 * handled the message. Reached from sp_def_window_proc. */
int sp_ti_def_window_proc(sp_hwnd h, uint32_t msg, uint32_t w, int32_t l, int32_t *result)
{
    if (msg == WM_IME_COMPOSITION) {
        if (l & SP_GCS_RESULTSTR) {
            uint16_t tmp[TI_MAX];
            int n = T.result_len;
            memcpy(tmp, T.result, (size_t)n * 2);
            for (int i = 0; i < n; i++) sp_send_message(h, WM_IME_CHAR, tmp[i], 1);
        }
        *result = 0;
        return 1;
    }
    if (msg == WM_IME_CHAR) {
        sp_post_message(h, WM_CHAR, w, l);
        *result = 0;
        return 1;
    }
    return 0;
}
