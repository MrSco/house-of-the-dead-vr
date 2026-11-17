# The House of the Dead VR on every platform

Plan (2026-11-16); P0 under way on the `multi-platform` branch (see "Progress"). It follows GoldenEye VR's platform work
(goldeneye-vr `docs/pc-platforms.md`, `docs/mobile-platforms.md`): one codebase, one CMake platform switch, a "no
headset" flat shell beside the VR one, and online play at parity across all of them.

## Goals (user, 2026-11-16)

- **Platforms:** Quest (now), Windows, Android phones and tablets, iOS / iPadOS, Steam Frame, and later Vision Pro.
- **Mode by device:** a headset plays the VR game, in the **immersive view by default** (as on Quest now), with the
  cabinet view a menu or stick-click away. A screen plays **flat mode**: the cabinet view's screen, which is the arcade
  picture full-window, as GoldenEye VR's flat builds play its virtual screen. **Immersive is the default wherever it
  is available:** a PC starts in VR when an OpenXR runtime reports a headset, and in flat mode otherwise (`--flat` /
  `--vr` and a menu row override it).
- **Parity:** the same game, settings and online play everywhere, and **every platform can play online with every
  other** (rooms, quick match, LAN where the OS allows it, voice).

## The one thing that decides crossplay, and why it already works

Online play is a deterministic lockstep (`src/quest/arcade_net.cpp`): the host sends each frame's cabinet inputs and
both sides run the same frame, compared every 60 frames by `M2Board::state_hash` (FNV-1a over main and work RAM).
Crossplay therefore needs the recompiled game to give bit-identical RAM on every CPU and compiler.

- **Already shown across architectures:** `tools/m2netguest` (Windows x64) joined a Quest (arm64) over the LAN and
  matched 500 of 500 checksums (docs/vr.md, "Online play and voice chat").
- **What makes it hold:** no fast-math and no FP contraction (`-ffp-contract=off`; MSVC `/fp:strict`), SoftFloat for
  the i960's extended precision. Apple clang (iOS, visionOS) needs the same flags; the CMake `else()` branch already
  gives them, so nothing new, only a test.
