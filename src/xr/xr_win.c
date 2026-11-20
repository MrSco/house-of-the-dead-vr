// PCVR (docs/platforms.md, P4): HOTDVR.exe in a VR headset, the VR shell (xr_shell.c) hosted on Windows (xr.h):
// OpenXR through Khronos's loader (openxr_loader.dll beside the exe, delay-loaded: the flat app runs without it), on
// whatever runtime is active (SteamVR, Meta's for Quest Link, Virtual Desktop's), with desktop OpenGL (a WGL context
// on a window that mirrors the left eye) through XR_KHR_opengl_enable. main_win.c picks VR or flat at start
// (win_app.h); the files folder, settings, voice, online play and crash reports are the flat app's, the VR settings
// in vr-options.cfg as on the Quest.
//
// The mirror window: Esc opens and shuts the VR menu; closing it quits.
// Test switches (env.txt, besides the shell's QUEST_* and XR_SHOT): XR_QUIT_AT=seconds quits then.
#include "platform/gl.h"
#include <unknwn.h>          // (IUnknown, for openxr_platform.h: WIN32_LEAN_AND_MEAN leaves it out)
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xr.h"
#include "quest.h"
#include "platform/platform.h"
#include "flat/win/win_app.h"

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);

static struct {
    HINSTANCE inst;
    HWND wnd;
    HDC dc;
    HGLRC gl;
    int w, h, quit, restart, switch_flat, import_restart;
    wchar_t files_w[MAX_PATH];
    XrGraphicsBindingOpenGLWin32KHR binding;
} X;

// ---------------------------------------------------------------- the runtime
// The loader DLL beside the exe (delay-loaded: none, no VR), then a runtime with a headset on it
int xr_win_probe(void) {
    static int loaded = -1;
    if (loaded < 0) {
        loaded = LoadLibraryW(L"openxr_loader.dll") != NULL;
        if (!loaded) LOGI("PCVR: no openxr_loader.dll beside the exe");
    }
    if (!loaded) return 0;
    const int ok = xr_instance();
    LOGI("PCVR: %s%s%s", ok ? "a headset on " : "no headset", ok ? xr_runtime_name() : "", ok ? "" : " (or no OpenXR runtime): flat");
    return ok;
}

// ---------------------------------------------------------------- xr.h's host
void xr_os_loader_init(void) {}
const void *xr_os_instance_next(void) { return NULL; }

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: X.quit = 1; return 0;
    case WM_SIZE: X.w = LOWORD(lp); X.h = HIWORD(lp); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !(lp & (1 << 30))) xr_menu_toggle();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// the mirror window and its GL: 3.3, compatibility profile (the VR shell's GL ES 3.0 code draws some things from
