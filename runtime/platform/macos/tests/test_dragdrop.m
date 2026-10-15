/*
 * test_dragdrop.m - drag and drop host case: the NSDraggingDestination category of sp_dragdrop.m, driven with a fake
 * NSDraggingInfo (a small class answering draggingPasteboard with file URLs on a private pasteboard), against
 * a real SPView in a real (small, not fullscreen) window that shows for about a second. Checks registration
 * (pending until the NSWindow exists), the queued events (kind, client point, the .png files only), the
 * operations returned to AppKit and the notify calls. No game code.
 *
 *   test_dragdrop [SANDBOX_ROOT | card.png card.png]   (the Makefile passes build/harness/sandbox, whose
 *       x/Documents/My Spore Creations/y/z.png are the game's cards; without any, two empty .png files in a
 *       temporary folder are used)
 */
#import <AppKit/AppKit.h>

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sp_macos.h"

static int t_pass, t_fail;
#define CHECK(c, ...) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

@interface SPView : NSView
- (sp_hwnd)hwnd;
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info;
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info;
- (void)draggingExited:(id<NSDraggingInfo>)info;
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info;
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info;
- (void)concludeDragOperation:(id<NSDraggingInfo>)info;
@end

@interface FakeDrag : NSObject
@property(nonatomic, strong) NSPasteboard *pb;
@property(nonatomic) NSPoint loc;
@property(nonatomic) NSDragOperation mask;
@end
@implementation FakeDrag
- (NSPasteboard *)draggingPasteboard { return _pb; }
- (NSPoint)draggingLocation { return _loc; }
- (NSDragOperation)draggingSourceOperationMask { return _mask; }
- (NSInteger)draggingSequenceNumber { return 42; }
- (id)draggingSource { return nil; }
- (NSWindow *)draggingDestinationWindow { return nil; }
@end

static int n_notify;
static uint32_t notify_answer = ~0u;
static uint32_t on_notify(void *user)
{
    n_notify++;
    CHECK(user == (void *)0x4242, "notify user");
    return notify_answer;
}
static int32_t proc(void *u, sp_hwnd h, uint32_t m, uint32_t w, int32_t l) { return 0; }

