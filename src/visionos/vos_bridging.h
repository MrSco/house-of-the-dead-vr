// What the visionOS app's Swift (VosApp.swift, VosControllers.swift) and its C / Objective-C (xr_vos.m) share.
// Swift reads this as its bridging header.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// xr_vos.m: the app started (Documents, the log, the audio session); the immersive space's layer renderer
// (unretained: the first starts the compositor's thread and the game)
void vosAppStart(void);
void vosLayerReady(void *layerRenderer);

// One PlayStation VR2 Sense controller's buttons, as a Touch controller's (VosControllers.swift)
typedef struct VosSenseHand {
    int connected;
    float stickX, stickY;
    float trigger, squeeze;
    int a, b, menu, stickClick, triggerTouch;
} VosSenseHand;
// both hands (out[0] left, out[1] right); returns how many are connected
int vosSenseSync(double displaySeconds, VosSenseHand *out);
void vosSenseHaptic(int hand, float amplitude, double seconds);

// The document picker: VosApp.swift opens a window for it (vosPickerOpen); the window answers xr_vos.m
// (vosPickerDone): 1 with the copy's path, 0 cancelled, -1 it could not be shown
void vosPickerOpen(const char *title);
void vosPickerDone(int status, const char *path);

#ifdef __cplusplus
}
#endif
