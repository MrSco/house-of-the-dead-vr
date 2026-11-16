# The House of the Dead VR on every platform

Plan only (2026-11-16); no code changes yet. It follows GoldenEye VR's platform work
(goldeneye-vr `docs/pc-platforms.md`, `docs/mobile-platforms.md`): one codebase, one CMake platform switch, a "no
headset" flat shell beside the VR one, and online play at parity across all of them.

## Goals (user, 2026-11-16)

- **Platforms:** Quest (now), Windows, Android phones and tablets, iOS / iPadOS, Steam Frame, and later Vision Pro.
- **Mode by device:** a headset plays the VR game (cabinet and immersive, as on Quest). A screen plays **flat mode**:
  the cabinet view's screen, which is the arcade picture full-window, as GoldenEye VR's flat builds play its virtual
  screen. A device that can do both (a PC with a headset) picks at start.
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
- **Protocol:** the bundle layout stays. HELLO gets the platform and input class (VR / mouse / touch / pad) for the
  lobby listing and a "VR players only" host option if wanted (open question 3), and a protocol version so mismatched
  builds refuse cleanly instead of desyncing.

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
| `src/flat` | The flat shell: a window, the game-camera draw, the HUD overlay, the menu as a 2D panel, mouse / touch / pad / gyro to the gun | Windows, Android, iOS, (macOS) |
| `src/platform/{android,windows,apple}` | The bodies of `platform.h` | per OS |
| `src/visionos` | Compositor Services + ARKit instead of OpenXR | Vision Pro |

The game side uses about 30 SDL2 calls (window, renderer, texture, events, audio); on desktop and mobile the real SDL2
replaces the shim, as m2netguest's `sdl_mini.cpp` shows for its eight.

## Rendering

- **One renderer, GLES 3.0:** `m2_gl.c` everywhere. Quest and Android natively; Windows through GL 3.3 core (the
  shader header swapped, as GoldenEye's PC build does); iOS and visionOS through ANGLE on Metal, which GoldenEye
  already proved on an iPhone and in its visionOS work (ANGLE built once in `~/angle`). ANGLE's Metal backend has no
  multiview, so visionOS draws one pass per eye.
- **Flat picture, two choices in GRAPHICS:**
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
| Steam Frame | APK flavor `frame`, run in Valve's Lepton | VR | as Quest | OpenXR, **Khronos** Android loader | Frame controllers, hands if offered | as Quest | GoldenEye's `frame` flavor is the recipe; Meta's loader only finds Meta's runtime. Built, then tested when a Frame is at hand |
| Android phone / tablet | APK flavor `mobile` | Flat | GLES 3.0, no multiview | none | Touch, gyro, pads | as Quest | Landscape; 16 KB page alignment |
| Windows | One zip: `HOTDVR.exe` | Flat, or VR with `--vr` / a launcher choice | GL 3.3 | OpenXR, any runtime (SteamVR, Link, Virtual Desktop) | Mouse, pads, light guns; VR controllers | Open dialog, or drop the zip on the window | Flat first, PCVR second |
| iOS / iPadOS | IPA (Xcode project from xcodegen) | Flat | ANGLE on Metal | none | Touch, gyro, MFi / Xbox / PS pads, iPad mouse | Document picker; Files shows the data folder | Built on the Mac; LAN discovery needs Apple's multicast entitlement, so iOS uses rooms and direct IP |
| Vision Pro | visionOS app | VR | ANGLE on Metal, one pass per eye | Compositor Services + ARKit | Hand tracking (aim ray + pinch, as the Quest's hands), PSVR2 Sense controllers | Document picker | Later; reuse gevr-visionos-native's findings |
| macOS (optional) | App | Flat | ANGLE on Metal | none | Mouse, pads | Open dialog | Nearly free after iOS; not asked for |

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
| P3 | **iOS / iPadOS** on ANGLE | An iPhone plays online with a Quest |
| P4 | **PCVR** on Windows: `src/xr` with a GL binding, SteamVR's quirks from GoldenEye (quads, dashboard button) | A Quest over Link and a SteamVR headset play the cabinet and immersive views |
| P5 | **Vision Pro**: a spike first (Compositor Services, hands, one eye pass each), then the port | Plays online with the others |

Each phase ends with the crossplay check against every platform already done, and the determinism replay.

## Risks

- **Desync from a compiler or library change.** Guarded by the replay test; a desync shows as a checksum mismatch
  within a second of play, already reported in the log.
- **Mouse players outscore VR players.** It is co-op, so this matters less than in GoldenEye's deathmatch; see open
  question 3.
- **App stores.** The ROM is the player's own (nothing of SEGA's ships), but Play and the App Store review emulator-
  like apps and SEGA's trademark in the name. Sideloading (APK, zip, AltStore / TestFlight-style ad hoc) is the plan;
  stores are a separate decision.
- **Mobile backgrounding** drops sockets within seconds: treat it as the partner leaving (late joining exists).
- **iOS Restart:** an iOS app cannot relaunch itself; settings that need a restart say so.

## Open questions for the user

1. **Order.** Proposed: Windows flat, Android phone + Frame, iOS, PCVR, Vision Pro. Should PCVR come earlier (it is
   the same Windows exe)?
2. **Flat picture default:** HD (sharp 3D at window resolution) or Original (the arcade's pixels)?
3. **Crossplay rules:** anyone joins anyone, with a platform icon in the room list; or also a host option for VR
   players only?
4. **Steam Frame:** the APK in Lepton (cheap, the GoldenEye route), or a native SteamOS arm64 build later?
5. **macOS and Linux:** in scope, or not for now?
6. **Distribution:** sideload only (GitHub releases, as now), or plan for Steam (Windows, PCVR, Frame) and the app
   stores?
