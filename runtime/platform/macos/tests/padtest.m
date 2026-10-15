/* padtest.m -- the GameController back end (sp_gamepad.m) on the Mac it runs on. No window.
 *   1. Virtual controllers (+[GCController controllerWithExtendedGamepad]): element values set by the test go
 *      through sp_gamepad_read and the XInput users exactly as a real pad's would (mapping, packet numbers,
 *      five pads on four users, detach and the waiting pad taking the free user).
 *   2. Real controllers: starts the back end (notifications, discovery), runs the main run loop for --seconds N
 *      (default 3) and prints every user's XInputGetState-equivalent state whenever its packet changes; with
 *      --rumble, a short low/high rumble on each pad that has haptics. Says when no pad is attached. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp_macos.h"

void sp_gamepad_read(GCExtendedGamepad *g, sp_pad_input *in);
int sp_gamepad_attach(GCController *c);
int sp_gamepad_detach(GCController *c);
void sp_gamepad_setup_queues(int verbose);
dispatch_queue_t sp_gamepad_queue(void);

static int t_pass, t_fail;
#define CHECK(c, ...) do { if (c) t_pass++; else { t_fail++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
        CHECK(a_ == b_, "%s = %lld, expected %lld", #a, a_, b_); } while (0)

static int on_padq(int (^blk)(void)) {
    __block int r;
    dispatch_sync(sp_gamepad_queue(), ^{ r = blk(); });
    return r;
}

static void test_virtual(void) {
    sp_pad_input in;
    sp_pad_xstate x;
    uint32_t pk, pk2;
    sp_pad_caps caps;
    if (@available(macOS 10.15, *)) {
        NSMutableArray<GCController *> *v = [NSMutableArray new];
        for (int i = 0; i < 5; i++) [v addObject:[GCController controllerWithExtendedGamepad]];
        GCExtendedGamepad *g = v[0].extendedGamepad;
        CHECK(g != nil, "virtual extended gamepad");
        /* at rest */
        sp_gamepad_read(g, &in);
        CHECK(in.buttons == 0 && in.lx == 0 && in.lt == 0, "rest");
        /* every button, one at a time */
        struct { GCControllerButtonInput *b; uint16_t bit; const char *name; } bt[] = {
            {g.buttonA, SP_PAD_A, "A"}, {g.buttonB, SP_PAD_B, "B"}, {g.buttonX, SP_PAD_X, "X"}, {g.buttonY, SP_PAD_Y, "Y"},
            {g.leftShoulder, SP_PAD_LEFT_SHOULDER, "LB"}, {g.rightShoulder, SP_PAD_RIGHT_SHOULDER, "RB"},
            {g.buttonMenu, SP_PAD_START, "Menu->START"}, {g.buttonOptions, SP_PAD_BACK, "Options->BACK"},
            {g.leftThumbstickButton, SP_PAD_LEFT_THUMB, "L3"}, {g.rightThumbstickButton, SP_PAD_RIGHT_THUMB, "R3"},
            {g.dpad.up, SP_PAD_DPAD_UP, "up"}, {g.dpad.down, SP_PAD_DPAD_DOWN, "down"},
            {g.dpad.left, SP_PAD_DPAD_LEFT, "left"}, {g.dpad.right, SP_PAD_DPAD_RIGHT, "right"},
        };
        for (unsigned i = 0; i < sizeof bt / sizeof bt[0]; i++) {
            if (!bt[i].b) { printf("virtual pad has no %s element (skipped)\n", bt[i].name); continue; }
            if (i >= 10) {   /* the d-pad: set through its axes */
                float dx = bt[i].bit == SP_PAD_DPAD_LEFT ? -1 : bt[i].bit == SP_PAD_DPAD_RIGHT ? 1 : 0;
                float dy = bt[i].bit == SP_PAD_DPAD_UP ? 1 : bt[i].bit == SP_PAD_DPAD_DOWN ? -1 : 0;
                [g.dpad setValueForXAxis:dx yAxis:dy];
            } else {
                [bt[i].b setValue:1.0f];
            }
            sp_gamepad_read(g, &in);
            sp_pad_map(&in, &x);
            CHECK(x.buttons == bt[i].bit, "%s -> 0x%04x (got 0x%04x)", bt[i].name, bt[i].bit, x.buttons);
            if (i >= 10) [g.dpad setValueForXAxis:0 yAxis:0];
            else [bt[i].b setValue:0.0f];
        }
        /* sticks and triggers */
        [g.leftThumbstick setValueForXAxis:1.0f yAxis:1.0f];      /* right and up */
        [g.rightThumbstick setValueForXAxis:-1.0f yAxis:-0.5f];   /* left and down */
        [g.leftTrigger setValue:1.0f];
        [g.rightTrigger setValue:0.5f];
        sp_gamepad_read(g, &in);
        sp_pad_map(&in, &x);
        CHECK(x.lx == 32767 && x.ly == 32767, "left stick right+up = (%d, %d)", x.lx, x.ly);
        CHECK(x.rx == -32768 && x.ry == -16384, "right stick left+down = (%d, %d)", x.rx, x.ry);
        CHECK(x.lt == 255 && x.rt == 128, "triggers %u %u", x.lt, x.rt);
        [g.leftThumbstick setValueForXAxis:0 yAxis:0];
        [g.rightThumbstick setValueForXAxis:0 yAxis:0];
        [g.leftTrigger setValue:0];
        [g.rightTrigger setValue:0];

        /* the users: five virtual pads on four */
        sp_pad_reset();
        for (int i = 0; i < 4; i++) CHECK_EQ(on_padq(^{ return sp_gamepad_attach(v[i]); }), i);
        CHECK_EQ(on_padq(^{ return sp_gamepad_attach(v[4]); }), -1);
        CHECK_EQ(on_padq(^{ return sp_gamepad_attach(v[2]); }), 2);   /* again: same user */
        CHECK_EQ(sp_pad_get_caps(0, &caps), 0);
        CHECK((caps.buttons & 0xf3ffu) == caps.buttons && (caps.buttons & (SP_PAD_A | SP_PAD_START)) == (SP_PAD_A | SP_PAD_START),
              "caps buttons 0x%04x", caps.buttons);
        printf("virtual pad: \"%s\", caps buttons 0x%04x, haptics %d, player index %ld\n", caps.name, caps.buttons,
               caps.rumble, (long)v[0].playerIndex);
        CHECK_EQ((long)v[3].playerIndex, 3);
        /* an element change reaches the user (through the valueChangedHandler on our queue, or a re-read) */
        sp_pad_get_state(1, &pk, &x);
        [v[1].extendedGamepad.buttonY setValue:1.0f];
        int via_handler = 0;
        for (int i = 0; i < 50 && !via_handler; i++) {
            on_padq(^{ return 0; });   /* drain the queue */
            sp_pad_get_state(1, &pk2, &x);
            if (x.buttons == SP_PAD_Y) via_handler = 1;
            else usleep(2000);
        }
        if (!via_handler) {   /* virtual pads may not fire the handler: push by hand */
            on_padq(^{ sp_pad_input i2; sp_gamepad_read(v[1].extendedGamepad, &i2); return sp_pad_update((__bridge void *)v[1], &i2); });
            sp_pad_get_state(1, &pk2, &x);
        }
        printf("element change reached user 1 %s\n", via_handler ? "through the valueChangedHandler" : "by a re-read (the handler did not fire for a virtual pad)");
        CHECK_EQ(x.buttons, SP_PAD_Y);
        CHECK(pk2 != pk, "packet changed");
        /* detach user 1: the waiting fifth pad cannot be found through GCController.controllers (virtual pads are
         * not listed), so attach it the way a connect notification would */
        CHECK_EQ(on_padq(^{ return sp_gamepad_detach(v[1]); }), 1);
        CHECK_EQ(sp_pad_get_state(1, NULL, NULL), -1);
        CHECK_EQ(on_padq(^{ return sp_gamepad_attach(v[4]); }), 1);
        sp_pad_get_state(1, NULL, &x);
        CHECK_EQ(x.buttons, 0);
        for (int i = 0; i < 5; i++) on_padq(^{ return sp_gamepad_detach(v[i]); });
        for (int u = 0; u < 4; u++) CHECK_EQ(sp_pad_get_state(u, NULL, NULL), -1);
    } else {
        printf("macOS before 10.15: no virtual controllers; skipped\n");
    }
}

