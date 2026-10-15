/*
 * text_test.m - text-input cases for sphost (included by hostapp.m; our own code only).
 *
 *   sphost --text-test
 *
 * A small window appears for about a second. Case 1 types Option-e then e as real NSEvents through the
 * normal drain, the view's interpretKeyEvents: and the system's input context (needs a keyboard layout
 * with the Option-e dead key: U.S., ABC, British ...; another layout reports the case as skipped). The other
 * cases call the view's NSTextInputClient methods directly, as an input method does, and check the
 * WM_IME_* / WM_CHAR sequence and the strings the IMM32 imports would return. The user's input source is
 * never changed.
 */
#include "../src/sp_textinput.inc"   /* the text-input core (BUILD NOTE in the file) */

static int g_tt_pass, g_tt_fail;
#define TCHECK(c, ...) do { if (c) g_tt_pass++; else { g_tt_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

typedef struct { uint32_t msg, w; int32_t l; } tt_rec;
static tt_rec g_tt[256];
static int g_ttn;
static int32_t tt_proc(void *user, sp_hwnd h, uint32_t msg, uint32_t w, int32_t l)
{
    if (msg != WM_PAINT && msg != WM_ERASEBKGND && msg != WM_SETCURSOR && msg != WM_NCHITTEST && msg != WM_TIMER &&
        g_ttn < 256)
        g_tt[g_ttn++] = (tt_rec){msg, w, l};
    if (msg == WM_PAINT) {
        sp_paintstruct ps;
        sp_begin_paint(h, &ps);
        sp_mac_present_clear(h, 0.3f, 0.3f, 0.35f);
        sp_end_paint(h, &ps);
        return 0;
    }
    return sp_def_window_proc(h, msg, w, l);
}
static void tt_pump(int passes)
{
    for (int i = 0; i < passes; i++) {
        sp_invalidate_rect(g_h, 0);
        sp_msg m;
        while (sp_peek_message(&m, 0, 0, 0, PM_REMOVE)) { sp_translate_message(&m); sp_dispatch_message(&m); }
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
    }
}
static int tt_find(uint32_t msg, uint32_t w, int from)
{
    for (int i = from; i < g_ttn; i++) if (g_tt[i].msg == msg && (w == 0xFFFFFFFFu || g_tt[i].w == w)) return i;
    return -1;
}
static int tt_count(uint32_t msg)
{
    int c = 0;
    for (int i = 0; i < g_ttn; i++) c += g_tt[i].msg == msg;
    return c;
}
static void tt_dump(const char *what)
{
    fprintf(stderr, "  %s:", what);
    for (int i = 0; i < g_ttn; i++) fprintf(stderr, " %s(0x%x)", sp_message_name(g_tt[i].msg), g_tt[i].w);
    fprintf(stderr, "\n");
}
/* Key events made through CGEvent carry the machine's keyboard type (91 here), which the input context's
 * dead-key composition needs: with an NSEvent from keyEventWithType: (keyboard type 0) Option-e e gave
 * U+00B4 then 'e' (observed in a test app). Posted as they are; route() takes a synthetic event
 * without a window number to the test window. */
static void tt_cgkey(int down, uint16_t kvk, NSEventModifierFlags f)
{
    CGEventRef ce = CGEventCreateKeyboardEvent(NULL, kvk, down ? true : false);
    CGEventSetFlags(ce, (CGEventFlags)f);
    CGEventSetTimestamp(ce, (CGEventTimestamp)(next_ts() * 1e9));
    post([NSEvent eventWithCGEvent:ce]);
    CFRelease(ce);
}
static void tt_flags(uint16_t kvk, NSEventModifierFlags f)
{
    post([NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:f timestamp:next_ts()
                      windowNumber:sp_mac_window_number(g_h) context:nil characters:@"" charactersIgnoringModifiers:@""
                         isARepeat:NO keyCode:kvk]);
}

