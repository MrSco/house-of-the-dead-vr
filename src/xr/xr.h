// The VR shell's seam (docs/platforms.md, "src/xr"): xr_shell.c is the OpenXR app (the game in a headset, the guns,
// the hands, the cabinet room, the VR menu), the same on every headset; its host is the part of the app that belongs
// to one OS: the Quest's NativeActivity with EGL (src/quest/quest_main.c) or HOTDVR.exe's PCVR mode with WGL
// (src/xr/xr_win.c). The host calls xr_* on its main thread, in this order, and provides xr_os_*.
#pragma once
#include <stddef.h>
#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the shell (xr_shell.c)
// The OpenXR loader and instance, and a headset on it: 1, or 0 (no runtime, no headset: logged). The host's
// xr_os_loader_init and xr_os_instance_next first. Nothing else of the shell's runs before it.
int xr_instance(void);
const char *xr_runtime_name(void);      // "" before xr_instance
// The files folder, made and the current directory: env.txt, the settings (vr-options.cfg), crash reports
// (xr_os_crash_reports), voice chat, online play's start (xr_os_online_started)
void xr_setup(const char *files);
// The GL context (xr_os_graphics_binding), the session, its spaces, the eye buffers and the controllers' actions; then the
// GL side (the programs, the models). 0 when one failed (logged).
int xr_session(void);
int xr_gl_init(void);
void xr_start(void);                    // the game thread, or (no ROM set) the setup page
void xr_events(void);                   // OpenXR's events (the session's state)
int xr_running(void);                   // the session is running: xr_frame each turn of the host's loop
void xr_frame(void);                    // waits for the headset's next frame, draws it
int xr_should_exit(void);               // QUIT, the runtime ending the session, or the game over
int xr_restart_requested(void);         // online play begins or ends: the app opens again
void xr_end_session(void);              // ask the runtime to end the session (the host then waits for it)
void xr_quit(void);                     // the partner told, the sound stopped (the process ends next)
void xr_menu_toggle(void);              // the VR menu open or shut (a key on the PC's mirror window)

// ---- the host (one per OS)
void xr_os_loader_init(void);           // before the instance (Android: the loader's JavaVM and context)
const void *xr_os_instance_next(void);  // XrInstanceCreateInfo's next chain (Android's create info), or NULL
// The graphics API's requirements asked (the runtime insists on it), a GL context made current on this thread
// (Android: EGL with GL ES 3; Windows: WGL with GL 3.3), and the session's graphics binding for it (its next chain)
int xr_os_graphics_binding(XrInstance inst, XrSystemId sys, const void **binding);
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

// the models and sounds of the VR app (src/quest/assets: Android's from the APK, the PC's from "assets" beside the exe)
typedef struct qasset qasset;
qasset *qasset_open(const char *name);  // NULL when there is none
int qasset_read(qasset *a, void *p, size_t n);   // 1 when all n bytes were read
const void *qasset_buffer(qasset *a, size_t *len);   // all of it in memory (NULL: could not)
void qasset_close(qasset *a);

#ifdef __cplusplus
}
#endif
