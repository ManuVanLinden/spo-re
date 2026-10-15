/* sp_gamepad.m -- game controllers over the GameController framework, feeding the four XInput users of
 * sp_gamepad_core.c.
 *
 * Started once by the first XInput call (sp_pad_start, game main thread). Controllers come and go through
 * GCControllerDidConnect/DidDisconnect (hot-plug); each extended-gamepad controller takes the lowest free user
 * (and its player LED shows it, as an Xbox pad's ring does on Windows); a fifth waits until a user frees up.
 * Input arrives on our own serial queue (the controllers' handlerQueue), so it never depends on how often the
 * game pumps; each change is mapped and stored under the core's lock. Rumble (XInputSetState) is applied on a
 * second queue through GCDeviceHaptics: one CHHapticEngine per handle when the pad has both (the low-frequency
 * motor on the left, the high-frequency one on the right, as on an Xbox pad), else one engine at the larger
 * speed. Our own code; no game code here. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <stdio.h>

#include "sp_macos.h"

static dispatch_queue_t g_padq;   /* attach, detach, input (the controllers' handlerQueue) */
static dispatch_queue_t g_rumq;   /* haptics */
static NSMutableArray<GCController *> *g_attached;   /* retains every controller that holds a user */
static NSMapTable *g_rumble;      /* GCController -> SPPadRumble, g_rumq only */
static int g_verbose = 1;

/* ---- extended gamepad -> host input --------------------------------------------------------------- */
void sp_gamepad_read(GCExtendedGamepad *g, sp_pad_input *in) {
    uint16_t b = 0;
    in->lx = g.leftThumbstick.xAxis.value;
    in->ly = g.leftThumbstick.yAxis.value;
    in->rx = g.rightThumbstick.xAxis.value;
    in->ry = g.rightThumbstick.yAxis.value;
    in->lt = g.leftTrigger.value;
    in->rt = g.rightTrigger.value;
    if (g.dpad.up.pressed) b |= SP_PAD_DPAD_UP;
    if (g.dpad.down.pressed) b |= SP_PAD_DPAD_DOWN;
    if (g.dpad.left.pressed) b |= SP_PAD_DPAD_LEFT;
    if (g.dpad.right.pressed) b |= SP_PAD_DPAD_RIGHT;
    if (g.buttonMenu.pressed) b |= SP_PAD_START;                 /* Xbox "Menu", PlayStation "Options" */
    if (g.buttonOptions && g.buttonOptions.pressed) b |= SP_PAD_BACK; /* Xbox "View", PlayStation "Share/Create" */
    if (g.leftThumbstickButton && g.leftThumbstickButton.pressed) b |= SP_PAD_LEFT_THUMB;
    if (g.rightThumbstickButton && g.rightThumbstickButton.pressed) b |= SP_PAD_RIGHT_THUMB;
    if (g.leftShoulder.pressed) b |= SP_PAD_LEFT_SHOULDER;
    if (g.rightShoulder.pressed) b |= SP_PAD_RIGHT_SHOULDER;
    if (g.buttonA.pressed) b |= SP_PAD_A;                         /* bottom face button (Cross) */
    if (g.buttonB.pressed) b |= SP_PAD_B;                         /* right (Circle) */
    if (g.buttonX.pressed) b |= SP_PAD_X;                         /* left (Square) */
    if (g.buttonY.pressed) b |= SP_PAD_Y;                         /* top (Triangle) */
    in->buttons = b;
}

void sp_gamepad_caps(GCController *c, sp_pad_caps *caps) {
    GCExtendedGamepad *g = c.extendedGamepad;
    memset(caps, 0, sizeof *caps);
    caps->subtype = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    caps->buttons = SP_PAD_DPAD_UP | SP_PAD_DPAD_DOWN | SP_PAD_DPAD_LEFT | SP_PAD_DPAD_RIGHT | SP_PAD_START |
                    SP_PAD_LEFT_SHOULDER | SP_PAD_RIGHT_SHOULDER | SP_PAD_A | SP_PAD_B | SP_PAD_X | SP_PAD_Y;
    if (g.buttonOptions) caps->buttons |= SP_PAD_BACK;
    if (g.leftThumbstickButton) caps->buttons |= SP_PAD_LEFT_THUMB;
    if (g.rightThumbstickButton) caps->buttons |= SP_PAD_RIGHT_THUMB;
    caps->triggers = 1;
    caps->sticks = 1;
    if (@available(macOS 11.0, *)) caps->rumble = c.haptics != nil;
    snprintf(caps->name, sizeof caps->name, "%s", c.vendorName ? c.vendorName.UTF8String : "controller");
}

