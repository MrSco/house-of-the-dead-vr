// Game pads through GameController (the Apple front ends: main_ios.m, main_mac.m, tvos/main_tvos.m): every extended
// gamepad connected, each a device of its own (flat.h's players: alone each is player 1's; Start on another pad than
// player 1's brings in player 2). A pad that disconnects lets go of its trigger; one that connects again takes a gone
// pad's device id (flat_pad_attach), so player 2's pad is player 2's again.
//  - either stick moves the pad's sight, R2 / R1 shoot, L2 / L1 / X reload, A / Menu give coins and Start, Options
//    opens the menu; in the menu (iOS, Mac) the D-pad or the left stick moves, A chooses, B / Options go back, the
//    bumpers switch tabs. (tvOS has its own: the remote's menu keys, apple_pads_each.)
//  - each pad rumbles for its own player's shots, reloads and hits (flat.h's rumble: apple_pad_rumble), through its
//    GCController.haptics (iOS and tvOS 14, macOS 11; a pad with motors: DualSense, DualShock 4, Xbox pads). The Siri
//    Remote has none. Its triggers kick too (flat.h's trigger feedback: apple_pad_trigger): a DualSense's adaptive
//    triggers (GCDualSenseAdaptiveTrigger), an Xbox One / Series pad's trigger motors (its haptics at the triggers).
#pragma once
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "flat/flat.h"
#include "platform/platform.h"

typedef struct ApplePad {
    int dev;                     // flat.h's device id
    float sx, sy;                // the sight, window pixels (-1: not yet: from the middle)
    int on, fire, reload;        // aiming (it moved or shot), the trigger and reload as last sent
    int prev[16];                // the front end's buttons last frame (its own meaning)
} ApplePad;

#define APPLE_PADS 8
static GCController *apple_pad_ctl[APPLE_PADS];   // (a slot's controller; nil: free)
static ApplePad apple_pad[APPLE_PADS];

// each extended gamepad connected, with its state (new ones attached, gone ones detached first)
static inline void apple_pads_each(void (^fn)(GCExtendedGamepad *g, ApplePad *p)) {
    NSArray<GCController *> *now = GCController.controllers;
    for (int i = 0; i < APPLE_PADS; i++)
        if (apple_pad_ctl[i] && (![now containsObject:apple_pad_ctl[i]] || !apple_pad_ctl[i].extendedGamepad)) {
            flat_pad_detach(apple_pad[i].dev);
            apple_pad_ctl[i] = nil;
        }
    for (GCController *c in now) {
        if (!c.extendedGamepad) continue;
        int slot = -1, free_slot = -1;
        for (int i = 0; i < APPLE_PADS; i++) {
            if (apple_pad_ctl[i] == c) slot = i;
            if (!apple_pad_ctl[i] && free_slot < 0) free_slot = i;
        }
        if (slot < 0) {
            if (free_slot < 0) continue;
            slot = free_slot;
            apple_pad_ctl[slot] = c;
            memset(&apple_pad[slot], 0, sizeof apple_pad[slot]);
            apple_pad[slot].sx = apple_pad[slot].sy = -1;
            apple_pad[slot].dev = flat_pad_attach();
            const char *trig = "none";
            if (@available(iOS 14.5, tvOS 14.5, macOS 11.3, *))
                if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) trig = "DualSense adaptive triggers";
            if (@available(iOS 14.0, tvOS 14.0, macOS 11.0, *))
                if (strcmp(trig, "none") == 0 && [c.haptics.supportedLocalities containsObject:GCHapticsLocalityRightTrigger])
                    trig = "trigger motors";
            plat_log(PLAT_LOG_INFO, "pad: %s, trigger feedback: %s", c.vendorName ? c.vendorName.UTF8String : "a game pad", trig);
        }
        // the pad's lights (where it has them): its player
        const GCControllerPlayerIndex want = flat_dev_player(apple_pad[slot].dev) ? GCControllerPlayerIndex2 : GCControllerPlayerIndex1;
        if (c.playerIndex != want) c.playerIndex = want;
        fn(c.extendedGamepad, &apple_pad[slot]);
    }
}

