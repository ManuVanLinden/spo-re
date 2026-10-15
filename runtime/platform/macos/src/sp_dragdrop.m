/*
 * sp_dragdrop.m - files dragged from the Finder onto the game view (declared in include/sp_macos.h).
 *
 * An NSDraggingDestination category on SPView (sp_window.m's content view; nothing there is
 * edited). sp_mac_dnd_attach(h, fn, user) registers the view of window h for NSPasteboardTypeFileURL, at once
 * when its NSWindow exists, otherwise when the window first becomes key, main or visible (the game registers
 * its drop target right after CreateWindowEx, before the first ShowWindow attaches the NSWindow; a run-loop
 * observer retries on every pass of the event drain until then).
 *
 * Each AppKit callback becomes one queued sp_dnd_event (ENTER with the .png files and the client point, OVER,
 * LEAVE, DROP with the files again), after which fn is called; runtime/platform/guest's sgp_dragdrop.c drains
 * the queue from there on the game thread and calls the game's IDropTarget. AppKit calls these methods on the
 * main thread from inside the event drain behind PeekMessage, so fn runs inside a shim of the game thread, as
 * window procedures do. A drag whose files include no .png is refused (NSDragOperationNone) and queues nothing.
 * The operation shown to the Finder is always Copy (the game answers DROPEFFECT_MOVE on Drop, which must never
 * make the Finder move the player's file), or None when the game refused.
 *
 * Client points use the same mapping as mouse events (sp_window.m view_to_client): the content rectangle
 * (letterbox in fullscreen) from sp_mac_content_rect and the client size the game sees.
 */
#import <AppKit/AppKit.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sp_macos.h"

@interface SPView : NSView
- (sp_hwnd)hwnd;                      /* sp_window.m's property */
@end
@interface SPView (SPDragDrop) <NSDraggingDestination>
@end

#define DND_Q 64
static sp_dnd_event g_q[DND_Q];
static unsigned g_qhead, g_qtail;
static sp_dnd_notify_fn g_fn;
static void *g_user;
static sp_hwnd g_pending[4];
static id g_observers[3];
static CFRunLoopObserverRef g_rlo;     /* retries while a registration is pending (every pass of the drain) */
static struct { int active; sp_hwnd h; int accepted; } g_drag;   /* the drag over our view, if any */

/* ---- the queue (main thread) ------------------------------------------------------------------------ */
static void free_files(char **f, int n)
{
    for (int i = 0; i < n; i++) free(f[i]);
    free(f);
}

static void push(const sp_dnd_event *e)
{
    if (g_qtail - g_qhead >= DND_Q) {            /* nobody drains (no guest): drop the oldest */
        sp_dnd_event *o = &g_q[g_qhead++ % DND_Q];
        free_files(o->files, o->nfiles);
    }
    g_q[g_qtail++ % DND_Q] = *e;
}

int sp_mac_dnd_poll(sp_dnd_event *out)
{
    if (g_qhead == g_qtail) return 0;
    *out = g_q[g_qhead++ % DND_Q];
    return 1;
}

void sp_mac_dnd_event_free(sp_dnd_event *e)
{
    if (!e) return;
    free_files(e->files, e->nfiles);
    e->files = NULL;
    e->nfiles = 0;
}

/* ---- registration ------------------------------------------------------------------------------------- */
static SPView *view_of(sp_hwnd h)
{
    Class c = [SPView class];
    for (NSWindow *w in NSApp.windows) {
        NSView *v = w.contentView;
        if ([v isKindOfClass:c] && [(SPView *)v hwnd] == h) return (SPView *)v;
    }
    return nil;
}