static void push(GCController *c) {
    sp_pad_input in;
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return;
    sp_gamepad_read(g, &in);
    sp_pad_update((__bridge void *)c, &in);
}

/* g_padq. Returns the user, or -1 (not a gamepad, or all four taken). */
int sp_gamepad_attach(GCController *c) {
    sp_pad_caps caps;
    GCExtendedGamepad *g = c.extendedGamepad;
    int u;
    if (!g) return -1;
    if ((u = sp_pad_user_of((__bridge void *)c)) >= 0) return u;
    sp_gamepad_caps(c, &caps);
    u = sp_pad_connect((__bridge void *)c, &caps);
    if (u < 0) {
        if (g_verbose) fprintf(stderr, "platform: gamepad \"%s\" waits: all four XInput users are taken\n", caps.name);
        return -1;
    }
    [g_attached addObject:c];
    if (g_padq) c.handlerQueue = g_padq;
    c.playerIndex = (GCControllerPlayerIndex)u;
    __weak GCController *wc = c;
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) {
        GCController *sc = wc;
        if (sc) push(sc);
    };
    push(c);
    if (g_verbose)
        fprintf(stderr, "platform: gamepad \"%s\" connected as XInput user %d%s\n", caps.name, u,
                caps.rumble ? " (haptics)" : "");
    return u;
}

static void rumble_stop(GCController *c);
/* One reference only: the same controller may have attached again since. */
static void release_one(GCController *c) {
    NSUInteger i = [g_attached indexOfObjectIdenticalTo:c];
    if (i != NSNotFound) [g_attached removeObjectAtIndex:i];
}

/* g_padq */
int sp_gamepad_detach(GCController *c) {
    int u = sp_pad_disconnect((__bridge void *)c);
    if (u < 0) return -1;
    c.extendedGamepad.valueChangedHandler = nil;
    c.playerIndex = GCControllerPlayerIndexUnset;
    if (g_verbose) fprintf(stderr, "platform: gamepad \"%s\" disconnected from XInput user %d\n",
                           c.vendorName ? c.vendorName.UTF8String : "controller", u);
    if (g_rumq) {
        dispatch_async(g_rumq, ^{ rumble_stop(c); });
        /* XInputSetState may still hold the bare pointer for a moment: release it later, not now. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), g_padq, ^{ release_one(c); });
    } else {
        release_one(c);
    }
    /* A waiting fifth pad takes the free user. */
    for (GCController *o in GCController.controllers)
        if (o != c && sp_pad_user_of((__bridge void *)o) < 0) sp_gamepad_attach(o);
    return u;
}

/* ---- rumble ------------------------------------------------------------------------------------- */
API_AVAILABLE(macos(11.0))
@interface SPPadMotor : NSObject
@property (nonatomic, strong) CHHapticEngine *engine;
@property (nonatomic, strong) id<CHHapticAdvancedPatternPlayer> player;
@property (nonatomic) BOOL started, running;
@property (nonatomic) float sharpness;
@end
@implementation SPPadMotor
- (void)set:(uint16_t)v {
    NSError *err = nil;
    if (!v) {
        if (self.running) [self.player stopAtTime:CHHapticTimeImmediate error:&err];
        self.running = NO;
        return;
    }
    if (!self.started) {
        if (![self.engine startAndReturnError:&err]) return;
        self.started = YES;
    }
    if (!self.player) {
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f],
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:self.sharpness]
        ] relativeTime:0 duration:30.0];
        CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
        if (!pat) return;
        self.player = [self.engine createAdvancedPlayerWithPattern:pat error:&err];
        if (!self.player) return;
        self.player.loopEnabled = YES;
    }
    CHHapticDynamicParameter *p = [[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:(float)v / 65535.0f relativeTime:0];
    [self.player sendParameters:@[p] atTime:CHHapticTimeImmediate error:&err];
    if (!self.running) self.running = [self.player startAtTime:CHHapticTimeImmediate error:&err];
}
- (void)shutdown {
    [self set:0];
    if (self.started) [self.engine stopWithCompletionHandler:nil];
    self.started = NO;
}
@end