static int text_test(void)
{
    if (sp_mac_app_init(0)) { fprintf(stderr, "Metal device unavailable\n"); return 1; }
    sp_mac_set_test_mode(1);
    sp_mac_set_text_test(1);
    g_h = sp_mac_create_window("text test (no game code)", 260, 260, 480, 200, tt_proc, NULL, SP_STYLE_DBLCLKS);
    NSWindow *w = nswin();
    if (getenv("SPHOST_KEY")) {
        [NSApp activateIgnoringOtherApps:YES];
        [w makeKeyAndOrderFront:nil];
    } else {
        [w orderFront:nil];
    }
    sp_show_window(g_h, 1, 0);
    sp_set_focus(g_h);
    tt_pump(3);
    NSView<NSTextInputClient> *v = (NSView<NSTextInputClient> *)w.contentView;
    TCHECK([v conformsToProtocol:@protocol(NSTextInputClient)], "the game view is an NSTextInputClient");
    TCHECK(w.firstResponder == v, "the view is the window's first responder");
    NSString *src = v.inputContext.selectedKeyboardInputSource;
    printf("text test: input source %s\n", src.UTF8String ?: "(none)");

    /* 1. plain typing through the input context is today's sequence: WM_KEYDOWN 'A', WM_CHAR 'a', WM_KEYUP */
    g_ttn = 0;
    tt_cgkey(1, 0x00, 0);
    tt_cgkey(0, 0x00, 0);
    tt_pump(3);
    int k = tt_find(WM_KEYDOWN, 'A', 0), c = tt_find(WM_CHAR, 0xFFFFFFFFu, 0);
    int us = src && ([src hasSuffix:@".US"] || [src hasSuffix:@".ABC"] || [src hasSuffix:@".British"] ||
                     [src hasSuffix:@".USExtended"] || [src hasSuffix:@".Australian"] || [src hasSuffix:@".Canadian"]);
    if (us) {
        TCHECK(g_ttn == 3 && k == 0 && c == 1 && g_tt[1].w == 'a' && g_tt[1].l == g_tt[0].l && g_tt[2].msg == WM_KEYUP,
               "'a' through the input context: KEYDOWN CHAR('a') KEYUP");
        if (g_tt_fail) tt_dump("a");
    }

    /* 2. Option-e e: the dead key's WM_SYSKEYDOWN carries no character, then e gives WM_CHAR 0xE9 */
    if (us) {
        NSEventModifierFlags opt = NSEventModifierFlagOption | 0x20;
        g_ttn = 0;
        tt_flags(0x3A, opt);
        tt_cgkey(1, 0x0E, opt);
        tt_cgkey(0, 0x0E, opt);
        tt_flags(0x3A, 0);
        tt_pump(3);
        int sd = tt_find(WM_SYSKEYDOWN, 'E', 0);
        TCHECK(sd >= 0 && !tt_count(WM_SYSCHAR) && !tt_count(WM_CHAR) && !tt_count(0x10D) && !tt_count(0x10F),
               "Option-e: WM_SYSKEYDOWN 'E', no character, no WM_IME_*");
        TCHECK(v.hasMarkedText && v.markedRange.length == 1, "the dead key is marked text (%lu)",
               (unsigned long)v.markedRange.length);
        int f1 = g_tt_fail;
        if (g_tt_fail) tt_dump("Option-e");
        g_ttn = 0;
        tt_cgkey(1, 0x0E, 0);
        tt_cgkey(0, 0x0E, 0);
        tt_pump(3);
        int kd = tt_find(WM_KEYDOWN, 'E', 0), ch = tt_find(WM_CHAR, 0xE9, 0);
        TCHECK(kd >= 0 && ch == kd + 1 && tt_count(WM_CHAR) == 1 && g_tt[ch].l == g_tt[kd].l,
               "then e: WM_KEYDOWN 'E', WM_CHAR 0xE9");
        TCHECK(!v.hasMarkedText, "nothing marked after the commit");
        if (g_tt_fail > f1) tt_dump("e");

        /* Option-x is not a dead key: the input context inserts what NSEvent.characters said, today's path */
        g_ttn = 0;
        tt_flags(0x3A, opt);
        tt_cgkey(1, 0x07, opt);
        tt_cgkey(0, 0x07, opt);
        tt_flags(0x3A, 0);
        tt_pump(3);
        int sc = tt_find(WM_SYSCHAR, 'x', 0);
        TCHECK(sc >= 0 && tt_count(WM_SYSCHAR) == 1 && !tt_count(WM_CHAR), "Option-x: WM_SYSCHAR 'x' (unchanged by text input)");
        if (sc < 0) tt_dump("Option-x");

        /* a dead key then a key the game handles itself (an arrow): the dead key is dropped, the arrow is delivered unchanged */
        g_ttn = 0;
        tt_flags(0x3A, opt);
        tt_cgkey(1, 0x0E, opt);
        tt_cgkey(0, 0x0E, opt);
        tt_flags(0x3A, 0);
        tt_cgkey(1, 0x7B, NSEventModifierFlagFunction | NSEventModifierFlagNumericPad);
        tt_cgkey(0, 0x7B, NSEventModifierFlagFunction | NSEventModifierFlagNumericPad);
        tt_pump(3);
        TCHECK(tt_find(WM_KEYDOWN, 0x25, 0) >= 0 && !tt_count(WM_CHAR) && !v.hasMarkedText,
               "dead key + Left: VK_LEFT, no character, nothing marked");
    } else {
        printf("text test: Option-e cases skipped (input source %s has no Option-e dead key we know)\n",
               src.UTF8String ?: "?");
    }

    /* 3. an input method's marked text through the view's NSTextInputClient methods */
    sp_ti_force_kind(SP_TI_IM);
    g_ttn = 0;
    [v setMarkedText:@"k" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    [v setMarkedText:[[NSAttributedString alloc] initWithString:@"か"] selectedRange:NSMakeRange(1, 0)
        replacementRange:NSMakeRange(NSNotFound, 0)];
    TCHECK(v.hasMarkedText && v.markedRange.location == 0 && v.markedRange.length == 1, "marked range (0,1)");
    TCHECK([[v attributedSubstringForProposedRange:NSMakeRange(0, 1) actualRange:NULL].string isEqualToString:@"か"],
           "attributedSubstring of the marked text");
    tt_pump(2);
    uint16_t cs[8];
    TCHECK(sp_ti_comp_string(SP_GCS_COMPSTR, NULL, 0) == 2 && sp_ti_comp_string(SP_GCS_COMPSTR, cs, 16) == 2 &&
           cs[0] == 0x304B, "ImmGetCompositionStringW(GCS_COMPSTR) = か");
    [v insertText:@"か" replacementRange:NSMakeRange(NSNotFound, 0)];
    tt_pump(3);
    int s0 = tt_find(0x10D, 0xFFFFFFFFu, 0), c1 = tt_find(0x10F, 'k', 0), c2 = tt_find(0x10F, 0x304B, 0);
    int r = c2 >= 0 ? tt_find(0x10F, 0x304B, c2 + 1) : -1, e = tt_find(0x10E, 0xFFFFFFFFu, 0);
    TCHECK(s0 == 0 && c1 == 1 && c2 == 2 && g_tt[c2].l == 0x1B8 && r > c2 && g_tt[r].l == 0x1800 && e > r &&
           tt_count(0x10D) == 1, "START, COMPOSITION(k), COMPOSITION(か), COMPOSITION(GCS_RESULTSTR), END");
    TCHECK(sp_ti_comp_string(SP_GCS_RESULTSTR, cs, 16) == 2 && cs[0] == 0x304B, "GCS_RESULTSTR = か");
    TCHECK(!v.hasMarkedText, "nothing marked after insertText");
    int ime_char = tt_find(0x286, 0x304B, 0), wmc = tt_find(WM_CHAR, 0x304B, 0);
    if (ime_char >= 0)
        TCHECK(wmc > ime_char, "DefWindowProc: WM_IME_CHAR then WM_CHAR 0x304B");
    printf("text test: DefWindowProc IME hook %s\n", ime_char >= 0 ? "present (WM_IME_CHAR posted)" : "absent (no WM_IME_CHAR)");
    if (s0 != 0 || e < 0) tt_dump("IM");

    /* 4. ImmNotifyIME(CPS_COMPLETE) while composing: one result, the IM's marked text discarded, no echo */
    g_ttn = 0;
    [v setMarkedText:@"にほん" selectedRange:NSMakeRange(3, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    TCHECK(sp_ti_notify(SP_NI_COMPOSITIONSTR, SP_CPS_COMPLETE, 0) == 1, "CPS_COMPLETE");
    tt_pump(3);
    TCHECK(tt_count(0x10D) == 1 && tt_count(0x10E) == 1 && tt_find(0x10F, 0x306B, 0) >= 0 && !v.hasMarkedText,
           "complete: one START, one END, result にほん");
    /* 5. the game closes the IME while composing: cancelled */
    g_ttn = 0;
    [v setMarkedText:@"x" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    sp_ti_set_open(0);
    tt_pump(3);
    int cz = tt_find(0x10F, 0, 1);
    TCHECK(cz >= 0 && g_tt[cz].l == 0 && tt_find(0x10E, 0xFFFFFFFFu, cz) > cz && !v.hasMarkedText,
           "ImmSetOpenStatus(FALSE): COMPOSITION(0,0), END, nothing marked");
    sp_ti_set_open(1);
    sp_ti_force_kind(-1);

    sp_destroy_window(g_h);
    tt_pump(1);
    printf("text test: %d passed, %d failed\n", g_tt_pass, g_tt_fail);
    return g_tt_fail ? 1 : 0;
}