static inline float apple_pad_dz(float v) { const float d = 0.2f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }

// The game's side of a pad (a frame): Start, the sight (moved in *sx, *sy: the pad's own or, tvOS, one it shares
// with the remote; clamped to the picture plus `past` of it, or, past < 0, a little inside it), the trigger,
// reload. Returns 1 when it aimed.
static inline int apple_pad_game(GCExtendedGamepad *g, ApplePad *p, double dt, float *sx, float *sy, float past, int start_pressed) {
    if (start_pressed) { flat_dev_start(p->dev, 1); flat_dev_start(p->dev, 0); }
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0) return 0;
    if (*sx < 0) { *sx = vx + vw * 0.5f; *sy = vy + vh * 0.5f; }
    const float lx = apple_pad_dz(g.leftThumbstick.xAxis.value), ly = apple_pad_dz(g.leftThumbstick.yAxis.value);
    const float rx = apple_pad_dz(g.rightThumbstick.xAxis.value), ry = apple_pad_dz(g.rightThumbstick.yAxis.value);
    const float mx = fabsf(rx) > fabsf(lx) ? rx : lx, my = fabsf(ry) > fabsf(ly) ? ry : ly;
    if (mx || my) {
        p->on = 1;
        if (past >= 0) {
            *sx = fminf(fmaxf(*sx + mx * vw * 2.0f * (float)dt, vx - vw * past), vx + vw * (1 + past));
            *sy = fminf(fmaxf(*sy - my * vw * 2.0f * (float)dt, vy - vh * past), vy + vh * (1 + past));
        } else {   // (tvOS: on the picture, so its crosshair always shows)
            *sx = fminf(fmaxf(*sx + mx * vw * 2.0f * (float)dt, vx + 2.0f), vx + vw - 3.0f);
            *sy = fminf(fmaxf(*sy - my * vw * 2.0f * (float)dt, vy + 2.0f), vy + vh - 3.0f);
        }
    }
    if (p->on) flat_dev_pointer(p->dev, (int)*sx, (int)*sy);
    const int fire = g.rightTrigger.value > 0.4f || g.rightShoulder.pressed;
    const int rel = g.leftTrigger.value > 0.4f || g.leftShoulder.pressed || g.buttonX.pressed;
    if (fire != p->fire) { p->on = 1; flat_dev_pointer(p->dev, (int)*sx, (int)*sy); flat_dev_trigger(p->dev, fire); p->fire = fire; }
    if (rel != p->reload) { flat_dev_reload(p->dev, rel); p->reload = rel; }
    return (mx || my) != 0;
}
// in the menu: its trigger and reload let go
static inline void apple_pad_release(ApplePad *p) {
    if (p->fire) flat_dev_trigger(p->dev, 0);
    if (p->reload) flat_dev_reload(p->dev, 0);
    p->fire = p->reload = 0;
}