API_AVAILABLE(macos(11.0))
@interface SPPadRumble : NSObject
@property (nonatomic, strong) SPPadMotor *lo, *hi;   /* hi nil: one engine for both motors */
@end
@implementation SPPadRumble
@end

API_AVAILABLE(macos(11.0))
static SPPadMotor *make_motor(GCDeviceHaptics *h, GCHapticsLocality loc, float sharpness) {
    CHHapticEngine *e = [h createEngineWithLocality:loc];
    if (!e) return nil;
    SPPadMotor *m = [SPPadMotor new];
    m.engine = e;
    m.sharpness = sharpness;
    e.playsHapticsOnly = YES;
    __weak SPPadMotor *wm = m;
    e.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
        dispatch_async(g_rumq, ^{ SPPadMotor *s = wm; if (s) { s.started = NO; s.running = NO; } });
    };
    e.resetHandler = ^{
        dispatch_async(g_rumq, ^{ SPPadMotor *s = wm; if (s) { s.started = NO; s.running = NO; s.player = nil; } });
    };
    return m;
}

static void rumble_apply(GCController *c, uint16_t lo, uint16_t hi) {
    if (@available(macOS 11.0, *)) {
        SPPadRumble *r = [g_rumble objectForKey:c];
        if (!r) {
            GCDeviceHaptics *h = c.haptics;
            if (!h) return;
            r = [SPPadRumble new];
            NSSet *loc = h.supportedLocalities;
            if ([loc containsObject:GCHapticsLocalityLeftHandle] && [loc containsObject:GCHapticsLocalityRightHandle]) {
                r.lo = make_motor(h, GCHapticsLocalityLeftHandle, 0.2f);
                r.hi = make_motor(h, GCHapticsLocalityRightHandle, 0.8f);
            } else {
                r.lo = make_motor(h, GCHapticsLocalityDefault, 0.5f);
            }
            if (!r.lo) return;
            [g_rumble setObject:r forKey:c];
        }
        if (r.hi) {
            [r.lo set:lo];
            [r.hi set:hi];
        } else {
            [r.lo set:lo > hi ? lo : hi];
        }
    }
}

static void rumble_stop(GCController *c) {
    if (@available(macOS 11.0, *)) {
        SPPadRumble *r = [g_rumble objectForKey:c];
        if (!r) return;
        [r.lo shutdown];
        [r.hi shutdown];
        [g_rumble removeObjectForKey:c];
    }
}

/* Any thread (the game's, inside XInputSetState): only queues. */
static void rumble_cb(void *dev, uint16_t lo, uint16_t hi) {
    GCController *c = (__bridge GCController *)dev;
    dispatch_async(g_rumq, ^{ rumble_apply(c, lo, hi); });
}

/* ---- start -------------------------------------------------------------------------------------- */
void sp_gamepad_setup_queues(int verbose) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_padq = dispatch_queue_create("spore.gamepad.input", DISPATCH_QUEUE_SERIAL);
        g_rumq = dispatch_queue_create("spore.gamepad.rumble", DISPATCH_QUEUE_SERIAL);
        g_attached = [NSMutableArray new];
        g_rumble = [NSMapTable strongToStrongObjectsMapTable];
        sp_pad_set_rumble_handler(rumble_cb);
    });
    g_verbose = verbose;
}

/* The padq, for tests that drive attach/detach with virtual controllers. */
dispatch_queue_t sp_gamepad_queue(void) { return g_padq; }

void sp_mac_gamepad_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        sp_gamepad_setup_queues(1);
        /* Windows XInput reads pads whether or not the game is in front (the game ignores input while it is
         * inactive anyway, DEVIATIONS 4.6). */
        if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            GCController *c = n.object;
            if (c) dispatch_async(g_padq, ^{ sp_gamepad_attach(c); });
        }];
        [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            GCController *c = n.object;
            if (c) dispatch_async(g_padq, ^{ sp_gamepad_detach(c); });
        }];
        /* Pads already known; the rest arrive as connect notifications once discovery finishes. */
        dispatch_async(g_padq, ^{
            for (GCController *c in GCController.controllers) sp_gamepad_attach(c);
        });
    });
}