- **The trap:** VR mode's scenery hook (`hotd_scenery_cull`, `M2Board::set_vr`) changes the wait loop's spare-time
  counters (0x51f9a4, 0x51e530..0x51e66f), so a VR player and a non-VR player disagree on the checksum (docs/vr.md
  says so). **Decision proposed: every build runs with the hook on, flat included.** The flat picture is unchanged
  (the game's own view is pixel-identical with it), it costs +0.02 ms a frame, and it keeps the RAM the same. The
  alternative, hashing around those bytes, works too but hides real desyncs in that range.
- **Guard it:** a determinism check per platform: `m2nettest` records bundles once, each platform's build replays
  them and must match all checksums (x64 MSVC, arm64 Android NDK, arm64 Apple clang). Run it before every release.
- **Protocol:** the bundle layout stays. HELLO gets the platform and input class (VR / mouse / touch / pad) for a
  platform icon in the room list, and a protocol version so mismatched builds refuse cleanly instead of desyncing.
  **Anyone joins anyone:** no VR-only rooms.

## How the code splits

Today `src/quest` is the whole app: the game bridge (`arcade_quest.cpp`, 1,152 lines), the 3D renderer
(`m2_gl.c`, GLES 3.0), OpenXR and the VR shell (`quest_xr.c`, 2,935 lines), the menu on a CPU canvas
(`quest_menu.c`, `quest_canvas.c`), voice (`voice.c`, AAudio + JNI), crash reports (`quest_report.c`, HTTP over
JNI), and a small SDL2 shim (`quest_sdl.c`). `src/net` (lobby, rooms, LAN host) is plain C and portable already.
The upstream desktop app in `src/app` (SDL3, SDL_GPU, ImGui) is Daytona's and shows the 2D software frame; it is
a reference, not the base.

Proposed layout (P0 does this with the Quest build unchanged):

| Directory | What | Platforms |
|---|---|---|
| `src/core` | The game bridge (`arcade_quest.cpp` without Android), `arcade_net`, `m2_gl.c`, the menu and canvas, options, voice DSP | all |
| `src/platform/platform.h` | The seam: files dir, log, ROM picker, HTTP POST, mic open/permission, haptics, "restart the app" | all |
| `src/xr` | OpenXR VR shell from `quest_xr.c`, split into the shared OpenXR code and a graphics binding (GLES on Android, GL on Windows) | Quest, Steam Frame, PCVR |
| `src/flat` | The flat shell: the game-camera draw, the HUD overlay, the menu as a 2D panel, the app round the game; a front end per OS (`win/`) for the window and mouse / touch / pad / gyro | Windows, Android, iOS, macOS |
| `src/platform/{android,windows,apple}` | The bodies of `platform.h` | per OS |
| `src/visionos` | Compositor Services + ARKit instead of OpenXR | Vision Pro |

The game side uses about 30 SDL2 calls (window, renderer, texture, events, audio); on desktop and mobile the real SDL2
replaces the shim, as m2netguest's `sdl_mini.cpp` shows for its eight.

## Rendering

- **One renderer, GLES 3.0:** `m2_gl.c` everywhere. Quest and Android natively; Windows through GL 3.3 core (the
  shader header swapped, as GoldenEye's PC build does); iOS and visionOS through ANGLE on Metal, which GoldenEye
  already proved on an iPhone and in its visionOS work (ANGLE built once in `~/angle`). ANGLE's Metal backend has no
  multiview, so visionOS draws one pass per eye.
- **Flat picture, two choices in GRAPHICS** (HD the default, decided 2026-11-16):
  - **HD** (default): `m2_gl` draws the game camera's view at the window's resolution, the 2D layers (HUD, text,
    2D screens) over it scaled up. 4:3, pillarboxed.
  - **Original:** the arcade's 496 x 384 software frame (`qframe_take`), scaled with nearest or a CRT filter.
- **Widescreen later, not now.** The VR hook sends the whole area's scenery, so a 16:9 Hor+ view would have walls,
  but the game deletes enemies and effects that leave its 4:3 view (game logic, left alone), so they would pop at
  the edges. Worth an experiment after launch.

## Flat mode controls (a light gun game suits every screen)

| Device | Aim | Shoot | Reload | Coins / Start / skip | Menu |
|---|---|---|---|---|---|
| Mouse + keyboard | The pointer | Left button | Right button, or off-screen shot | Space / Enter | Esc |
| Touch | Where you tap | The tap | Two-finger tap, or a RELOAD button | On-screen START | On-screen pause |
| Gamepad | Stick moves a crosshair; gyro aim on pads that have it (DualSense, Switch Pro) | Right trigger | A button, or flick the pad down with gyro | Start | Back / Options |
| Light guns (Sinden, GunCon 3, Gun4IR) | They act as a mouse on Windows, so they work as the mouse row | | | | |

Touch is the natural fit: a tap is a light-gun shot at that point. Gyro aim on phones (Off / While held / Always)
as GoldenEye's iOS build has it. The menu is the same tabs as the VR one, drawn on the same canvas, pointed at by
mouse or touch; VR-only rows (hands, cabinet room, comfort, physical reload) are hidden in flat mode.

## Per platform

| Platform | Package | Mode | Graphics | XR | Input | ROM import | Notes |
|---|---|---|---|---|---|---|---|
| Quest 3 / 3S | APK (`com.mrsco.hotdvr`) | VR | GLES 3.0 + multiview | OpenXR, Meta loader | Touch controllers, hands | `ImportActivity` (SAF) | Shipping now |
| Steam Frame | APK flavor `frame`, run in Valve's Lepton | VR | as Quest | OpenXR, **Khronos** Android loader | Frame controllers, hands if offered | as Quest | GoldenEye's `frame` flavor is the recipe; Meta's loader only finds Meta's runtime. Built, then tested when a Frame is at hand. The APK only for now, no native SteamOS build |
| Android phone / tablet | APK flavor `mobile` | Flat | GLES 3.0, no multiview | none | Touch, gyro, pads | as Quest | Landscape; 16 KB page alignment |
| Windows | One zip: `HOTDVR.exe` | VR (immersive) when a headset is found, else flat; `--flat` / `--vr` force one | GL 3.3 | OpenXR, any runtime (SteamVR, Link, Virtual Desktop) | Mouse, pads, light guns; VR controllers | Open dialog, or drop the zip on the window | Flat first, PCVR second |
| iOS / iPadOS | IPA (Xcode project from xcodegen), installed with SideStore | Flat | ANGLE on Metal | none | Touch, gyro, MFi / Xbox / PS pads, iPad mouse | Document picker; Files shows the data folder | Built on the Mac; LAN discovery needs Apple's multicast entitlement, so iOS uses rooms and direct IP |
| Vision Pro | visionOS app | VR | ANGLE on Metal, one pass per eye | Compositor Services + ARKit | Hand tracking (aim ray + pinch, as the Quest's hands), PSVR2 Sense controllers | Document picker | Later; reuse gevr-visionos-native's findings |
| macOS | App (zip on GitHub) | Flat | ANGLE on Metal | none | Mouse, pads, light guns | Open dialog | Right after iOS, sharing its Apple layer and ANGLE |

Linux is out of scope.

## What changes per OS service

| Service | Quest today | Windows | Android phone | Apple |
|---|---|---|---|---|
| Lobby, rooms, LAN host | `src/net` sockets | same | same | same (LAN: entitlement) |
| Voice capture | AAudio + JNI permission | SDL2 capture (WASAPI) | AAudio, as Quest | SDL2 CoreAudio, `AVAudioSession` playAndRecord, `NSMicrophoneUsageDescription` |
| Voice DSP (Opus, SpeexDSP, RNNoise) | `voice_dsp.c` | same | same | same |
| Crash reports | HTTP via JNI | WinHTTP | JNI, as Quest | NSURLSession |
| Files dir | external files dir | beside the exe or `%LOCALAPPDATA%` | external files dir | Documents |
| Restart to apply settings | `RestartActivity` | re-run the exe | as Quest | Not possible: say "reopen the app" |

## Phases

| # | What | Done when |
|---|---|---|
| P0 | Carve `src/core` / `src/platform` / `src/xr` / `src/flat` out of `src/quest`; the hook on in every build; the determinism replay as a test | The Quest APK plays and nets exactly as 0.3.0; a desktop target links the core |
| P1 | **Windows flat**: window, HD and Original pictures, mouse / pad / light gun, the menu as a panel, ROM by dialog, voice, crash reports, packaging | A Windows player joins a Quest by room code and plays a stage, voice both ways |
| P2 | **Android phone** flavor (touch, gyro) and the **Steam Frame** flavor (Khronos loader) | A phone plays online with a Quest and a PC; the Frame APK builds |
| P3 | **iOS / iPadOS** on ANGLE, installed with SideStore; then **macOS** on the same Apple layer | An iPhone and a Mac play online with a Quest |
| P4 | **PCVR** on Windows: `src/xr` with a GL binding, SteamVR's quirks from GoldenEye (quads, dashboard button) | A Quest over Link and a SteamVR headset play the cabinet and immersive views |
| P5 | **Vision Pro**: a spike first (Compositor Services, hands, one eye pass each), then the port | Plays online with the others |

Each phase ends with the crossplay check against every platform already done, and the determinism replay.

## Progress

**P0, done (2026-11-16):**
- `src/core` holds the shared code, moved from `src/quest`: the bridge (`arcade.cpp`, was `arcade_quest.cpp`),
  `arcade_net`, `m2_gl.c`, the menu, canvas and options, voice chat (`voice.c`, `voice_dsp.c`), the SDL2 subset the
  game thread runs on (`host_sdl.c`, was `src/quest/quest_sdl.c`), and the headers between the game thread and a
  shell (`quest.h`, `m2_vr.h`, `d3d_vr.h`). `netplay.h` moved to `src/net` beside `online.c`. File and function
  names keep their `quest_` prefixes for now; renaming them is a mechanical pass for later.
- `src/platform/platform.h` is the OS seam: log, clock, sleep-until, threads (with a stack size), static locks and
  condition variables, SDL's recursive mutex, files, env, sound out and the microphone. `posix.c` (Android, later
  Apple), `android/` (log, AAudio out, the microphone and its permission over JNI) and `windows/` (Win32, WASAPI)
  implement it; `gl.h` picks GL ES 3.0, or desktop GL 3.3 loaded at run time (`windows/gl_win.h`), and the GLSL
  version line. `src/core` includes no Android header.
- The Quest APK builds from the new layout; its library has 0.3.0's symbols plus the `plat_*` ones. On the Quest 3
  the game loads and runs, AAudio plays (a 25 s audio dump has the game's sound), voice chat starts.
- `m2nettest` runs with the VR scenery hook on, as the apps do. **`scripts/crossplay_check.py`** records a host run
  on each of the PC (x64, MSVC 14.51 `/fp:strict`, as HOTDVR.exe) and the Quest (arm64, NDK clang) and replays it
  on the other: 30 of 30 checksums matched both ways over 1,800 frames.

**P1, Windows flat (2026-11-16): done but for a player's check.** `python scripts/build_windows.py [--zip]` builds
`build_windows/HOTDVR.exe` (MSVC and Ninja in Visual Studio's environment; the newest Visual Studio, for C11
atomics) and `HOTDVR-<version>-windows.zip`.
- `src/flat` is the flat shell for every screen: `flat.c` draws the VR app's cabinet view at the window's size
  (HD, 4x MSAA) or at the arcade's 496 x 384 scaled up sharp (Original), 4:3 in the middle; `flat_app.c` runs the
  files folder, voice, online start-up and the game thread; `flat_menu.c` shows the shared menu as a panel (its flat
  layout: PICTURE, SYSTEM, ONLINE). `win/main_win.c` is the Windows front end (WGL 3.3 core, per-monitor DPI, mouse
  and keys, Alt+Enter / F11 full screen, the ROM set by an Open dialog at the first start), `win/pad_win.c` the
  XInput pads. Its window and input are Win32's own (the plan said GLFW: no dependency was needed).
- Online: HOTDVR.exe joined the Quest's LAN game by IP and played stage 1 by script: 120 of 120 checksums over
  7,200 frames, voice both ways (about 3,000 Opus frames each way, none lost). Two copies on the PC by room code on
  zonesync.net: 60 of 60; over the LAN: 100 of 100. `online.c` had voice stubbed off on every build but Android;
  it is on wherever `HOTD_VOICE` is defined.
- Test switches for this (env.txt): `FLAT_SHOT`, `FLAT_QUIT_AT`, `FLAT_MENU_AT`, `FLAT_TEST_ONLINE`,
  `FLAT_TEST_PLAY`, `FLAT_TEST_TALK`; on the Quest `QUEST_TEST_ONLINE` gained `hostlan` and `joinip:`, and
  `QUEST_TEST_TALK` holds push to talk.

**P2, Android phones and the Steam Frame (2026-11-16): done but for devices.** `python scripts/build_quest.py
--flavor quest|frame|phone` (the Quest app stays the default).
- **phone** (`com.mrsco.hotdvr.android`, `hotdvr-android.apk`): `src/flat` with an Android front end
  (`src/flat/android/main_android.c`: NativeActivity, EGL / GL ES 3.0, landscape). Touch: a tap on the picture
  shoots there, a second finger reloads; `src/flat/flat_touch.c` puts RELOAD, START and MENU in the bars beside the
  4:3 picture (faint over its corners on a 4:3 screen); Back opens the menu (its touch layout: no Display row).
  Pads: either stick moves a sight, R2 / R1 shoot, L2 / L1 / X reload, A / Start coins, Select the menu. The ROM set
  comes through the Quest app's ImportActivity, the restarts through its RestartActivity
  (`src/platform/android/activity.c` starts either). arm64 only, Android 10 and later.
  Checked in the Android 14 x86_64 emulator (arm64 through its translation, the dev PC's `hotd_test` AVD): the game
  runs (board ~10 ms a frame translated), the touch buttons give coins and Start and start a game, the menu opens and
  RESUME closes it, taps aim; it joined HOTDVR.exe's LAN game at 10.0.2.2: 80 of 80 checksums over 4,800 frames.
  The dev phone at hand (moto g pure) has a 32-bit userspace and can't install it.
- **frame** (`com.mrsco.hotdvr.frame`, `hotdvr-frame.apk`): the Quest app's library as it is, with the Quest's
  manifest minus Meta's features (hand tracking, passthrough, supported devices, the Oculus VR category) and head
  tracking not required, as GoldenEye VR's frame flavor. The Quest build already had what Lepton needs: the
  Khronos OpenXR loader (not Meta's), OpenXR 1.0, the graphics requirements asked before the session, every Meta
  extension optional, Touch controllers bound. Built, untested: no Frame at hand.

**Left for a player (the end check):** on the headset, a stage and an online game with this build; on Windows, the
mouse, a pad and a light gun by hand, the menu's clicks, full screen, the ROM dialog, and voice with real speech;
a real phone (touch, a pad, the ROM picker, voice); a Steam Frame.

**Left, later:**
- Crash reports on Windows (WinHTTP; the SYSTEM tab hides the row on flat builds until then).
- `src/xr`: the OpenXR shell (`quest_xr.c`, guns, hands, cabinet room) leaves `src/quest` when PCVR (P4) needs it.
- The crossplay check with clang-cl too (CI's compiler; not installed on the dev PC).
- The Quest's `QUEST_TEST_ONLINE` deletes all of env.txt before its restart, so other switches don't survive it;
  the flat hook drops only its own line.
- Phones: gyro aim; the navigation bar (the theme hides the status bar; immersive mode needs the UI thread); a
  32-bit (armeabi-v7a) build for phones without a 64-bit userspace, if wanted; LAN discovery (the Quest takes a
  Wi-Fi multicast lock through JNI; the phone does not yet: joining by IP and by room code work).

## Risks

- **Desync from a compiler or library change.** Guarded by the replay test; a desync shows as a checksum mismatch
  within a second of play, already reported in the log.
- **Mouse players outscore VR players.** It is co-op, so this matters less than in GoldenEye's deathmatch; rooms
  stay open to everyone.
- **Distribution is sideload only, as GoldenEye VR:** APKs (Quest, Frame, phone) and the Windows and macOS zips on
  GitHub releases, iOS through **SideStore**. No Play, App Store or Steam: they review emulator-like apps and SEGA's
  trademark is in the name. SideStore's free Apple ID signing means a 7-day refresh, and no multicast entitlement,
  so iOS finds games by rooms and direct IP, not LAN discovery.
- **Mobile backgrounding** drops sockets within seconds: treat it as the partner leaving (late joining exists).
- **iOS Restart:** an iOS app cannot relaunch itself; settings that need a restart say so.

## Decisions (user, 2026-11-16)

1. **Order:** as proposed (Windows flat, Android phone + Frame, iOS then macOS, PCVR, Vision Pro). Windows flat
   comes first because it is the quickest second platform for crossplay testing against the Quest; PCVR reuses the
   Quest's VR shell, so it waits until the split has settled.
2. **Default mode:** immersive wherever VR is available (Quest, Frame, Vision Pro, a PC with a headset); flat
   elsewhere. The flat picture defaults to **HD**, with Original in GRAPHICS.
3. **Crossplay:** anyone joins anyone, a platform icon in the room list, no VR-only option.
4. **Steam Frame:** the APK in Lepton for now; a native SteamOS build is not planned.
5. **macOS:** in scope, after iOS. **Linux:** dropped.
6. **Distribution:** sideload only, as GoldenEye VR: GitHub releases, and SideStore for iOS.