static int register_view(sp_hwnd h)
{
    SPView *v = view_of(h);
    if (!v) return 0;
    if (![v.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL])
        [v registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return 1;
}

static void stop_observing(void)
{
    for (int i = 0; i < 3; i++)
        if (g_observers[i]) { [[NSNotificationCenter defaultCenter] removeObserver:g_observers[i]]; g_observers[i] = nil; }
    if (g_rlo) {
        CFRunLoopObserverInvalidate(g_rlo);
        CFRelease(g_rlo);
        g_rlo = NULL;
    }
}

static void retry_pending(void)
{
    int left = 0;
    for (int i = 0; i < 4; i++)
        if (g_pending[i]) {
            if (register_view(g_pending[i])) g_pending[i] = 0;
            else left++;
        }
    if (!left) stop_observing();
}

int sp_mac_dnd_attach(sp_hwnd h, sp_dnd_notify_fn fn, void *user)
{
    if (!NSApp) return 0;
    g_fn = fn;
    g_user = user;
    @autoreleasepool {
        if (register_view(h)) return 1;
        for (int i = 0; i < 4; i++)
            if (!g_pending[i] || g_pending[i] == h) { g_pending[i] = h; break; }
        if (!g_observers[0]) {
            NSNotificationName names[3] = {NSWindowDidBecomeKeyNotification, NSWindowDidBecomeMainNotification,
                                           NSWindowDidChangeOcclusionStateNotification};
            for (int i = 0; i < 3; i++)
                g_observers[i] = [[NSNotificationCenter defaultCenter] addObserverForName:names[i] object:nil queue:nil
                                                                               usingBlock:^(NSNotification *n) { retry_pending(); }];
            /* nextEventMatchingMask runs the main run loop in the default mode on every drain; the observer
             * catches a window that appears without becoming key, main or visible-notified */
            g_rlo = CFRunLoopObserverCreateWithHandler(NULL, kCFRunLoopBeforeSources | kCFRunLoopBeforeWaiting, true, 0,
                                                       ^(CFRunLoopObserverRef o, CFRunLoopActivity a) { retry_pending(); });
            if (g_rlo) CFRunLoopAddObserver(CFRunLoopGetMain(), g_rlo, kCFRunLoopCommonModes);
        }
    }
    return 2;
}

void sp_mac_dnd_detach(sp_hwnd h)
{
    @autoreleasepool {
        SPView *v = view_of(h);
        if (v) [v unregisterDraggedTypes];
        for (int i = 0; i < 4; i++) if (g_pending[i] == h) g_pending[i] = 0;
        if (g_drag.h == h) g_drag.active = 0;
    }
}

/* ---- what is dragged ---------------------------------------------------------------------------------- */
static int is_png(NSURL *u)
{
    return u.isFileURL && [u.pathExtension caseInsensitiveCompare:@"png"] == NSOrderedSame;
}

static char **png_files(id<NSDraggingInfo> info, int *n)
{
    NSArray<NSURL *> *urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                                     options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    char **out = NULL;
    int k = 0;
    *n = 0;
    if (!urls.count) return NULL;
    out = calloc(urls.count, sizeof *out);
    if (!out) return NULL;
    for (NSURL *u in urls) {
        const char *p;
        if (!is_png(u) || !(p = u.fileSystemRepresentation)) continue;
        out[k++] = strdup(p);
    }
    if (!k) { free(out); return NULL; }
    *n = k;
    return out;
}

static uint32_t key_state(int button_down)
{
    NSEventModifierFlags m = NSEvent.modifierFlags;
    return (button_down ? 1u : 0u) | ((m & NSEventModifierFlagShift) ? 4u : 0u) |
           ((m & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) ? 8u : 0u) |
           ((m & NSEventModifierFlagOption) ? 0x20u : 0u);
}

/* The client point (guest units) of a dragging location, as mouse events map it. */
static void client_point(SPView *v, sp_hwnd h, NSPoint window_pt, int32_t *x, int32_t *y)
{
    NSPoint p = [v convertPoint:window_pt fromView:nil];
    double r[4];
    int32_t cx = 0, cy = 0, lw = 0, lh = 0;
    sp_mac_content_rect(h, r);
    sp_get_last_client(h, &cx, &cy, &lw, &lh);
    double vx = p.x - r[0], vy = v.bounds.size.height - p.y - r[1];
    double sx = r[2] > 0 && lw > 0 ? lw / r[2] : 1.0, sy = r[3] > 0 && lh > 0 ? lh / r[3] : 1.0;
    *x = (int32_t)floor(sx == 1.0 ? vx : vx * sx);
    *y = (int32_t)floor(sy == 1.0 ? vy : vy * sy);
}

static NSDragOperation answer(uint32_t effect, int fallback)
{
    if (effect == ~0u) return fallback ? NSDragOperationCopy : NSDragOperationNone;
    return effect ? NSDragOperationCopy : NSDragOperationNone;
}

static uint32_t notify(void)
{
    return g_fn ? g_fn(g_user) : ~0u;
}

@implementation SPView (SPDragDrop)
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    sp_dnd_event e = {SP_DND_ENTER, self.hwnd, 0, 0, key_state(1), 0, NULL};
    if (g_drag.active && g_drag.accepted) {            /* an exit we never saw */
        sp_dnd_event l = {SP_DND_LEAVE, g_drag.h, 0, 0, 0, 0, NULL};
        push(&l);
    }
    g_drag.active = 1, g_drag.h = self.hwnd, g_drag.accepted = 0;
    if (!(info.draggingSourceOperationMask & NSDragOperationCopy)) return NSDragOperationNone;
    e.files = png_files(info, &e.nfiles);
    if (!e.nfiles) return NSDragOperationNone;         /* only .png files are offered */
    client_point(self, e.hwnd, info.draggingLocation, &e.x, &e.y);
    push(&e);
    NSDragOperation op = answer(notify(), 1);
    g_drag.accepted = 1;
    return op;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info
{
    if (!g_drag.active || !g_drag.accepted) return NSDragOperationNone;
    sp_dnd_event e = {SP_DND_OVER, self.hwnd, 0, 0, key_state(1), 0, NULL};
    client_point(self, e.hwnd, info.draggingLocation, &e.x, &e.y);
    push(&e);
    return answer(notify(), 1);
}
- (void)draggingExited:(id<NSDraggingInfo>)info
{
    if (g_drag.active && g_drag.accepted) {
        sp_dnd_event e = {SP_DND_LEAVE, self.hwnd, 0, 0, 0, 0, NULL};
        push(&e);
        notify();
    }
    g_drag.active = 0;
}
- (BOOL)wantsPeriodicDraggingUpdates { return NO; }
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info { return g_drag.active && g_drag.accepted; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    if (!g_drag.active || !g_drag.accepted) return NO;
    sp_dnd_event e = {SP_DND_DROP, self.hwnd, 0, 0, key_state(0), 0, NULL};
    e.files = png_files(info, &e.nfiles);
    client_point(self, e.hwnd, info.draggingLocation, &e.x, &e.y);
    push(&e);
    g_drag.active = 0;
    uint32_t eff = notify();
    return eff == ~0u || eff != 0;
}
- (void)concludeDragOperation:(id<NSDraggingInfo>)info { g_drag.active = 0; }
- (void)draggingEnded:(id<NSDraggingInfo>)info { g_drag.active = 0; }
@end
