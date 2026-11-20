// Apple Vision Pro (docs/platforms.md, P5): what the visionOS app's parts share.
//
//   VosApp.swift        the SwiftUI app: one full-immersion space (a CompositorLayer), and the document picker's
//                       window; VosControllers.swift: PlayStation VR2 Sense controllers' buttons (GameController)
//   vos_render.m        the compositor's thread: ANGLE's GL context, ARKit (the head, the hands, the controllers'
//                       poses), each display frame's input, one GL pass per eye into Metal textures, then into the
//                       drawable
//   vos_shell.c         the VR shell (src/quest/quest_xr.c's, without OpenXR): the game's world and the cabinet
//                       view, guns, hands, the menu, online play's poses
//
// Spaces: LOCAL is ARKit's origin raised to the head's first height (as the Quest's LOCAL, at your eyes where the app
// started); metres, +y up, -z forward. Matrices are column-major 4x4 (GL's and simd's).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// a Sense controller, laid out as a Touch controller
typedef struct {
    int connected, pose_ok;
    float aim[16];               // LOCAL <- the controller's aim pose (-z along its ray)
    float trigger, squeeze;      // 0..1
    float stick_x, stick_y;      // up is +
    int fire;                    // the trigger clicked (with some hysteresis)
    int a, b, menu, stick_click, thumb;   // A / X, B / Y, the menu button, the stick's click, the thumb resting
} VosController;

// a tracked hand (ARKit's skeleton, read in vos_render.m)
typedef struct {
    int tracked;
    float aim[16];               // LOCAL <- the aim ray's pose: at the hand, -z along the ray
    int pinch;                   // thumb and index finger together
    float pinch_strength;        // 0..1 (the hand model's trigger finger)
    int middle_pinch;            // thumb and middle finger together
    int palm_to_face;            // the palm turned to the head (the menu gesture's pose)
} VosHand;

typedef struct {
    double time;                 // the frame's display time (seconds, CACurrentMediaTime's clock)
    int focused;                 // the space shows and takes input
    int head_ok;
    float head[16];              // LOCAL <- the head (the eyes' midpoint)
    float floor_y;               // the floor's height in LOCAL
    float ipd;                   // metres between the eyes
    int hz;                      // the display's rate (0: not known yet)
    int hands_ok;                // hand tracking runs
    VosHand hand[2];             // 0 left, 1 right
    VosController ctl[2];
} VosInput;

// ---- vos_shell.c (the compositor's thread)
int vos_shell_start(const char *files);           // the files folder; the game starts when the ROM set is there
int vos_shell_gl_init(int eye_w, int eye_h);      // GL context current; each eye's picture's size
void vos_shell_frame(const VosInput *in);         // a display frame's input and update, before the eyes
// one eye: LOCAL <- eye, tangents (left, right, up, down; left and down negative), into this GL texture (sRGB)
void vos_shell_eye(int e, const float *eye, const float tan[4], unsigned tex);
int vos_shell_exit(void);                         // 1: the app should close (QUIT, or the game ended)
void vos_shell_quit(void);                        // closing: the partner hears at once, the sound stops
void vos_shell_rom_chosen(int ok);                // the picker's answer (the set is in Documents/arcade when ok)

// ---- what the app gives the shell (vos_render.m)
struct AAssetManager;
struct AAssetManager *vos_assets(void);           // the bundle's assets (src/visionos/shim/android/asset_manager.h)
void vos_os_pick_rom(void);                       // open the document picker's window
void vos_os_haptic(int hand, float amplitude, int ms);
const char *vos_files_dir(void);                  // Documents

#ifdef __cplusplus
}
#endif