static void real(double seconds, int rumble) {
    uint32_t last[SP_PAD_USERS] = {0};
    int seen[SP_PAD_USERS] = {0}, any = 0, rumbled[SP_PAD_USERS] = {0};
    sp_pad_reset();
    sp_mac_gamepad_start();
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([end timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.016]];
        for (int u = 0; u < SP_PAD_USERS; u++) {
            uint32_t pk;
            sp_pad_xstate x;
            if (sp_pad_get_state(u, &pk, &x)) { seen[u] = 0; continue; }
            if (!seen[u] || pk != last[u]) {
                sp_pad_caps c;
                sp_pad_get_caps(u, &c);
                printf("user %d \"%s\" packet %u: buttons 0x%04x LT %3u RT %3u L (%6d, %6d) R (%6d, %6d)\n", u, c.name, pk,
                       x.buttons, x.lt, x.rt, x.lx, x.ly, x.rx, x.ry);
                if (rumble && c.rumble && !rumbled[u]) {
                    rumbled[u] = 1;
                    printf("user %d: rumble low 0x8000 for 0.4 s, then high 0x8000 for 0.4 s\n", u);
                    sp_pad_set_vibration(u, 0x8000, 0);
                    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
                    sp_pad_set_vibration(u, 0, 0x8000);
                    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
                    sp_pad_set_vibration(u, 0, 0);
                }
                seen[u] = 1;
                any = 1;
                last[u] = pk;
            }
        }
    }
    printf("real controllers: %s (GCController.controllers lists %lu)\n", any ? "at least one attached" : "none attached",
           (unsigned long)GCController.controllers.count);
}

int main(int argc, char **argv) {
    double seconds = 3;
    int rumble = 0, virt = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rumble")) rumble = 1;
        else if (!strcmp(argv[i], "--real-only")) virt = 0;
    }
    @autoreleasepool {
        sp_gamepad_setup_queues(1);
        if (virt) test_virtual();
        if (seconds > 0) real(seconds, rumble);
    }
    printf("padtest: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