static int take(sp_dnd_event *e, int kind) { return sp_mac_dnd_poll(e) && e->kind == kind; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        char a[1024], b[1024];
        int have = 0;
        if (argc == 2) {
            char pat[1200];
            glob_t g;
            snprintf(pat, sizeof pat, "%s/*/Documents/My Spore Creations/*/*.png", argv[1]);
            if (!glob(pat, 0, NULL, &g) && g.gl_pathc >= 2) {
                snprintf(a, sizeof a, "%s", g.gl_pathv[0]);
                snprintf(b, sizeof b, "%s", g.gl_pathv[g.gl_pathc - 1]);
                have = 1;
            }
            if (!glob(pat, 0, NULL, &g)) globfree(&g);
        }
        if (!have && argc >= 3) {
            snprintf(a, sizeof a, "%s", argv[1]);
            snprintf(b, sizeof b, "%s", argv[2]);
        } else if (!have) {
            char d[] = "/tmp/spore-dnd-mac-XXXXXX";
            if (!mkdtemp(d)) return 2;
            snprintf(a, sizeof a, "%s/A card.png", d);
            snprintf(b, sizeof b, "%s/B.PNG", d);
            fclose(fopen(a, "w"));
            fclose(fopen(b, "w"));
        }
        {   /* a drag from the Finder always carries absolute file URLs */
            char ra[1024], rb[1024];
            if (realpath(a, ra)) snprintf(a, sizeof a, "%s", ra);
            if (realpath(b, rb)) snprintf(b, sizeof b, "%s", rb);
        }
        printf("files: %s\n       %s\n", a, b);
        NSString *txt = [NSTemporaryDirectory() stringByAppendingPathComponent:@"spore-dnd-not-a-card.txt"];
        [@"x" writeToFile:txt atomically:YES encoding:NSUTF8StringEncoding error:nil];

        CHECK(sp_mac_app_init(0) == 0, "app init");
        sp_hwnd h = sp_window_register(proc, NULL, 0);
        CHECK(h != 0, "window registered");
        /* the game registers before the window is shown: pending until the NSWindow exists */
        CHECK(sp_mac_dnd_attach(h, on_notify, (void *)0x4242) == 2, "pending before the NSWindow");
        CHECK(sp_mac_attach_window(h, "drag and drop test", 120, 120, 640, 480) == 1, "attach");
        SPView *v = nil;
        for (NSWindow *w in NSApp.windows)
            if ([w.contentView respondsToSelector:@selector(hwnd)] && [(SPView *)w.contentView hwnd] == h) v = (SPView *)w.contentView;
        CHECK(v != nil, "view");
        if (!v) return 1;
        [v.window orderFront:nil];
        [v.window makeKeyWindow];
        int registered = 0;
        for (int i = 0; i < 100 && !registered; i++) {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                               inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e) [NSApp sendEvent:e];
            registered = [v.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL];
        }
        CHECK(registered, "registered for file URLs once the window appeared");
        printf("registration after the window appeared: %s\n", registered ? "done while pumping events" : "not seen");
        if (!registered) CHECK(sp_mac_dnd_attach(h, on_notify, (void *)0x4242) == 1, "attach once the window exists");

        double k = sp_mac_guest_scale();
        FakeDrag *f = [FakeDrag new];
        f.pb = [NSPasteboard pasteboardWithUniqueName];
        [f.pb clearContents];
        [f.pb writeObjects:@[ [NSURL fileURLWithPath:@(a)], [NSURL fileURLWithPath:txt], [NSURL fileURLWithPath:@(b)] ]];
        f.mask = NSDragOperationCopy | NSDragOperationMove | NSDragOperationLink | NSDragOperationGeneric;
        f.loc = NSMakePoint(100, 380);   /* window points, bottom-left origin: client (100, 100) points */
        sp_dnd_event e;

        /* 1: enter, move, exit */
        n_notify = 0;
        NSDragOperation op = [v draggingEntered:(id<NSDraggingInfo>)f];
        CHECK(op == NSDragOperationCopy, "enter op %lu", (unsigned long)op);
        CHECK(n_notify == 1, "notify %d", n_notify);
        CHECK(take(&e, SP_DND_ENTER), "ENTER queued");
        CHECK(e.hwnd == h && e.nfiles == 2, "ENTER hwnd 0x%x files %d", e.hwnd, e.nfiles);
        CHECK(e.x == (int32_t)(100 * k) && e.y == (int32_t)(100 * k), "ENTER point %d,%d", e.x, e.y);
        CHECK(e.nfiles == 2 && !strcmp(e.files[0], a) && !strcmp(e.files[1], b), "files %s | %s", e.nfiles ? e.files[0] : "",
              e.nfiles > 1 ? e.files[1] : "");
        CHECK(e.keys & 1, "MK_LBUTTON");
        sp_mac_dnd_event_free(&e);
        f.loc = NSMakePoint(200, 280);
        op = [v draggingUpdated:(id<NSDraggingInfo>)f];
        CHECK(op == NSDragOperationCopy, "update op");
        CHECK(take(&e, SP_DND_OVER) && e.x == (int32_t)(200 * k) && e.y == (int32_t)(200 * k) && !e.nfiles, "OVER %d,%d", e.x, e.y);
        [v draggingExited:(id<NSDraggingInfo>)f];
        CHECK(take(&e, SP_DND_LEAVE), "LEAVE queued");
        CHECK(!sp_mac_dnd_poll(&e), "queue empty");
        CHECK(n_notify == 3, "notify %d", n_notify);

        /* 2: enter and drop */
        f.loc = NSMakePoint(320, 240);
        op = [v draggingEntered:(id<NSDraggingInfo>)f];
        CHECK(op == NSDragOperationCopy, "enter op");
        CHECK([v prepareForDragOperation:(id<NSDraggingInfo>)f], "prepare");
        notify_answer = 2;   /* the game answers DROPEFFECT_MOVE on Drop: still a copy for the Finder */
        CHECK([v performDragOperation:(id<NSDraggingInfo>)f], "perform");
        [v concludeDragOperation:(id<NSDraggingInfo>)f];
        CHECK(take(&e, SP_DND_ENTER), "ENTER");
        sp_mac_dnd_event_free(&e);
        CHECK(take(&e, SP_DND_DROP) && e.nfiles == 2 && e.x == (int32_t)(320 * k) && e.y == (int32_t)(240 * k) && !(e.keys & 1),
              "DROP files %d at %d,%d keys %u", e.nfiles, e.x, e.y, e.keys);
        sp_mac_dnd_event_free(&e);
        notify_answer = ~0u;

        /* 3: the game refuses (DROPEFFECT_NONE) */
        notify_answer = 0;
        op = [v draggingEntered:(id<NSDraggingInfo>)f];
        CHECK(op == NSDragOperationNone, "refused enter op %lu", (unsigned long)op);
        [v draggingExited:(id<NSDraggingInfo>)f];
        while (sp_mac_dnd_poll(&e)) sp_mac_dnd_event_free(&e);
        notify_answer = ~0u;

        /* 4: no .png: refused, nothing queued, nobody notified */
        [f.pb clearContents];
        [f.pb writeObjects:@[ [NSURL fileURLWithPath:txt] ]];
        n_notify = 0;
        op = [v draggingEntered:(id<NSDraggingInfo>)f];
        CHECK(op == NSDragOperationNone, "text-only enter op");
        CHECK(![v prepareForDragOperation:(id<NSDraggingInfo>)f] && ![v performDragOperation:(id<NSDraggingInfo>)f], "no drop");
        CHECK(!sp_mac_dnd_poll(&e) && n_notify == 0, "nothing queued");

        /* 5: a source that does not allow copy */
        [f.pb clearContents];
        [f.pb writeObjects:@[ [NSURL fileURLWithPath:@(a)] ]];
        f.mask = NSDragOperationLink;
        CHECK([v draggingEntered:(id<NSDraggingInfo>)f] == NSDragOperationNone, "link-only source refused");
        CHECK(!sp_mac_dnd_poll(&e), "nothing queued");

        sp_mac_dnd_detach(h);
        CHECK(![v.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL], "detached");
        [f.pb releaseGlobally];
        [v.window orderOut:nil];
        sp_mac_detach_window(h);
        [[NSFileManager defaultManager] removeItemAtPath:txt error:nil];
    }
    printf("drag and drop host tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