// iOS and the Mac: every pad's frame
enum { AP_A, AP_B, AP_MENU, AP_OPT, AP_LB, AP_RB, AP_UP, AP_DOWN, AP_LEFT, AP_RIGHT };
static inline void apple_pads_frame(double dt) {
    apple_pads_each(^(GCExtendedGamepad *g, ApplePad *p) {
        int cur[16] = { 0 };
        cur[AP_A] = g.buttonA.pressed; cur[AP_B] = g.buttonB.pressed;
        cur[AP_MENU] = g.buttonMenu.pressed; cur[AP_OPT] = g.buttonOptions.pressed;
        cur[AP_LB] = g.leftShoulder.pressed; cur[AP_RB] = g.rightShoulder.pressed;
        cur[AP_UP] = g.dpad.up.pressed || g.leftThumbstick.yAxis.value > 0.5f;
        cur[AP_DOWN] = g.dpad.down.pressed || g.leftThumbstick.yAxis.value < -0.5f;
        cur[AP_LEFT] = g.dpad.left.pressed || g.leftThumbstick.xAxis.value < -0.5f;
        cur[AP_RIGHT] = g.dpad.right.pressed || g.leftThumbstick.xAxis.value > 0.5f;
        int pressed[16];
        for (int i = 0; i < 16; i++) pressed[i] = cur[i] && !p->prev[i];
        memcpy(p->prev, cur, sizeof p->prev);
        if (flat_menu_is_open()) {
            apple_pad_release(p);
            if (pressed[AP_A]) flat_menu_key(FLAT_KEY_ENTER);
            if (pressed[AP_B] || pressed[AP_OPT]) flat_menu_key(FLAT_KEY_BACK);
            if (pressed[AP_RB]) flat_menu_key(FLAT_KEY_TAB);
            if (pressed[AP_LB]) flat_menu_key(FLAT_KEY_TAB_BACK);
            if (pressed[AP_UP]) flat_menu_key(FLAT_KEY_UP);
            if (pressed[AP_DOWN]) flat_menu_key(FLAT_KEY_DOWN);
            if (pressed[AP_LEFT]) flat_menu_key(FLAT_KEY_LEFT);
            if (pressed[AP_RIGHT]) flat_menu_key(FLAT_KEY_RIGHT);
            return;
        }
        if (pressed[AP_OPT]) { flat_menu_set_open(1); return; }
        apple_pad_game(g, p, dt, &p->sx, &p->sy, 0.05f, pressed[AP_A] || pressed[AP_MENU]);
    });
}
// ---- rumble (flat.h's flat_os_rumble, for the pad that is device dev): a CoreHaptics engine for each pad, made,
// started and played on a queue of its own, so the frame never waits for it. One event at a time per pad (a new one
// stops the last); ms 0 stops it. Its strength is the stronger motor's; sharp for the light motor, dull for the heavy.
// Engines: [slot][0] the pad's body, [slot][1] its left trigger, [slot][2] its right (an Xbox pad's trigger motors).
enum { AH_BODY, AH_LEFT_TRIGGER, AH_RIGHT_TRIGGER };
static CHHapticEngine *apple_pad_engine[APPLE_PADS][3];
static GCController *apple_pad_engine_of[APPLE_PADS][3];   // (the pad its engine was made for)
static id<CHHapticPatternPlayer> apple_pad_playing[APPLE_PADS][3];
static inline int apple_rumble_log(void) {
    static int log = -1;
    if (log < 0) log = getenv("FLAT_TEST_RUMBLE") && atoi(getenv("FLAT_TEST_RUMBLE"));
    return log;
}
// (on the queue) pad slot's engine `which` plays strength 0..1 (sharp 0..1) for ms; ms 0 stops it
static inline void apple_haptic_play(int slot, int which, GCController *c, float strength, float sharp, int ms) API_AVAILABLE(ios(14.0), tvos(14.0), macos(11.0)) {
    if (apple_pad_engine_of[slot][which] != c) {   // (a pad new in this slot: its own engine)
        [apple_pad_engine[slot][which] stopWithCompletionHandler:nil];
        apple_pad_engine[slot][which] = nil;
        apple_pad_playing[slot][which] = nil;
        apple_pad_engine_of[slot][which] = c;
        GCHapticsLocality where = which == AH_LEFT_TRIGGER ? GCHapticsLocalityLeftTrigger
                                  : which == AH_RIGHT_TRIGGER ? GCHapticsLocalityRightTrigger : GCHapticsLocalityDefault;
        CHHapticEngine *e = which == AH_BODY || [c.haptics.supportedLocalities containsObject:where]
                                ? [c.haptics createEngineWithLocality:where] : nil;
        NSError *err = nil;
        if (e && [e startAndReturnError:&err]) {
            __weak CHHapticEngine *w = e;
            e.resetHandler = ^{ [w startAndReturnError:nil]; };
            e.playsHapticsOnly = YES;
            apple_pad_engine[slot][which] = e;
        }
        if (apple_rumble_log())
            plat_log(PLAT_LOG_INFO, "rumble: pad %d %s haptics %s", apple_pad[slot].dev - FLAT_DEV_PAD + 1,
                     which == AH_BODY ? "body" : which == AH_LEFT_TRIGGER ? "left trigger" : "right trigger",
                     apple_pad_engine[slot][which] ? "on" : c.haptics ? "none there" : "none (no motors)");
    }
    CHHapticEngine *e = apple_pad_engine[slot][which];
    if (!e) return;
    [apple_pad_playing[slot][which] stopAtTime:CHHapticTimeImmediate error:nil];
    apple_pad_playing[slot][which] = nil;
    if (ms <= 0) return;
    CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:strength],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharp] ]
        relativeTime:0 duration:ms / 1000.0];
    NSError *err = nil;
    CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
    id<CHHapticPatternPlayer> pl = pat ? [e createPlayerWithPattern:pat error:&err] : nil;
    if (pl && [pl startAtTime:CHHapticTimeImmediate error:&err]) apple_pad_playing[slot][which] = pl;
    else apple_pad_engine_of[slot][which] = nil;   // (its engine made again next time)
}
static inline dispatch_queue_t apple_haptics_queue(void) {
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("hotdvr.rumble", DISPATCH_QUEUE_SERIAL);
    return q;
}
static inline void apple_pad_rumble(int dev, float low, float high, int ms) {
    for (int i = 0; i < APPLE_PADS; i++) {
        GCController *c = apple_pad_ctl[i];
        if (!c || apple_pad[i].dev != dev) continue;
        const int slot = i;
        const float strength = fminf(fmaxf(fmaxf(low, high), 0), 1), sharp = low + high > 0 ? high / (low + high) : 0.5f;
        dispatch_async(apple_haptics_queue(), ^{
            if (@available(iOS 14.0, tvOS 14.0, macOS 11.0, *)) apple_haptic_play(slot, AH_BODY, c, strength, sharp, ms);
        });
    }
}
// ---- trigger feedback (flat.h's flat_os_trigger, for the pad that is device dev): a DualSense's trigger mode set at
// once (GameController sends it); an Xbox pad's trigger motor played on the haptics queue
static inline void apple_pad_trigger(int dev, int right, int mode, float amp, int ms) {
    for (int i = 0; i < APPLE_PADS; i++) {
        GCController *c = apple_pad_ctl[i];
        if (!c || apple_pad[i].dev != dev) continue;
        if (@available(iOS 14.5, tvOS 14.5, macOS 11.3, *)) {
            if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) {
                GCDualSenseGamepad *ds = (GCDualSenseGamepad *)c.extendedGamepad;
                GCDualSenseAdaptiveTrigger *t = right ? ds.rightTrigger : ds.leftTrigger;
                // a gun's: resistance from 30% of the pull, a snap at 60%; a shot's buzz as GoldenEye VR's
                if (mode == FLAT_TRIG_WEAPON) [t setModeWeaponWithStartPosition:0.3f endPosition:0.6f resistiveStrength:0.85f];
                else if (mode == FLAT_TRIG_BUZZ) [t setModeVibrationWithStartPosition:0.f amplitude:fminf(fmaxf(amp, 0), 1) frequency:0.6f];
                else [t setModeOff];
                continue;
            }
        }
        if (mode == FLAT_TRIG_WEAPON) continue;   // (an Xbox pad: its motors only buzz)
        const int slot = i, which = right ? AH_RIGHT_TRIGGER : AH_LEFT_TRIGGER;
        const float strength = fminf(fmaxf(amp, 0), 1);
        dispatch_async(apple_haptics_queue(), ^{
            if (@available(iOS 14.0, tvOS 14.0, macOS 11.0, *))
                apple_haptic_play(slot, which, c, strength, 1.0f, mode == FLAT_TRIG_BUZZ ? ms : 0);
        });
    }
}

// the mouse or touch aims again: the sights of the pads that drive its player start in the middle next time
static inline void apple_pads_pointer_moved(int dev) {
    for (int i = 0; i < APPLE_PADS; i++)
        if (apple_pad_ctl[i] && flat_dev_player(apple_pad[i].dev) == flat_dev_player(dev)) { apple_pad[i].on = 0; apple_pad[i].sx = -1; }
}
