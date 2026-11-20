// The flat app's Windows front end (HOTDVR.exe): a window with an OpenGL 3.3 core context (WGL), the mouse as the
// gun (light guns such as Sinden, GunCon 3 and Gun4IR are mice to Windows), Space or Enter for coins and Start; game
// pads too (pad_win.c).
// Alt+Enter or F11: fullscreen (a borderless window over the monitor). The files folder is "files" beside the exe
// (--files DIR: another, e.g. two copies on one PC playing each other).
// In a VR headset instead (src/xr/xr_win.c) when an OpenXR runtime has one, unless pc-mode.cfg in the files folder
// says "flat" (the SYSTEM tab's PLAY ON THE SCREEN / PLAY IN VR write it); --vr and --flat override it.
//
// Test switches: flat_app.c's (env.txt in the files folder, or the environment).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flat/flat.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "quest_options.h"
#include "quest_menu.h"
#include "online.h"
#include "win_app.h"

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
void pad_poll(double now, double dt);   // pad_win.c
void pad_mouse_moved(void);

static struct {
    HWND wnd;
    HDC dc;
    HGLRC gl;
    int w, h, minimized, quit, fullscreen, trigger, import_restart, switch_vr;
    wchar_t files[MAX_PATH];
    WINDOWPLACEMENT placement;
} W;