// client memory, which a core profile refuses)
static int gl_context(void) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = X.inst;
    wc.hIcon = LoadIconW(X.inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"hotdvr-xr";
    RegisterClassW(&wc);
    RECT r = { 0, 0, 960, 540 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    X.wnd = CreateWindowExW(0, L"hotdvr-xr", L"House of the Dead VR - in your headset (Esc: the VR menu)", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, X.inst, NULL);
    if (!X.wnd) { LOGE("PCVR: no window"); return 0; }
    if (!(getenv("FLAT_TEST_HIDDEN") && atoi(getenv("FLAT_TEST_HIDDEN")))) ShowWindow(X.wnd, SW_SHOWNOACTIVATE);
    GetClientRect(X.wnd, &r);
    X.w = r.right; X.h = r.bottom;
    X.dc = GetDC(X.wnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(X.dc, &pfd);
    if (!pf || !SetPixelFormat(X.dc, pf, &pfd)) { LOGE("PCVR: no pixel format"); return 0; }
    HGLRC old = wglCreateContext(X.dc);
    if (!old || !wglMakeCurrent(X.dc, old)) { LOGE("PCVR: no OpenGL"); return 0; }
    PFN_wglCreateContextAttribsARB create = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0 };
    X.gl = create ? create(X.dc, NULL, attrs) : NULL;
    if (!X.gl) { LOGE("PCVR: no OpenGL 3.3 context"); return 0; }
    wglMakeCurrent(X.dc, X.gl);
    wglDeleteContext(old);
    const char *missing = NULL;
    if (!plat_gl_load(&missing)) { LOGE("PCVR: OpenGL has no %s", missing); return 0; }
    // the mirror never waits for the monitor: the headset paces the frames (xrWaitFrame)
    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(0);
    return 1;
}
int xr_os_graphics_binding(XrInstance inst, XrSystemId sys, const void **binding) {
    PFN_xrGetOpenGLGraphicsRequirementsKHR greq = NULL;
    xrGetInstanceProcAddr(inst, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&greq);
    XrGraphicsRequirementsOpenGLKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!greq || XR_FAILED(greq(inst, sys, &gr))) { LOGE("PCVR: xrGetOpenGLGraphicsRequirementsKHR failed"); return 0; }
    LOGI("PCVR: the runtime wants OpenGL %u.%u to %u.%u", XR_VERSION_MAJOR(gr.minApiVersionSupported), XR_VERSION_MINOR(gr.minApiVersionSupported),
         XR_VERSION_MAJOR(gr.maxApiVersionSupported), XR_VERSION_MINOR(gr.maxApiVersionSupported));
    if (!gl_context()) return 0;
    X.binding = (XrGraphicsBindingOpenGLWin32KHR){ XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    X.binding.hDC = X.dc;
    X.binding.hGLRC = X.gl;
    *binding = &X.binding;
    return 1;
}
void crash_win_start(const char *files, const int *opt);   // platform/windows/crash_win.c
void xr_os_crash_reports(const char *files, const int *opt) { crash_win_start(files, opt); }
void xr_os_online_started(void) {}
int xr_os_import_rom(void) {
    if (!win_import_rom(X.wnd, X.files_w)) return 0;
    X.import_restart = 1;   // (the app starts again with it)
    return 1;
}
const char *xr_os_update_line(void) { return NULL; }
void xr_os_update_cmd(const char *cmd) { (void)cmd; }
void xr_os_update_poll(double now) { (void)now; }
void xr_os_pc_switch(void) {
    win_pc_mode_save(1);
    X.switch_flat = 1;
    LOGI("PCVR: PLAY ON THE SCREEN: starting again flat");
}
int xr_os_mirror(int *w, int *h) {
    if (!X.wnd || IsIconic(X.wnd)) return 0;
    *w = X.w; *h = X.h;
    return 1;
}

// the VR app's models and sounds: "assets" beside the exe (build_windows.py copies src/quest/assets there)
struct qasset { uint8_t *data; size_t len, pos; };
qasset *qasset_open(const char *name) {
    wchar_t exe[MAX_PATH], path[MAX_PATH + 64], wname[64];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    swprintf(path, MAX_PATH + 64, L"%sassets\\%s", exe, wname);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    qasset *a = calloc(1, sizeof *a);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (a && n > 0 && (a->data = malloc((size_t)n)) && fread(a->data, 1, (size_t)n, f) == (size_t)n) a->len = (size_t)n;
    else if (a) { free(a->data); free(a); a = NULL; }
    fclose(f);
    return a;
}
int qasset_read(qasset *a, void *p, size_t n) {
    if (a->len - a->pos < n) return 0;
    memcpy(p, a->data + a->pos, n);
    a->pos += n;
    return 1;
}
const void *qasset_buffer(qasset *a, size_t *len) { *len = a->len; return a->data; }
void qasset_close(qasset *a) { if (a) { free(a->data); free(a); } }

// ---------------------------------------------------------------- the app
// After xr_win_probe found a headset: runs the game in it until it ends. Ends the process.
void xr_win_main(HINSTANCE inst, const wchar_t *files_w, const char *files) {
    X.inst = inst;
    wcscpy_s(X.files_w, MAX_PATH, files_w);
    plat_mkdir(files);
    if (plat_chdir(files)) LOGE("cannot go into %s", files);   // port.log, the test switches' files land here
    xr_setup(files);
    if (!xr_session() || !xr_gl_init()) {
        LOGE("PCVR: the session could not be made: flat instead");
        MessageBoxW(NULL, L"The VR headset could not be started (port.log in the files folder says why).\n\nThe game starts on the screen instead.",
                    L"House of the Dead VR", MB_ICONWARNING);
        xr_quit();
        win_restart(L"--flat");
        ExitProcess(0);
    }
    xr_start();
    double quit_at = -1;
    if (getenv("XR_QUIT_AT") && *getenv("XR_QUIT_AT")) quit_at = atof(getenv("XR_QUIT_AT"));
    const uint64_t t0 = plat_now_ns();
    int finishing = 0;
    uint64_t finish_t = 0;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) X.quit = 1;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (quit_at >= 0 && (double)(plat_now_ns() - t0) / 1e9 >= quit_at) { LOGI("XR_QUIT_AT: quitting"); quit_at = -1; X.quit = 1; }
        xr_events();
        quest_audio_service();
        if (xr_restart_requested()) X.restart = 1;
        if (!finishing && (X.quit || X.restart || X.switch_flat || X.import_restart || xr_should_exit())) {
            LOGI("finishing");
            finishing = 1;
            finish_t = plat_now_ns();
            xr_end_session();
        }
        // (the runtime ends the session: stopping, then exiting; a runtime that never does gets 3 s)
        if (finishing && (!xr_running() || plat_now_ns() - finish_t > 3000000000ULL)) break;
        if (xr_running()) {
            xr_frame();
            if (!IsIconic(X.wnd)) SwapBuffers(X.dc);
        } else {
            Sleep(10);   // (the session not running yet, or the headset asleep)
        }
    }
    LOGI("PCVR: exiting");
    xr_quit();
    if (X.restart || X.import_restart) win_restart(NULL);      // online play begins or ends, a new ROM set
    else if (X.switch_flat) win_restart(L"");                // on the screen (pc-mode.cfg says so now)
    fflush(stderr);
    ExitProcess(0);
}
