// HOTDVR.exe's two modes (docs/platforms.md, P4): flat (main_win.c, the window and the mouse) and VR
// (src/xr/xr_win.c, the VR shell in a headset through OpenXR). main_win.c's wWinMain picks one at start: --vr or
// --flat on the command line, else pc-mode.cfg in the files folder ("flat": the screen; "auto" or none: the headset
// when an OpenXR runtime has one, else the screen). What both use is here.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// main_win.c
// Windows' Open dialog for the ROM set, copied into files\arcade as hotd.zip or hotd.7z: 1 when it was
int win_import_rom(HWND owner, const wchar_t *files);
// This exe again: mode NULL, as it was started; else with its --vr / --flat taken out and mode (L"": none) put in
void win_restart(const wchar_t *mode);
void win_pc_mode_save(int flat);        // pc-mode.cfg: "flat", or "auto"

// src/xr/xr_win.c
int xr_win_probe(void);                 // openxr_loader.dll, a runtime and a headset on it: 1 (the OpenXR instance kept)
// the game in the headset (xr_win_probe first); ends the process
void xr_win_main(HINSTANCE inst, const wchar_t *files_w, const char *files);

#ifdef __cplusplus
}
#endif