static void fullscreen(int on) {
    if (on == W.fullscreen) return;
    W.fullscreen = on;   // (first: the setting below calls back here)
    if (flat_opt_get(QOPT_FULLSCREEN) != on) flat_opt_set(QOPT_FULLSCREEN, on);   // (kept for the next start)
    if (on) {
        W.placement.length = sizeof W.placement;
        GetWindowPlacement(W.wnd, &W.placement);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoW(MonitorFromWindow(W.wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(W.wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(W.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(W.wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(W.wnd, &W.placement);
        SetWindowPos(W.wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: W.quit = 1; return 0;
    case WM_SIZE:
        W.minimized = wp == SIZE_MINIMIZED;
        W.w = LOWORD(lp); W.h = HIWORD(lp);
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        const int x = (short)LOWORD(lp), y = (short)HIWORD(lp), down = (wp & MK_LBUTTON) != 0;
        if (msg == WM_LBUTTONDOWN) SetCapture(h);
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        static int last_x = -1, last_y = -1;
        if (x != last_x || y != last_y) { last_x = x; last_y = y; pad_mouse_moved(); }
        flat_menu_mouse(x, y, down);
        if (flat_menu_is_open()) { if (W.trigger) { W.trigger = 0; flat_trigger(0); } return 0; }
        flat_pointer(x, y);
        if (msg != WM_MOUSEMOVE && down != W.trigger) { W.trigger = down; flat_trigger(down); }
        return 0;
    }
    case WM_RBUTTONDOWN: if (!flat_menu_is_open()) flat_reload(1); return 0;
    case WM_RBUTTONUP: flat_reload(0); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            // (hidden over the picture where the game's own sight shows instead: Crosshair ON)
            const int hide = flat_pointer_in_view(p.x, p.y) && !flat_menu_is_open() && flat_opt_get(QOPT_CROSSHAIR);
            SetCursor(hide ? NULL : LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { fullscreen(!W.fullscreen); return 0; }
        break;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const int down = msg == WM_KEYDOWN, repeat = down && (lp & (1 << 30));
        if (down && wp == VK_F11) { fullscreen(!W.fullscreen); return 0; }
        if (flat_menu_is_open()) {
            if (!down) return 0;
            switch (wp) {
            case VK_UP: flat_menu_key(FLAT_KEY_UP); break;
            case VK_DOWN: flat_menu_key(FLAT_KEY_DOWN); break;
            case VK_LEFT: flat_menu_key(FLAT_KEY_LEFT); break;
            case VK_RIGHT: flat_menu_key(FLAT_KEY_RIGHT); break;
            case VK_RETURN: case VK_SPACE: if (!repeat) flat_menu_key(FLAT_KEY_ENTER); break;
            case VK_ESCAPE: if (!repeat) flat_menu_key(FLAT_KEY_BACK); break;
            case VK_TAB: flat_menu_key(GetKeyState(VK_SHIFT) < 0 ? FLAT_KEY_TAB_BACK : FLAT_KEY_TAB); break;
            }
            return 0;
        }
        if (wp == VK_SPACE || wp == VK_RETURN) { if (!repeat) flat_start(down); return 0; }
        if (wp == 'T') { online_talk_key(down != 0); return 0; }   // online, PUSH TO TALK
        if (down && !repeat && wp == VK_ESCAPE) { flat_menu_set_open(1); return 0; }
        break;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- flat.h's front-end calls
void flat_os_fullscreen(int on) { fullscreen(on); }
void flat_os_quit(void) { W.quit = 1; }
int flat_os_touch(void) { return 0; }
void crash_win_start(const char *files, const int *opt);   // crash_win.c
void flat_os_crash_reports(const char *files, const int *opt) { crash_win_start(files, opt); }
int flat_os_has_reports(void) { return 1; }
// the ROM set: chosen in Windows' Open dialog, copied into files/arcade as hotd.zip or hotd.7z; then the app starts
// again with it (the images made from the old one are made again: their stamp names the set)
void flat_os_import_rom(void) {
    if (W.fullscreen) fullscreen(0);
    if (win_import_rom(W.wnd, W.files)) W.import_restart = 1;
}
int win_import_rom(HWND owner, const wchar_t *files) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"MAME ROM set (hotd.zip, hotd.7z)\0*.zip;*.7z\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Your ROM set of The House of the Dead (MAME's hotd, Revision A)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    const wchar_t *ext = wcsrchr(path, L'.');
    const int is7z = ext && !_wcsicmp(ext, L".7z");
    wchar_t dst[MAX_PATH + 32], other[MAX_PATH + 32];
    swprintf(dst, MAX_PATH + 32, L"%s\\arcade\\%s", files, is7z ? L"hotd.7z" : L"hotd.zip");
    swprintf(other, MAX_PATH + 32, L"%s\\arcade\\%s", files, is7z ? L"hotd.zip" : L"hotd.7z");
    if (_wcsicmp(path, dst) && !CopyFileW(path, dst, FALSE)) {
        MessageBoxW(owner, L"The ROM set could not be copied into the files folder.", L"House of the Dead VR", MB_ICONERROR);
        return 0;
    }
    DeleteFileW(other);   // (the game takes hotd.zip first: only the one chosen stays)
    LOGI("ROM set: copied in; restarting");
    return 1;
}

// ---- VR or the screen (win_app.h)
static wchar_t exe_path[MAX_PATH], pc_mode_path[MAX_PATH + 32], start_dir[MAX_PATH];
void win_pc_mode_save(int flat) {
    FILE *f = _wfopen(pc_mode_path, L"w");
    if (f) { fputs(flat ? "flat\n" : "auto\n", f); fclose(f); }
}
static int pc_mode_flat(void) {
    char t[32] = "";
    FILE *f = _wfopen(pc_mode_path, L"r");
    if (f) { if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f); }
    return !strncmp(t, "flat", 4);
}
void win_restart(const wchar_t *mode) {
    static wchar_t cmd[32768];
    if (!mode) wcscpy_s(cmd, 32768, GetCommandLineW());
    else {   // the arguments again without --vr / --flat, then mode
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        swprintf(cmd, 32768, L"\"%s\"", exe_path);
        for (int i = 1; argv && i < argc; i++) {
            if (!wcscmp(argv[i], L"--vr") || !wcscmp(argv[i], L"--flat")) continue;
            wcscat_s(cmd, 32768, L" \"");
            wcscat_s(cmd, 32768, argv[i]);
            wcscat_s(cmd, 32768, L"\"");
        }
        if (*mode) { wcscat_s(cmd, 32768, L" "); wcscat_s(cmd, 32768, mode); }
        LocalFree(argv);
    }
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    // (from the folder it was started in: a relative --files is relative to that, not to the files folder)
    if (CreateProcessW(exe_path, cmd, NULL, NULL, FALSE, 0, NULL, start_dir[0] ? start_dir : NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOGE("restart failed (%lu)", (unsigned long)GetLastError());
    }
}
// PLAY IN VR (the SYSTEM tab): the app starts again in the headset, and does so from now on
void flat_os_pc_switch(void) {
    if (!xr_win_probe()) {
        MessageBoxW(W.wnd, L"No VR headset found.\n\nStart your headset's OpenXR runtime first (SteamVR, Quest Link or Air Link, "
                           L"Virtual Desktop) with the headset connected, then choose PLAY IN VR again.",
                    L"House of the Dead VR", MB_ICONINFORMATION);
        return;
    }
    win_pc_mode_save(0);
    W.switch_vr = 1;
    LOGI("PLAY IN VR: starting again in the headset");
}

static int gl_context(void) {
    W.dc = GetDC(W.wnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(W.dc, &pfd);
    if (!pf || !SetPixelFormat(W.dc, pf, &pfd)) { LOGE("no pixel format"); return 0; }
    HGLRC old = wglCreateContext(W.dc);
    if (!old || !wglMakeCurrent(W.dc, old)) { LOGE("no OpenGL"); return 0; }
    PFN_wglCreateContextAttribsARB create = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    W.gl = create ? create(W.dc, NULL, attrs) : NULL;
    if (!W.gl) { LOGE("no OpenGL 3.3 core context"); return 0; }
    wglMakeCurrent(W.dc, W.gl);
    wglDeleteContext(old);
    const char *missing = NULL;
    if (!plat_gl_load(&missing)) { LOGE("OpenGL: no %s", missing); return 0; }
    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(1);
    return 1;
}

static void utf8(const wchar_t *w, char *out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;
    // sharp on high-DPI screens (Windows 10 1703 and later)
    typedef BOOL (WINAPI *PFN_dpi)(HANDLE);
    PFN_dpi dpi = (PFN_dpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (dpi) dpi((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    // the files folder: "files" beside the exe, or --files DIR
    wchar_t exe[MAX_PATH], wfiles[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy_s(wfiles, MAX_PATH, exe);
    wchar_t *slash = wcsrchr(wfiles, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - wfiles), L"files");
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--files")) GetFullPathNameW(argv[++i], MAX_PATH, wfiles, NULL);
    CreateDirectoryW(wfiles, NULL);
    wcscpy_s(W.files, MAX_PATH, wfiles);
    wcscpy_s(exe_path, MAX_PATH, exe);
    GetCurrentDirectoryW(MAX_PATH, start_dir);
    swprintf(pc_mode_path, MAX_PATH + 32, L"%s\\pc-mode.cfg", wfiles);
    int want_vr = -1;   // --vr 1, --flat 0, else pc-mode.cfg
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--vr")) want_vr = 1;
        if (!wcscmp(argv[i], L"--flat")) want_vr = 0;
    }
    // port.log: everything the app and the game log (stderr)
    wchar_t wlog[MAX_PATH + 16];
    swprintf(wlog, MAX_PATH + 16, L"%s\\port.log", wfiles);
    _wfreopen(wlog, L"w", stderr);
    _wfreopen(wlog, L"a", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    char files[MAX_PATH * 3];
    utf8(wfiles, files, sizeof files);
    timeBeginPeriod(1);

    {   // VR or the screen. VR needs the ROM set first: without it the screen asks for it, then starts again
        wchar_t z[MAX_PATH + 32], s7[MAX_PATH + 32];
        swprintf(z, MAX_PATH + 32, L"%s\\arcade\\hotd.zip", wfiles);
        swprintf(s7, MAX_PATH + 32, L"%s\\arcade\\hotd.7z", wfiles);
        const int rom = GetFileAttributesW(z) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(s7) != INVALID_FILE_ATTRIBUTES;
        const int try_vr = want_vr == 1 || (want_vr < 0 && !pc_mode_flat());
        LOGI("mode: %s (%s)%s", try_vr ? "VR if a headset is found" : "flat", want_vr == 1 ? "--vr" : want_vr == 0 ? "--flat" : "pc-mode.cfg",
             rom ? "" : "; no ROM set yet: flat first");
        if (try_vr && rom) {
            if (xr_win_probe()) xr_win_main(inst, wfiles, files);   // (never returns)
            if (want_vr == 1)
                MessageBoxW(NULL, L"No VR headset found (--vr): the game starts on the screen.\n\nStart your headset's OpenXR runtime "
                                  L"(SteamVR, Quest Link or Air Link, Virtual Desktop) with the headset connected, then start the game again.",
                            L"House of the Dead VR", MB_ICONINFORMATION);
        }
    }

    if (!flat_app_start(files)) {
        MessageBoxW(NULL, L"The game could not start: see port.log in the files folder.", L"House of the Dead VR", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"hotdvr";
    RegisterClassW(&wc);
    // 1280 x 960 of picture (4:3), smaller on a small screen
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int cw = 1280, ch = 960;
    while ((ch > (work.bottom - work.top) * 9 / 10 || cw > (work.right - work.left) * 9 / 10) && cw > 640) { cw -= 160; ch -= 120; }
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    W.wnd = CreateWindowExW(0, L"hotdvr", L"House of the Dead VR", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!W.wnd || !gl_context() || !flat_gl_init()) {
        MessageBoxW(NULL, L"OpenGL 3.3 is needed: see port.log in the files folder.", L"House of the Dead VR", MB_ICONERROR);
        flat_app_quit();
        return 1;
    }
    const int hidden = getenv("FLAT_TEST_HIDDEN") && atoi(getenv("FLAT_TEST_HIDDEN"));   // (tests: never shown)
    if (!hidden) ShowWindow(W.wnd, show);
    GetClientRect(W.wnd, &r);
    W.w = r.right; W.h = r.bottom;

    if (!hidden && flat_opt_get(QOPT_FULLSCREEN)) { W.fullscreen = 0; fullscreen(1); }
    // no ROM set yet: ask for it at once
    {
        wchar_t z[MAX_PATH + 32], s7[MAX_PATH + 32];
        swprintf(z, MAX_PATH + 32, L"%s\\arcade\\hotd.zip", wfiles);
        swprintf(s7, MAX_PATH + 32, L"%s\\arcade\\hotd.7z", wfiles);
        if (GetFileAttributesW(z) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(s7) == INVALID_FILE_ATTRIBUTES && !getenv("FLAT_NO_ROM_DIALOG")) {
            MessageBoxW(W.wnd, L"This app holds none of The House of the Dead: it plays the 1996 arcade game from your own MAME ROM set "
                               L"(hotd, Revision A: hotd.zip or hotd.7z).\n\nChoose it in the next window. It is copied into the files "
                               L"folder beside HOTDVR.exe.", L"Load your ROM set", MB_ICONINFORMATION);
            flat_os_import_rom();
        }
    }
    while (!W.quit && !flat_app_restart_requested() && !W.import_restart && !W.switch_vr) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) W.quit = 1;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        {
            static double last;
            const double now = (double)plat_now_ns() * 1e-9;
            pad_poll(now, last ? now - last : 0);
            last = now;
        }
        flat_app_frame(W.minimized);
        if (W.minimized || !W.w || !W.h) { Sleep(16); continue; }
        flat_draw(W.w, W.h, 0);
        SwapBuffers(W.dc);
    }
    const int restart = flat_app_restart_requested() || W.import_restart;
    flat_app_quit();
    if (restart) win_restart(NULL);   // online play begins (or ends): this program again, as it was started
    else if (W.switch_vr) win_restart(L"");   // PLAY IN VR (pc-mode.cfg says so now)
    fflush(stderr);
    ExitProcess(0);
}
