// The VR shell's seam (docs/platforms.md, "src/xr"): xr_shell.c is the VR app (the game in a headset, the guns,
// the hands, the cabinet room, the VR menu), the same on every headset; its host is the part of the app that belongs
// to one OS: the Quest's NativeActivity with EGL (src/quest/quest_main.c), HOTDVR.exe's PCVR mode with WGL
// (src/xr/xr_win.c), or the Vision Pro's Compositor Services and ARKit (src/visionos/xr_vos.m). The host calls xr_*
// on its main thread (Vision Pro: the compositor's), in this order, and provides xr_os_*.
//
// XR_OPENXR (1 unless the build says 0): the shell runs the headset through OpenXR (the Quest, PCVR). With 0 (Vision
// Pro) the host has no OpenXR: it paces the frames, reads the head, hands and controllers into an XrhInput each
// display frame (xr_host_frame) and asks for each eye (xr_host_eye).
#pragma once
#include <stddef.h>
#ifndef XR_OPENXR
#define XR_OPENXR 1
#endif
#if XR_OPENXR
#include <openxr/openxr.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- the shell (xr_shell.c)
#if XR_OPENXR
// The OpenXR loader and instance, and a headset on it: 1, or 0 (no runtime, no headset: logged). The host's
// xr_os_loader_init and xr_os_instance_next first. Nothing else of the shell's runs before it.
int xr_instance(void);
const char *xr_runtime_name(void);      // "" before xr_instance
#endif
// The files folder, made and the current directory: env.txt, the settings (vr-options.cfg), crash reports
// (xr_os_crash_reports), voice chat, online play's start (xr_os_online_started)
void xr_setup(const char *files);
#if XR_OPENXR
// The GL context (xr_os_graphics_binding), the session, its spaces, the eye buffers and the controllers' actions; then the
// GL side (the programs, the models). 0 when one failed (logged).
int xr_session(void);
int xr_gl_init(void);
#endif
void xr_start(void);                    // the game thread, or (no ROM set) the setup page
#if XR_OPENXR
void xr_events(void);                   // OpenXR's events (the session's state)
int xr_running(void);                   // the session is running: xr_frame each turn of the host's loop
void xr_frame(void);                    // waits for the headset's next frame, draws it
#endif
int xr_should_exit(void);               // QUIT, the runtime ending the session, or the game over
int xr_restart_requested(void);         // online play begins or ends: the app opens again
#if XR_OPENXR
void xr_end_session(void);              // ask the runtime to end the session (the host then waits for it)
#endif
void xr_quit(void);                     // the partner told, the sound stopped (the process ends next)
void xr_menu_toggle(void);              // the VR menu open or shut (a key on the PC's mirror window)

#if !XR_OPENXR
// ---- a host without OpenXR (Vision Pro): its own frames, its input in these. Spaces: LOCAL is where the head
// started (at your eyes, as OpenXR's LOCAL); metres, +y up, -z forward; matrices column-major 4x4 (GL's).
typedef struct {                        // a controller (Vision Pro: a PlayStation VR2 Sense), laid out as Touch's
    int connected, pose_ok;
    float aim[16];                      // LOCAL <- its aim pose (-z along the ray)
    float trigger, squeeze;             // 0..1
    float stick_x, stick_y;             // up is +
    int fire;                           // the trigger clicked (with some hysteresis)
    int a, b, menu, stick_click, thumb; // A / X, B / Y, the menu button, the stick's click, the thumb resting
} XrhController;
typedef struct {                        // a tracked hand
    int tracked;
    float aim[16];                      // LOCAL <- the aim ray's pose: at the hand, -z along the ray
    int pinch;                          // thumb and index finger together (fires, clicks in the menu)
    float pinch_strength;               // 0..1 (the hand model's trigger finger)
    int middle_pinch;                   // thumb and middle finger together (the off hand: skip)
    int palm_to_face;                   // the palm turned to the head (the left's, with a pinch: the menu)
} XrhHand;
typedef struct {
    double time;                        // the frame's display time (seconds)
    int focused;                        // the app shows and takes input
    int head_ok;
    float head[16];                     // LOCAL <- the head (the eyes' midpoint)
    float floor_y;                      // the floor's height in LOCAL
    float ipd;                          // metres between the eyes
    int hz;                             // the display's rate (0: not known yet)
    int hands_ok;                       // hand tracking runs
    XrhHand hand[2];                    // 0 left, 1 right
    XrhController ctl[2];
} XrhInput;
// The GL context current, each eye's picture's size: the GL side (the programs, the models). 0 when it failed.
int xr_host_gl_init(int eye_w, int eye_h);
// A display frame: its input (kept until the next), the game's newest picture, the scene; then each eye, its pose
// (LOCAL <- eye) and field of view as tangents (left, right, up, down; left and down negative), drawn into the GL
// texture tex (eye_w x eye_h, sRGB)
void xr_host_frame(const XrhInput *in);
void xr_host_eye(int e, const float *eye, const float tan[4], unsigned tex);
// A host that can't start the app again (online play's start and end, a new ROM set): this page instead of the
// game, and a pull of the trigger (or the menu) closes the app (xr_should_exit)
void xr_close_with(const char *text);
// The host's ROM set import answered (ok: the set is in arcade/): 1 when the game started with it now (the setup
// page showed), 0 when it loads only when the app opens again (or none came)
int xr_rom_chosen(int ok);
// The setup page shows (no ROM set yet): its text, the way to load one, with `line` under it (iCloud's progress:
// Vision Pro's xr_vos.m); 0 when the game runs
int xr_rom_setup(const char *line);
#endif

// ---- the host (one per OS)
#if XR_OPENXR
void xr_os_loader_init(void);           // before the instance (Android: the loader's JavaVM and context)
const void *xr_os_instance_next(void);  // XrInstanceCreateInfo's next chain (Android's create info), or NULL
// The graphics API's requirements asked (the runtime insists on it), a GL context made current on this thread
// (Android: EGL with GL ES 3; Windows: WGL with GL 3.3), and the session's graphics binding for it (its next chain)
int xr_os_graphics_binding(XrInstance inst, XrSystemId sys, const void **binding);
#else
void xr_os_haptic(int hand, float amplitude, int ms);   // a controller's rumble (OpenXR's is the shell's own)
#endif
void xr_os_crash_reports(const char *files, const int *opt);   // a crash of the last run reported (the settings read)
void xr_os_online_started(void);        // this run is an online game (Android: Wi-Fi's multicast lock for LAN play)
int xr_os_import_rom(void);             // choose the ROM set (the app then starts again with it); 0: could not
// the app's updates (the SYSTEM tab): the status line (NULL: no updater), a command, the status polled
const char *xr_os_update_line(void);
void xr_os_update_cmd(const char *cmd);
void xr_os_update_poll(double now);
void xr_os_pc_switch(void);             // HOTDVR.exe: start again on the screen, and keep to it (PC only)
// The window the left eye is shown in (the PC's mirror), its size; 0: none. Its framebuffer is GL's 0.
int xr_os_mirror(int *w, int *h);

// the models and sounds of the VR app (src/quest/assets: Android's from the APK, the PC's from "assets" beside the exe,
// the Vision Pro's from the app bundle's)
typedef struct qasset qasset;
qasset *qasset_open(const char *name);  // NULL when there is none
int qasset_read(qasset *a, void *p, size_t n);   // 1 when all n bytes were read
const void *qasset_buffer(qasset *a, size_t *len);   // all of it in memory (NULL: could not)
void qasset_close(qasset *a);

#ifdef __cplusplus
}
#endif
