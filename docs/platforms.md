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
| `src/xr` | VR shell from `quest_xr.c`, split into the shared OpenXR code and a graphics binding (GLES on Android, GL on Windows); without OpenXR its host's frames and input (`xr.h`, Vision Pro: `src/visionos/xr_vos.m`) | Quest, Steam Frame, PCVR, Vision Pro |
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

## Two players on one screen (the flat apps, offline)

The game has the cabinet's two guns; online, player 2 is the partner. Offline, the flat apps (Windows, Mac, iOS,
Apple TV, Android) give player 2 to a second device here, as GoldenEye VR's split screen gives a pad to each player.
The rules are flat.c's (`flat_dev_*`, `flat_pad_attach`); each front end only says which device each input came
from.

- **A device** is the keyboard and mouse (one, with a light gun that is a mouse), touch, the Siri Remote, or a pad
  (each pad its own).
- **Alone** every device is player 1's, the last one moved aims, as before. A pad that connects does nothing until it
  is used.
- **Player 2 joins** with a Start press (Space / Enter, a pad's A / Cross or Start, touch's START, the Siri Remote's
  Play/Pause held) when all of these hold: not online; player 1 is playing (`host_player_playing(0)`: in a stage,
  not the continue countdown, a scene, the title or the demo); player 2 isn't in the game (`host_player_state(1)`:
  never joined, or out on their continue countdown or after it); and the device isn't player 1's. A device is player 1's once a Start of its has gone to player 1 this game: the one that
  started the game, continued it or skipped a scene. The press is player 2's coins and Start, and the device is
  player 2's from then on: its sight (the blue crosshair), shoot, reload, and its Start (player 2's continues).
  Every other Start is player 1's, so a pad picked up to continue player 1 continues player 1.
- **Player 2's seat** stays with that device while the game runs: Start on it is always player 2's. When player 2 is
  out (the countdown, or their game over), Start on another device that isn't player 1's takes the seat over and
  continues player 2. The game over (neither player in the game nor on a countdown: the ranking, then the title)
  frees the seat and forgets which devices were player 1's.
- **A pad that disconnects** lets go of its trigger and reload (player 2 just stops shooting); a pad that connects
  again gets a gone pad's id, player 2's first, so it is player 2's again (Windows: SDL3 or XInput; Apple:
  GameController; Android: the input device id, a gone one found once a second through `InputDevice.getDevice`).
- **Online**: starting online play drops a local player 2 (the app restarts into the online game); the ONLINE tab
  says so while a player 2 is here, as do the HOST and QUICK MATCH confirmations. In an online game every device is
  the local player's, as before.
- **The game's side** (arcade.cpp): player 2's inputs travel the same event queue as player 1's (the mouse events'
  `which` 1, the 2 key as its Start, a user event for in and out), read once per frame as player 1's are, and
  `arcade_net::frame` gets them as a second local gun only when not online, where they fill the inputs a guest's gun
  fills online (gun 2's aim, trigger bit, Start). Alone with no player 2 the inputs are exactly as before, so the
  determinism tests and online play don't change. `draw_crosshairs` draws player 2's crosshair too (blue; a TV's
  larger style on Apple TV).
- **Tests** (Windows): `FLAT_TEST_PAD=n` plugs in n virtual SDL pads and `FLAT_TEST_PAD_SCRIPT` drives them and the
  keyboard and mouse (pad_win.c: `2@40:A,1@45:RT,K@50:0.3;0.4,2@60:OFF,2@65:ON`, ...), logging each pad's player
  and sight a second after each step.

## Rumble and trigger feedback

Every build that has something to vibrate gives the same three events, each player's own, read once from the game:
arcade.cpp's `update_ammo` compares each player's bullets, lives and state byte (docs/vr.md, "In play or not") from
one game frame to the next and counts **a shot** (a bullet less), **a reload** (the magazine full again, in play)
and **a hit** (a life less); steps of exactly one, so a continue, RESTART or an online drop-in's loaded state is none.
`host_player_events` publishes the counts and quest.h's `host_events_new` gives a shell what's new since it last
looked (two events between looks are one: no burst).

| Event | Flat pads | Phone by touch (iPhone, Android) | Headsets |
|---|---|---|---|
| Shot | Short kick (low 0.25, high 0.75, 70 ms) | iPhone: light impact; Android: the system's click | The firing hand (as before: when the bullet count drops after a pull) |
| Reload | Softer pulse (0.2, 0.2, 90 ms) | iPhone: soft impact; Android: the system's tick | The reloading hand (as before) |
| Hit | Strong and long (1.0, 0.7, 450 ms) | iPhone: heavy impact; Android: a 300 ms full buzz | **New:** both hands, full, 400 ms |

- **Which device** (flat.c `flat_player_device`): the one that player last shot, reloaded or pressed Start with.
  Alone, player 1's events go to whichever device they are using (a pad, touch); with a player 2, player 2's pad
  gets only player 2's. Online only this player's slot (`net_local`), never the partner's.
- **Never while paused or in the menu** (online too, where the game runs on): the events of that time are dropped,
  and a running rumble stops when it opens (`flat_os_rumble(dev, ..., 0)`; the headsets' `xrStopHapticFeedback`, the
  Sense controllers' player stopped). The app leaving the screen or quitting stops everything (`flat_rumble_stop`).
- **The setting**: Rumble (`QOPT_RUMBLE`, ON by default), on the headsets' GUN tab as before and now on the flat
  apps' GAME tab (mouse, touch and TV lists). The mouse builds' GAME tab was full: their Display row moved to SYSTEM.
- **Never waits**: SDL and XInput return at once; Apple's CoreHaptics engines are made and played on a serial queue
  of their own; Android's vibrators are called from a thread attached to Java once (the frame only queues).

**Trigger feedback** (GoldenEye VR's, 5a15a50c, made a gun's): the GAME tab's Trigger feedback row (`QOPT_TRIGGER`,
ON; dimmed and off while Rumble is OFF). Pads only, each player's own:
- **DualSense**: while that player plays (`host_player_playing`), the right trigger (R2 shoots) is a gun's:
  resistance from a third of the pull and a snap at about 60% (Apple: `setModeWeaponWithStartPosition:0.3
  endPosition:0.6 resistiveStrength:0.85`; SDL: the effect packet's weapon mode `25 48 00 06`, zones 3 and 6,
  strength 7 of 8). Each shot swaps it for 100 ms of vibration (`06 28 ff 00`, GoldenEye's simple vibration), then
  the weapon again; a reload buzzes the left trigger lightly for 80 ms. Off (`05`) when the player isn't playing
  (scenes, the continue, the title), paused, in the menu, at the quit and in the background, and when the pad is no
  longer that player's, so a DualSense is never left stiff. A pad that disconnects is forgotten (it comes back loose).
- **Xbox One / Series** (impulse triggers): the right trigger's motor buzzes 100 ms at full on a shot, the left one
  80 ms at 35% on a reload (SDL `SDL_RumbleGamepadTriggers`; Apple: CoreHaptics engines at
  `GCHapticsLocalityRightTrigger` / `LeftTrigger` where the pad lists them).
- Windows needs SDL3.dll for it (its type and trigger-rumble capability say which pad is which); XInput alone has no
  trigger motors, so plain rumble only. Windows waits 60 ms at the quit for SDL's thread to send the last effect.
  Android has no API for either: none there.

| Platform | Pads | Touch | Triggers | Notes |
|---|---|---|---|---|
| Windows | SDL3 `SDL_RumbleGamepad` (SDL3.dll, loaded at run time), else XInput `XInputSetState` (stopped by `pad_poll` at its end) | none (no touch) | SDL3: DualSense effects, Xbox trigger motors | Keyboard and mouse: none |
| Mac | `GCController.haptics` with CoreHaptics (macOS 11) | none | `GCDualSenseAdaptiveTrigger` (macOS 11.3), Xbox trigger haptics | CoreHaptics imported before AVFoundation (its header names `AVAudioSession`) |
| iPhone / iPad | as the Mac (iOS 14) | iPhone: `UIImpactFeedbackGenerator` light / soft / heavy; iPad: none | as the Mac (iOS 14.5) | |
| Apple TV | as the Mac (tvOS 14) | none | as the Mac (tvOS 14.5) | The Siri Remote has no motor |
| Android phone | `InputDevice.getVibratorManager()` and a `CombinedVibration` (Android 12), else `getVibrator()` | The phone's vibrator: click, tick, a 300 ms buzz | none (no API) | `android.permission.VIBRATE` |
| Quest, Steam Frame, PC VR | OpenXR `xrApplyHapticFeedback` (as before) | | | The hit: both hands |
| Vision Pro | Sense controllers' CoreHaptics (as before) | | | The hit: both hands; amplitude 0 stops what's playing |

- **Tests**: `FLAT_TEST_RUMBLE=1` (env.txt) logs each request (`rumble: player 2 shot on device 17 (low 0.25, high
  0.75, 70 ms)`, `trigger: device 16 right weapon (in play)`); on Windows the virtual SDL pads have motors and log what
  SDL sends them (start and end), and `FLAT_TEST_PAD_KINDS=ps5,xbox` makes them a DualSense (its effects packets
  logged) and an Xbox Series pad (its trigger motors). The headsets log `rumble: hit, both hands`.

## Per platform

| Platform | Package | Mode | Graphics | XR | Input | ROM import | Notes |
|---|---|---|---|---|---|---|---|
| Quest 3 / 3S | APK (`com.mrsco.hotdvr`) | VR | GLES 3.0 + multiview | OpenXR, Meta loader | Touch controllers, hands | `ImportActivity` (SAF) | Shipping now |
| Steam Frame | APK flavor `frame`, run in Valve's Lepton | VR | as Quest | OpenXR, **Khronos** Android loader | Frame controllers, hands if offered | as Quest | GoldenEye's `frame` flavor is the recipe; Meta's loader only finds Meta's runtime. Built, then tested when a Frame is at hand. The APK only for now, no native SteamOS build |
| Android phone / tablet | APK flavor `mobile` | Flat | GLES 3.0, no multiview | none | Touch, gyro, pads | as Quest | Landscape; 16 KB page alignment |
| Windows | One zip: `HOTDVR.exe` | VR (immersive) when a headset is found, else flat; `--flat` / `--vr` force one | GL 3.3 | OpenXR, any runtime (SteamVR, Link, Virtual Desktop) | Mouse, pads, light guns; VR controllers | Open dialog, or drop the zip on the window | Flat first, PCVR second |
| iOS / iPadOS | IPA (Xcode project from xcodegen), installed with SideStore | Flat | ANGLE on Metal | none | Touch, gyro, MFi / Xbox / PS pads, iPad mouse | Document picker; Files shows the data folder | Built on the Mac; LAN discovery needs Apple's multicast entitlement, so iOS uses rooms and direct IP |
| Vision Pro | visionOS app | VR | ANGLE on Metal, one pass per eye | Compositor Services + ARKit | Hand tracking (aim ray + pinch, as the Quest's hands), PSVR2 Sense controllers | Document picker | P5: builds (`scripts/build_visionos.py`), runs in the visionOS Simulator and plays online with Windows; on TestFlight from 0.5.0; the headset itself not tried yet, see P5 under Progress |
| macOS | App (zip on GitHub) | Flat | ANGLE on Metal | none | Mouse, pads, light guns | Open dialog | Right after iOS, sharing its Apple layer and ANGLE |
| Apple TV (tvOS 17+) | App (Xcode project from xcodegen): TestFlight (from 0.5.0), or installed from Xcode with a developer account | Flat | ANGLE on Metal (its tvOS slices) | none | Siri Remote (touch surface moves the sight, click shoots), MFi / Xbox / PS pads | A page on the LAN (address and QR code on the TV) | Files in Caches (tvOS may empty them): settings and saves also in the app's defaults; voice listens only |

Linux is out of scope.

## What changes per OS service

| Service | Quest today | Windows | Android phone | Apple |
|---|---|---|---|---|
| Lobby, rooms, LAN host | `src/net` sockets | same | same | same (LAN: entitlement) |
| Voice capture | AAudio + JNI permission | SDL2 capture (WASAPI) | AAudio, as Quest | SDL2 CoreAudio, `AVAudioSession` playAndRecord, `NSMicrophoneUsageDescription` |
| Voice DSP (Opus, SpeexDSP, RNNoise) | `voice_dsp.c` | same | same | same |
| Crash reports | HTTP via JNI (`quest_report.c`) | WinHTTP (`crash_win.c`) | the Quest's `quest_report.c` | NSURLSession (`crash_apple.m`) |
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
| TV | **Apple TV** (asked for 2026-11-19): the iOS app's flat shell for tvOS, the Siri Remote as the gun, the ROM set over the LAN | Plays online with the others |

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

**P3, iPhone / iPad and the Mac (2026-11-16): done but for devices.** Built on a Mac (Xcode 27.2,
ANGLE in `~/angle` as GoldenEye VR's): `python scripts/apple_sync.py [--images]` on the PC sends the checkout, the
generated code and the fetched libraries over ssh; `python3 scripts/build_apple.py simulator|device|mac [--app]
[--ipa]` builds there. TestFlight (iPhone and iPad, Apple TV, Vision Pro), Apple TV dev installs and test runs go
through `scripts/apple/` (its README.md): `testflight.sh ios|tvos|visionos BUILD [--upload]`, `dev-install.sh`,
`cycle.sh`, `run-env.sh`, `desktop.sh` (a command in the Mac's desktop session, from ssh). They build from whichever
synced copy they run in, and take the account (team, keychain, App Store Connect key, profiles, device names) from a
settings file on the Mac, outside the repository (`HOTD_TOOLS_ENV`); uploaded archives, with their dSYMs, are kept
there by platform and build.
- `src/platform/apple/apple.m`: platform.h's log (the unified log and stderr), sound out and the microphone through
  CoreAudio (RemoteIO; the Mac's microphone is not done yet: the Mac app hears voice chat but sends none), the iOS
  microphone permission. `posix.c` does the rest. GL ES 3 is ANGLE's on Metal.
- **iOS** (`src/flat/ios`): a UIKit app (a scene, a CAMetalLayer view, EGL from ANGLE), touch as on Android
  (`flat_touch.c`), pads through GameController, Documents as the files folder (the Files app shows it), the ROM set
  from the document picker. It cannot restart itself: online play's restart and a new ROM set say "open the app
  again" and close. `cmake/flat_apple.cmake` makes static libraries, merged into one; `src/flat/ios/project.yml`
  (xcodegen) is the app around them; `--ipa` packs an unsigned IPA for SideStore.
  Checked in the iOS 27 Simulator (iPhone 17): the game at 2622 x 1206, START taps give coins and start a game, the
  menu opens and closes; it hosted a LAN game after its "open the app again" restart and HOTDVR.exe joined and
  played: 100 of 100 checksums over 6,000 frames (and later joined it late, at frame 55,200: 920 of 920).
- **Mac** (`src/flat/mac`, `House of the Dead VR.app`): an AppKit window on a CAMetalLayer, ANGLE's dylibs in the
  bundle, the mouse as the gun, keys as on Windows, pads, Application Support as the files folder, an Open panel for
  the ROM set; online play's restart opens a new copy of the app. Checked on the M4 Pro Mac: the game draws (a
  timer drives the frames: a view's display link stops while the screen sleeps), and it hosted a LAN game that
  HOTDVR.exe joined and played: 60 of 60.
- Apple clang computes the same game: `m2nettest` built on the Mac replayed the PC's and the Quest's runs (30 of 30
  each), and the PC the Mac's (30 of 30).

**P4, PCVR (2026-11-19): done but for a headset.** HOTDVR.exe plays in a VR headset through OpenXR, on whatever
runtime is active (SteamVR, Meta's for Quest Link / Air Link, Virtual Desktop's), immersive and cabinet views as on
the Quest.
- **`src/xr`**: the Quest's VR shell left `src/quest` (`quest_xr.c` is `src/xr/xr_shell.c`; the guns, hands and
  cabinet beside it). `xr.h` is the seam to its host: `src/quest/quest_main.c` on the Quest (the NativeActivity, EGL,
  logcat, the crash handler, multicast lock, updater, restart and ROM import, the loader's JavaVM, the APK's assets)
  and `src/xr/xr_win.c` on Windows. The models load through `qasset` (the APK's assets; `assets\` beside the exe).
  The Quest's code paths are unchanged; on the Quest 3 the new build starts as 0.4.1 did, line for line in the log,
  and runs the game in 3D (only one run got past the Guardian dialog: see below).
- **Windows**: Khronos's loader (`openxr_loader.dll` 1.1.53, from the OpenXR SDK's release, hash pinned in
  `build_windows.py`) is delay-loaded, so the flat app runs without it. A WGL 3.3 compatibility context (the shell
  draws some things from client memory, which a core profile refuses) on a window that mirrors the left eye (Esc:
  the VR menu). The eyes draw 4x MSAA into renderbuffers resolved into the sRGB swapchains (GL_FRAMEBUFFER_SRGB on).
  Index, Vive and WMR controllers are bound besides Touch and the simple profile.
- **VR or flat**: VR when the active runtime has a headset (`xrGetSystem` succeeds), flat otherwise; no loader, no
  runtime, or no headset all fall back to flat (checked: Virtual Desktop's runtime without its headset answers
  `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, a missing runtime JSON, the DLL taken away). `--vr` / `--flat` override it; the
  SYSTEM tab's **PLAY IN VR** / **PLAY ON THE SCREEN** (beside CHANGE ROM SET) starts the app again in the other mode
  and remembers it in `pc-mode.cfg`. Without a ROM set the screen asks for it first. VR keeps its settings in
  `vr-options.cfg` as on the Quest.
- **SteamVR's quirks** (GoldenEye VR's notes): its dashboard takes the left menu button, and the Index has none, so
  on a PC a thumbstick's double click also opens the VR menu (a held click still switches the view). The shell uses
  no quad layers (the HUD and menu are drawn into the eye buffers), so GoldenEye's quad-slot and sub-rectangle
  troubles don't arise.
- **Checked without a headset** on SteamVR 2.18.4's null driver (forced for the test with `XR_RUNTIME_JSON` and
  settings put back after): the session runs focused, every frame drawn (about 320 frames in each 10 s, the null
  driver's pace), the immersive view and the VR menu render with the right colours (`XR_SHOT` pictures of the left
  eye). Online: HOTDVR.exe in VR hosted a LAN game and a flat copy (`--files`) joined by IP and played by script: 180
  of 180 checksums over 10,800 frames. `crossplay_check.py`: PC x64 and Quest arm64 30 of 30 both ways, save states
  the same for 600 frames both ways. The link version stays 3.
- Test switches (env.txt): `XR_SHOT=seconds[,...]` (the left eye to `xr_shot.ppm`), `XR_QUIT_AT=seconds`, and the
  shell's `QUEST_MENU_AT`, `QUEST_TEST_ONLINE`, `QUEST_TEST_TALK` work on the PC too. The PC's port.log has an `xr:`
  line every 10 s (frames, drawn, session state, view, controllers).
- Fixed on the way: the Windows app's restart (online play, a new ROM set) runs from the folder it started in, so a
  relative `--files` still names the same folder (it made `files2\files2` before).
- **Left for a player** (a headset on): on a PC, Quest Link and SteamVR (a Quest through Virtual Desktop or Steam
  Link, or an Index): both views, the controllers (aim, trigger, reload by grip / B / Y and the gestures, A / X skip),
  the thumbstick double click under SteamVR, haptics, the mirror window, PLAY ON THE SCREEN and PLAY IN VR; voice in
  VR on a PC. On the Quest 3, this build's menu, guns, hands, cabinet room and passthrough (the shell moved; the
  desk runs here kept being stopped by a Guardian dialog). Seen on SteamVR: the attract mode's title logo draws over
  the VR menu (the game's face layer comes after the menu; probably the Quest's too).

**P5, Vision Pro (2026-11-19): built, checked in the visionOS Simulator, crossplay with Windows; its VR shell is
`src/xr`'s, the Quest's and the PC's.** Made on branch `t3/visionos-vr-port`, merged beside PCVR and the Apple TV. On the PC
`python scripts/apple_sync.py --dest <folder>`, then on the Mac `python3 scripts/build_visionos.py simulator|device
[--app]` (`--archive BUILD [--upload]` as build_apple.py's, with HOTD_BUNDLE_ID; TestFlight below). Both the
simulator app and the device app (unsigned) build with Xcode 27.2 against the visionOS 27.2 SDK, deployment target 26.0. ANGLE's visionOS
slices: `~/angle/xcframeworks-visionos`, as GoldenEye VR's `tools/build_angle_apple.sh` makes them.
- `src/visionos`: `VosApp.swift` opens straight into a full-immersion space (a CompositorLayer; the scene role
  `CPSceneSessionRoleImmersiveSpaceApplication`) and has the document picker's window for the ROM set;
  `xr_vos.m` (the shell's host, as `quest_main.c` and `xr_win.c` are) is the compositor's thread: ANGLE with a pbuffer, ARKit (world tracking for the head, hand tracking,
  accessory tracking for the Sense controllers' poses, one session), each frame's input, **one GL pass per eye** into
  an sRGB Metal texture of ours (EGL_METAL_TEXTURE_ANGLE, GL fenced to Metal by ANGLE's shared event), then a Metal
  pass copying each eye into the drawable with its depth at 2 m (visionOS drops a frame whose depth is all 0), the
  drawable stamped with the head's device anchor; `VosControllers.swift` reads the Sense controllers' buttons
  (GameController) and rumbles them. These follow GoldenEye VR's native visionOS build, but each eye is drawn with
  its own real (canted) pose and field of view, so the copy is one to one.
- **The shell is `src/xr/xr_shell.c`**, every headset's (it was first a copy, `vos_shell.c`, now gone): the immersive
  and cabinet views, the cabinet room (dark, the courtyard; passthrough is not offered), the guns, hands, menu,
  reloads (gestures included), recenter, the online partner's guns and hands, the same settings file
  (`vr-options.cfg`) and the QUEST_* test switches. It builds without OpenXR for Vision Pro (`XR_OPENXR=0`,
  cmake/visionos.cmake): the OpenXR runtime (instance, session, actions, swapchains, hand tracking, passthrough,
  refresh rate, events) is under that switch, and the host paces the frames instead: each display frame it hands the
  shell the head, the floor, the eyes' distance, the hands (aim ray, pinches, palm to the face) and the controllers
  (laid out as Touch) in an `XrhInput` (`xr_host_frame`), then asks for each eye with its pose and field of view
  (`xr_host_eye`). `xr.h` has that seam beside the OpenXR hosts'. An app that can't start itself again shows a last
  page with `xr_close_with` (online play's start and end, a new ROM set while the game runs); `xr_rom_chosen` starts
  the game at once when the set comes in on the setup page (`quest_clear_message` in `host_sdl.c`). The models load
  through `qasset` (the bundle's `assets` folder).
- Test switches the Vision Pro added, now every headset's: `QUEST_TEST_PLAY=1` (FLAT_TEST_PLAY's scripted play:
  coins, Start, shots round the screen, a reload every eighth) and `QUEST_TEST_PICK=seconds[,file]` (the ROM set's
  import on the setup page as a pull starts it; on Vision Pro the host answers with that file from Documents 3 s
  later, as the picker does). The low-head floor (below) is the host's, as it is ARKit's origin.
- **Hands** (ARKit's skeleton, in `xr_vos.m`): the aim ray runs from a shoulder point (20 cm under the head,
  17 cm aside, by the head's heading) through the hand; a thumb-index pinch fires (on under 1.5 cm, off over 3 cm),
  a thumb-middle pinch on the off hand is the skip button, the left palm turned to the face with a pinch opens the
  menu. **Sense controllers** as Touch: trigger fires, grip reloads (as the GRIP setting), A / X skip, B / Y reload,
  Create / Options the menu, the stick's click held switches the view; poses from ARKit's `aim` location.
- **LOCAL** is ARKit's origin raised to the first head's height, recentred on the first tracked head (the Quest's
  LOCAL is at your eyes when the app starts). Documents is the files folder (the Files app shows it); online play's
  start and a new ROM set say "open the app again" and close on a pinch, as iOS.
- **Crossplay:** every file of the game is compiled with `-fno-fast-math -ffp-contract=off` by Apple clang for arm64,
  the compiler and flags that matched the PC and the Quest in P3 (m2nettest, 30 of 30 both ways). Online play
  (`src/net`, `arcade_net.cpp`, the VR scenery hook on) is the same code as every other app. ONLINE_VER unchanged.
- **Checked in the visionOS 27.2 Simulator** (Apple Vision Pro; the T3 device tools don't offer it, so `simctl`
  installed, launched, pushed the files into the app's Documents and took the pictures). The simulator has one view
  (3840 x 2160), a fixed head, no hand tracking and no accessory tracking, so the test switches stand in for a player:
  - It starts into the space and draws (ANGLE on the simulator's GPU, 90 Hz): the setup page without a ROM set; with
    hotd.7z in Documents/arcade the game, the board 0.7 to 2 ms a frame. `QUEST_SHOT_HANDS=3` puts the gun before
    the head: the immersive view's attract scenes round you (the office, the courtyard).
  - The simulator's head starts at ARKit's origin, so the floor was at eye level (the cabinet view showed its coin
    door): a first head under 0.5 m now takes a standing player's 1.6 m (a real headset's ARKit origin is on the
    floor).
  - The cabinet view (`view=1` in vr-options.cfg) with `QUEST_TEST_PLAY=1`: the cabinet in the courtyard, the game
    on its screen, coins and a game under way. `QUEST_MENU_AT=12,0`: the menu (PAUSED, the GUN tab) with the gun's
    ray on it.
  - The document picker: `QUEST_TEST_PICK=seconds[,file]` (new) opens the picker's window as a pinch on the setup
    page does, and with a file in Documents answers as the picker does when you choose it. The window and its file
    browser opened beside the space, the set moved into arcade/, and the game started without reopening the app
    (`quest_clear_message`).
  - Online, with HOTDVR.exe (MSVC, this branch's `host_sdl.c`): `QUEST_TEST_ONLINE=5,hostlan` showed "open the app
    again"; opened again (by `simctl`: the simulator can't pinch) it hosted, and HOTDVR.exe joined by IP
    (`FLAT_TEST_ONLINE=8,joinip:<the Mac>`, `FLAT_TEST_PLAY=1`), loaded at frame 1,948: **140 of 140** checksums
    over 8,400 frames, both playing. HOTDVR.exe hosted a room on zonesync.net and the Vision Pro joined by its code
    (`QUEST_TEST_ONLINE=5,join:CODE`), loaded at frame 2,282: **140 of 140** over 8,400 frames, the guest's gun
    blue. No desync either way. A LAN join from the Mac to the PC didn't connect (the PC's firewall blocked that
    copy of HOTDVR.exe), as the Apple TV found.
  - Another immersive app opened in the same simulator closes this one's space, and the app closes ("the immersive
    space closed").
- **Checked again on the shared shell** (2026-11-19, a simulator of its own): the immersive view with
  `QUEST_TEST_PLAY` (a game under way, the gun before the head with `QUEST_SHOT_HANDS`), the cabinet view and its
  menu (`QUEST_MENU_AT`), `QUEST_TEST_PICK` (setup page, the picker's answer, the set into arcade/, the game started),
  and online play: `QUEST_TEST_ONLINE=5,hostlan` showed the last page ("open the app again"); opened again it hosted
  on the LAN and HOTDVR.exe (flat, `FLAT_TEST_ONLINE=5,joinip:<the Mac>`, `FLAT_TEST_PLAY=1`) joined, loaded at frame
  1,387: **140 of 140** checksums over 8,400 frames, no desync, both players in the game. The visionOS simulator and
  device apps build without warnings from the shell; the Quest, Frame and phone APKs, HOTDVR.exe (flat and in VR on
  SteamVR's null driver: focused, every frame drawn, the menu), the iOS, Mac and Apple TV apps build;
  `crossplay_check.py`: 30 of 30 both ways, save states the same for 600 frames both ways.
  SteamVR's null driver needs, for a test: a play area (`chaperone_info.vrchap` for its universe, else Room Setup
  opens and closes the app) and standby off (`power`: `turnOffScreensTimeout`, `pauseCompositorOnStandby`), else the
  session stops being focused after about 10 s; both put back after.
- **Left for a real Vision Pro:** stereo and the canted eyes, head tracking, the depth written at 2 m (not black), the
  frame rate (eyes at the drawable's size with 4x MSAA if ANGLE has EXT_multisampled_render_to_texture), the hands'
  aim ray, pinch thresholds and whether the palm-and-pinch menu gesture collides with visionOS's own (Home / Control
  Center), the Sense controllers (aim pose, buttons by name, rumble), a file chosen in the picker by hand, the
  microphone and voice chat (the permission prompt shows over the game when an online game starts), and an online
  game with a Quest and a PC.
- **TestFlight (2026-11-20): 0.5.0 build 9** went up under the iPhone app's record (`com.mrsco.hotdvr`, which had
  its visionOS platform already). The iPhone app's App Store profile serves visionOS too (an iOS App Store profile
  lists iOS, xrOS and visionOS), so `HOTD_PROFILE` is that profile's name; the rest of the account as for
  build_apple.py (`HOTD_TEAM`, `HOTD_KEYCHAIN`, `HOTD_KEYCHAIN_PASS`, `HOTD_ASC_KEY_DIR`, `HOTD_ASC_KEY_ID`,
  `HOTD_ASC_ISSUER`), over plain ssh like the iPhone's (manual signing, the keychain unlocked with its password file):
  `scripts/apple/testflight.sh visionos BUILD --upload`. Validation
  refused the first archive with no visionOS app icon (error 90970, CFBundleIcons.CFBundlePrimaryIcon), so the app
  has one now: `src/visionos/Assets.xcassets`, a round solid image stack (`python scripts/make_icons.py visionos`:
  the square icon as the opaque back layer, clear middle and front layers). Export compliance comes from
  `ITSAppUsesNonExemptEncryption` = NO in Info.plist, as the iPhone app's. Build numbers rise per platform
  (`testflight.sh visionos next` asks App Store Connect for the next); `release.py --testflight` takes the Quest's
  versionCode, or that when it is higher.
- **Later:** the cabinet room's PASSTHROUGH as mixed immersion; the display
  rate (GRAPHICS' Hz row only shows the system's); foveation.

**Since (2026-11-16):**
- **The 3D was too dark everywhere**: the shared renderer decoded the arcade's colours with a 2.2 power and the sRGB
  framebuffer encoded them with the sRGB curve, which crushed the shadows (5% showed under 2%). It uses the exact
  sRGB curve now (the Quest too). A **Brightness** setting (GRAPHICS in VR, GAME in the flat apps, 25% by default)
  raises the black level and lifts the shadows; 0% is as the arcade drew it.
- **Pads**: Windows loads SDL3.dll (built from `extern/sdl3`, shipped beside HOTDVR.exe) for Xbox, PlayStation,
  Switch and other pads, XInput without it; pads with a gyroscope can aim with it (GAME: Pad gyro aim). Checked with
  a virtual SDL pad (`FLAT_TEST_PAD=1`: the stick moves the sight, A starts a game) and the Android emulator's
  gamepad keys (A starts a game, Select opens the menu).
- **Gyro aim on phones and tablets** (GAME: Gyro aim, Gyro speed): turning the device moves the sight, a tap
  shoots at it. Android (the gyroscope, by the display's rotation) checked in the emulator; iOS (CoreMotion) builds.
- **Crash reports on Windows** (`crash_win.c`): the Quest's JSON to the same relay, the PDB kept by build id in
  `build_windows/symbols/`; checked end to end against a stand-in relay, frames symbolized.
- **Crash reports on every platform** (2026-11-20, `tools/vr/crash-relay/README.md`): the relay is the project's own
  Worker at `https://hotdvr.roccojuliano.com/v1/reports` (deployed; it files issues once its GitHub token is set),
  `REPORT_URL` set in `quest_report.c` (Quest, Steam Frame, and now the Android phones: its signal handler moved there
  from `quest_main.c`), `crash_win.c` (Windows flat and PC VR, `platform` windows / pcvr; version and commit stamped
  by `scripts/build_info.py`) and the new `src/platform/apple/crash_apple.m` (iPhone and iPad, Mac, Apple TV, Vision
  Pro: a signal handler writing crash.txt with the executable's LC_UUID and image+offset frames, the last run's
  port.log kept as port.prev.log, NSURLSession). The SYSTEM tab's Crash reports row now shows on every flat app
  (`flat_os_has_reports`). The relay labels issues `platform:<platform>` and keys Windows and Apple crashes by offsets
  and build id (their shipped binaries have no names); the private repository's triage workflow writes a
  deterministic digest (symbols, disassembly, the source at the report's commit, a `sig-` label across builds and
  platforms, duplicates closed). Symbols for every platform go to the private `crash-symbols` release on each release
  (`quest_tools.py symbols [--apple]`, run by `release.py`: the Android libraries, Windows' PDB and exe, the Apple
  dSYMs from the Mac's builds and TestFlight archives via `scripts/apple_symbols.py`); the Apple builds now compile
  with line tables for their dSYMs, and the Mac app gets its dSYM and is stripped of its debug map. Checked: the relay's
  tests, `wrangler dev` against a mock GitHub, a forced crash (`FLAT_CRASH_TEST`) on Windows, in the iOS simulator and
  in the Mac app sent through it (issues filed, then symbolized with the build's PDB or dSYM), the phone APK in the
  emulator sending its report to the deployed Worker over HTTPS (the 503 of a relay without its token: kept for
  later), and the digest on GitHub's runner for a Quest, a Windows and an iOS report. Not run: the Quest, Apple TV and
  Vision Pro apps' crash path (tvOS and visionOS compile).
- **The Mac's microphone** (AVAudioEngine, AVCaptureDevice permission); this Mac mini has no microphone, so only its
  receiving is checked. **Android phones** take Wi-Fi's multicast lock in an online run, as the Quest does.

- **Joining a game under way is quick** (link version 3): the host sends its game as it is, a save state packed
  against the ROMs to about 180 KB, instead of the guest running every frame since the host's start. Through
  zonesync.net the guest was in 2.4-3 s after the session began; checked between two Windows apps (LAN: 40 of 40
  checksums after the load), with m2netguest through the relay, and PC to Quest and back by `crossplay_check.py`
  (save on one, load on the other: the same for 600 frames; the two machines' states of a frame are the same bytes).
  Details in docs/vr.md, "Online play and voice chat". A build of version 2 can't share a room with these.

- **Checked against the original (MAME 0.289)**: `scripts/compare_mame.py` puts MAME's frame beside ours (the
  software rasterizer, MAME's algorithm, and the apps' m2_gl), found by picture; MAME is driven by our input scripts
  (`tools/mame-plugins/hotd_shots.lua`) and the app saves exact frames (`M2_SHOTS`, `M2_INPUT_SCRIPT`, arcade.cpp).
  The game and the software rasterizer match MAME pixel for pixel; m2_gl had three faults, now fixed:
  - **Figures in walls and floors**: the arcade has no depth buffer (its sort order decides), and the game's models
    dip into what they stand on or lean against: with real depth the girl at the mansion's door showed only her
    hand, the dead lying on the ground were mostly gone, feet sank into floors. The flat apps now draw in the
    arcade's order alone (`m2_gl_arcade_order`: its picture exactly); the headset keeps real positions but writes
    each polygon's depth at the z the arcade sorted it by (`GeoPoly::sortz`, `M2Vert::key`; a polygon reaching
    behind the camera, as the VR scenery's ground does, by the plane at that z), so the guns and hands still meet
    the world in depth. Against the arcade's order over 102 frames of attract and stage 1: the same but for slivers
    of distant ground at the screen's edge (0.44% of the pixels at most).
  - **Windows drew the 3D far too dark** (MAME's mid tones at 0.6): desktop GL encodes into an sRGB target only
    with `GL_FRAMEBUFFER_SRGB` on (GLES always does). At Brightness 0% Windows now matches the software
    rasterizer's colours. The Brightness default, chosen while Windows was too dark, is 25% now (was 50%: it washed
    out the lit scenes; 25% opens the darkest rooms and leaves those near the arcade's), under a new key in
    options.cfg (`brightness_level_pct`) so the 50 saved files carry is dropped.
  - The "plain floor" seen on Windows was those two together (the marble's pattern is there).

**Apple TV (2026-11-19; on two real Apple TVs 2026-11-20): done but for a person with the Siri Remote.** On the Mac:
`python scripts/apple_sync.py [--dest DIR]` from the PC, then `python3 scripts/build_tvos.py simulator|device [--app]
[--ipa] [--dest UDID]` there (the
libraries in `build_tvos/<slice>`, the app from `src/flat/tvos/project.yml`; ANGLE's tvOS slices in
`~/angle/xcframeworks-tvos`, made by GoldenEye VR's `tools/build_angle_apple.sh tvos-device tvos-simulator`).
- `src/flat/tvos/main_tvos.m`: the iOS app's shape (a scene, a CAMetalLayer view, ANGLE's EGL, a frame each display
  refresh), 4:3 in the middle of the TV. Picture HD is drawn at most 1080 lines (1440 x 1080, 4x MSAA) and scaled up
  on a 4K TV (`flat_picture_limit`); on the Apple TV HD's A8 (Metal's Apple2 family) Picture starts as Original and HD
  is 720 lines without MSAA (see the hardware check below). The controller is a `GCEventViewController`, so the
  remote's and pads' presses are the game's and Menu doesn't leave the app. No screensaver while a player is in the
  game or online play is on (`idleTimerDisabled`: tvOS suspends an app under it, and an online partner would wait).
- **The Siri Remote** (GameController's micro gamepad, absolute touch positions): the touch surface moves the sight
  like a mouse, relative, a slow swipe across it half the picture and quick ones up to three times that (GAME:
  Remote speed, 25-300%); a click shoots (a 2nd generation remote: the centre click, `Button Center`; its ring's four
  clicks, `Cardinal Direction Pad`, reload: its `Button A` is any click, the ring's too); Play/Pause reloads;
  Play/Pause held half a second gives coins and Start (skips a scene); Menu opens the menu. The sight stays on the
  picture (its crosshair, larger and outlined on a TV, always shows; it starts in the middle).
  After the first test on an Apple TV 4K ("jumping around", the crosshair lost, reloads unreliable), and as GoldenEye
  VR found on the same TV: the surface's last reading as the finger lifts is a few hundredths, not 0, and a click's
  press moves the finger. So a move applies only once the next reading shows the finger still down, a reading near
  the middle right after one far from it is the lift, a jump of over a quarter of the surface is a new landing, the
  surface is frozen while a click is held and 0.12 s after, and a click puts the sight back where it was 60 ms
  before the press.
  In the menu (its TV layout: the remote's keys line, GAME = Picture, Crosshair, Brightness, Remote speed, no QUIT)
  each third of the surface swiped is one focus step (a 2nd generation remote: the ring's clicks instead, repeating
  when held), a click chooses, Play/Pause resumes, Menu goes back, and at the menu's top Menu goes to the Home
  screen, as Apple asks: there the controller hands its presses to UIKit (`controllerUserInteractionEnabled`).
  UIKit's own presses (a keyboard, the Simulator's remote) do the same; a press seen by both is counted once.
- **Pads** as on iOS (either stick moves the sight, R2 / R1 shoot, L2 / L1 / X reload, A / Menu coins and Start,
  Options the menu); in the menu the d-pad, A and B work as the remote's swipes, click and Menu, L1 / R1 the tabs.
- **The ROM set** (`tv_upload.m`): a TV has no document picker and no Files app, so when the set is missing the app
  shows `http://<the TV's address>:8007/` (another port if taken) and its QR code; that page, opened on a phone or
  computer on the same network, sends hotd.zip or hotd.7z in one POST. The file's first bytes decide (a ZIP or a 7z,
  whatever its name; anything else is refused, on the page and the TV); then the game starts. The menu's ROM row
  shows the same page over the game (Menu cancels it); a new set loads when the app opens again.
- **Storage** (`tv_store.m`): a tvOS app can't write Documents on the device and keeps only its defaults
  (NSUserDefaults, about 500 KB); everything else is Library/Caches, which tvOS empties when it runs short of space.
  The files folder is `Library/Caches/HOTDVR`; options.cfg, online.cfg and save/ (the game's settings and high
  scores, about 17 KB) are copied into the defaults every 20 s, on going to the background and before closing, and
  put back at start when missing. A ROM set that was there and is gone is asked for again with the reason ("tvOS
  removed the ROM set to free space... your settings and scores are kept"). Keeping the ROM itself would need iCloud
  or on-demand resources: not done.
- **Voice chat** listens; an Apple TV has no microphone of its own. `apple.m` asks for one (AVAudioApplication) only
  when tvOS reports an input (a headset through the TV), else voice is receive-only.
- **Online play** as on iOS: rooms, quick match, LAN host and direct IP; an app can't restart itself, so starting or
  leaving online play says "open the app again" and closes. Nothing in the protocol changed (no ONLINE_VER bump).
- **Install**: SideStore and AltStore don't sign for tvOS, so an Apple TV takes the app from Xcode with a developer
  account (`DEVELOPMENT_TEAM=<team> python3 scripts/build_tvos.py device --app --dest <the Apple TV's UDID>`, then
  `xcrun devicectl device install app --device <UDID> build_tvos/dd/Build/Products/Release-appletvos/HouseOfTheDeadVR.app`;
  a free account's build lapses after 7 days, a paid one's after a year), or through TestFlight (below). On a Mac
  with no account in Xcode, automatic signing takes the team's App Store Connect key (`HOTD_ASC_KEY_DIR`,
  `HOTD_ASC_KEY_ID`, `HOTD_ASC_ISSUER`, as build_apple.py's) and `--dest` registers that Apple TV with the team. The
  development identity's keychain must be unlocked, so run it from the Mac's desktop session (Terminal, or a
  `.command` file opened there), not over ssh ("No Accounts" / no profile otherwise). `scripts/apple/dev-install.sh
  [--launch] DEVICE` does all this from ssh (the build through `desktop.sh`), and `cycle.sh` / `run-env.sh` run the
  tests below (env.txt in, screenshots and port.log out). The home-screen icon (the wide
  banner, 5:3, as a layered stack) and the Top Shelf come from `python scripts/make_icons.py tvos`.
- **TestFlight (2026-11-20): 0.5.0 build 9**, under the iPhone app's record and bundle id (`com.mrsco.hotdvr`), as
  GoldenEye VR's Apple TV app is: `python3 scripts/build_tvos.py device --archive BUILD [--upload]` makes the App
  Store archive and .ipa with that build number and sends it (altool, validated first), with the account as
  build_apple.py's (`HOTD_TEAM`, `HOTD_KEYCHAIN`, `HOTD_KEYCHAIN_PASS`, `HOTD_ASC_KEY_DIR`, `HOTD_ASC_KEY_ID`,
  `HOTD_ASC_ISSUER`), over plain ssh like the iPhone's (`scripts/apple/testflight.sh tvos BUILD --upload`; the screen
  may be locked). Two things App Store Connect needed first, both done through
  the API with the team's key: Apple TV as a platform of the app record (an App Store version for tvOS, `POST
  /v1/appStoreVersions` with `platform: TV_OS`; it stays "Prepare for submission", nothing is sent for review), and a
  tvOS App Store profile (`profileType: TVOS_APP_STORE`, the bundle id and the Apple Distribution certificate; named
  "House of the Dead VR tvOS"; the iPhone app's profile doesn't sign tvOS), which `HOTD_PROFILE` names. Export
  compliance comes from `ITSAppUsesNonExemptEncryption` = NO in Info.plist.
- Test switches (env.txt in `Library/Caches/HOTDVR`, sent with `devicectl device copy to --domain-type
  appDataContainer --domain-identifier com.mrsco.hotdvr`): `FLAT_TEST_REMOTE="seconds action [args]; ..."` plays the
  remote through the same code (swipe DX DY [SECONDS], click, play, playhold, menu, up / down / left / right), logging
  the presses and the sight; `FLAT_TEST_PICTURE=lines,samples` another limit for HD. port.log has a perf line every
  10 s (frames drawn, those later than 1.5 refreshes, the main thread's time drawing and waiting for the swap, the
  drawable, the thermal state) and the game's "frame ms" line every 600 frames (the board's parts, the game's own
  frames a second, frames not drawn to keep up, and how long the speakers asked for sound the game hadn't made).
- An Apple TV asleep refuses a launch ("System is asleep - foreground app launch forbidden") and devicectl has no
  wake; `devicectl device reboot` brings it back awake (and the TV on, by HDMI-CEC).
- **Checked in the tvOS 27.2 Simulator** (Apple TV 4K, 3840 x 2160; the T3 device tools don't offer tvOS
  simulators, so `simctl` installed, launched and took the pictures):
  - The ROM page: the PC fetched it over the LAN, a README sent to it was refused, hotd.7z (38.4 MB) arrived in
    1.5 s and the game started by itself (ANGLE on the tvOS simulator's GPU, the board 0.7 ms a frame).
  - The remote by `FLAT_TEST_REMOTE`: two Play/Pause holds gave the credits and started a game; a slow swipe of 0.8
    moved the sight 607 px, the same swipe done quickly hit the stop just past the picture's edge (about 3x); clicks
    shot, Play/Pause reloaded; Menu opened the menu, swipes moved the focus (Remote speed 100% to 125% and back),
    Play/Pause resumed, and Menu at the menu's top logged the hand-over to the Home screen.
  - The caches emptied by hand while the app was closed: 4 files put back (options.cfg byte for byte), the ROM page
    asked again with the reason, the set sent again, the game back.
  - Crossplay with HOTDVR.exe: the Apple TV hosted a LAN game and HOTDVR.exe joined by IP (the host's game loaded at
    frame 897): **140 of 140** checksums over 8,300 frames; HOTDVR.exe hosted a room on zonesync.net and the Apple TV
    joined by its code (loaded at frame 2,104): **120 of 120** over 7,200 frames. Voice: the TV received 3,121 Opus
    frames, none lost, and sent none.
  - The iOS simulator app and the Mac app still build with the shared changes.
- **Checked on real Apple TVs (2026-11-20)**: an Apple TV 4K (3rd generation, A15, tvOS 27.3, a 2nd generation or
  later Siri Remote) and an Apple TV HD (A8, tvOS 26.6.1, a 1st generation Siri Remote), from the Mac over devicectl
  (install, launch, files in and out, `devicectl device capture screenshot` for the pictures), test switches for the
  player:
  - Install: a signed development build on each (the team's key made the tvOS profile); the asset catalog compiles
    without a warning or notice, and Assets.car holds the brand assets (the layered App Icon, 400 x 240 and 800 x 480,
    its flattened and radiosity images, Top Shelf Image and Top Shelf Image Wide at 1x and 2x). The home screen shows
    the banner icon, and the Top Shelf above it when it has the focus.
  - The ROM page on the first start, with the TV's address and QR code; no local-network prompt appeared on either.
    hotd.7z from the PC to `http://<the TV>:8007/upload`: 38.4 MB in 2.0 s (4K) and 2.7 s (HD), the game started by
    itself. ANGLE gives OpenGL ES 3.0 on both, the A8 too (the Apple2 patch in GoldenEye VR's
    `tools/build_angle_apple.sh`, already in `~/angle/xcframeworks-tvos`).
  - **The 4K at 40 to 48 fps, fixed**: HD drew the 3D at the TV's 2880 x 2160 with 4x MSAA; the board took 3 ms, and
    Picture Original held 60 with the main thread at 2.5 ms, so it was the GPU's fill. HD at most 1080 lines (scaled
    up), and the multisampled picture invalidated after its resolve: 59.9 to 60 fps, no frame late, in play and in
    the menu, thermal nominal.
  - **The HD at 15 fps, fixed as far as an A8 goes**: the game's texture decode in each pixel (m2_gl.c) is more than
    its GPU fills: HD 15 fps (1080 lines, 4x MSAA), 20 (no MSAA), 28 (720, 4x), 38 (720), 43 (540); Original near
    60. Each frame's 2D layers went into the one texture the GPU was still reading, and ANGLE's Metal waited 11 to
    15 ms of the main thread for it: three textures in turn (the upload 4 ms; ANGLE converts RGBA bytes for an sRGB
    texture on the CPU, 1.35 ms into plain RGBA8). The VR cabinet room's courtyard, made beside the game on the first
    run (42 s of the A8's two cores), isn't made by the flat apps. Now Picture Original by default there: 59.9 fps,
    no frame late. Not the cause: the VR geometrizer's all-round scenery (culling it as the arcade does saved 0.5 ms).
  - **The HD's sound cut out (the user heard it), fixed**: the busiest scenes took the game thread 18 ms a frame of
    the arcade's 17.4 (the board up to 15.6 ms), so the game ran at 50 to 55 frames a second and the speakers went
    short, up to 1.4 s in 10 s; an online guest a few frames behind dropped its sound outright (5.6 s). A flat app
    behind the arcade's time now skips drawing a frame, never two in a row; the game runs every frame. The HD: 57.5
    frames a second throughout, the speakers 0 ms short (21 to 232 frames of 600 not drawn in the busy parts). The
    4K: 57.5 and none skipped.
  - The remote by `FLAT_TEST_REMOTE` on both: Play/Pause held twice gave coins and Start; a slow swipe of 0.8 moved
    the sight a quarter of the picture (741 px of 2880 on the 4K, 370 of 1440 on the HD), a quick one further (2.3x);
    a fast swipe down put it under the picture, where a click reloads; Menu opened the TV menu, swipes moved the
    focus, Remote speed 100% to 125%, Play/Pause resumed, Menu at the top logged the Home screen. The 4K's remote
    reported a 2nd generation or later; the HD's 1st generation remote never connected (asleep), so it is untested.
  - Storage: Remote speed changed to 125% in the menu, the app closed and opened again: 125% in options.cfg and the
    menu, the 4 files kept in the app's defaults (17 KB). (Emptying the caches by hand can't be done with devicectl.)
  - Online: the 4K hosted on the LAN and HOTDVR.exe joined by IP, twice: **100 of 100** checksums (from frame 1,427)
    and **220 of 220** (joined under way at frame 3,350, 13,150 frames), the 4K at 59.9 fps hosting. The 4K hosted
    and the HD joined: **200 of 200** (frames 1,711 to 13,680). With the frame skip: the HD hosted (skipping up to 232
    of 600) and the 4K joined, **160 of 160**; HOTDVR.exe hosted a zonesync.net room and the HD joined by its code,
    **160 of 160**, the HD guest at 57.1 to 57.9 frames a second, its speakers at most 174 ms short in 10 s.

- **Two players on one screen** (2026-11-20, branch local-2p; the rules under "Two players on one screen" above):
  - The game's RAM first: with player 1 alone, `host_player_playing(1)` read 1 (0x51F294 isn't player 2's lives) and
    0x51F4A0 went to 0 on player 1's continue screen, so the rules go by each player's state byte instead (docs/vr.md).
  - Windows (HOTDVR.exe, hidden, `FLAT_TEST_PAD` 2 or 3 virtual SDL pads and the keyboard and mouse scripted): the
    keyboard starts player 1, pad 2's A joins as player 2 (the 2P HUD, the blue crosshair beside the red one, its
    shots hitting and its bullets dropping); pad 1 used alone with the mouse there stays player 1 (its Start in play
    doesn't join); a pad that was never used pressing A on player 1's continue screen continues player 1 and is then
    player 1's (its A later in play doesn't join); player 2's pad unplugged and plugged in again is player 2's again;
    player 2 continues with their own pad; a third pad's A while player 2 is out takes the seat over. The ONLINE tab
    and HOST A ROOM's confirmation say player 2 here leaves; the CONTROLS page has the rows.
  - Determinism: `crossplay_check.py`: 30 of 30 checksums both ways (PC and Quest), save states the same 600 frames
    both ways, and the bundles both machines recorded byte for byte the 0.5.0 build's. Online, two HOTDVR.exe on
    this PC (`hostlan`, `joinip:127.0.0.1`): 60 of 60 checksums, no desync; the host's second pad's A there drove
    the host's own player, as before, and joined nobody.
  - iOS Simulator (iOS 27): touch is player 1, a hidden GCVirtualController (iOS 17's) presses A and joins as player
    2, its stick and trigger move and fire the blue sight. Only one virtual controller connects at a time there.
  - Apple TV Simulator: a stand-in pad device (`FLAT_TEST_REMOTE` padstart) starts player 1; the remote's Play/Pause
    held joins as player 2, its swipes and clicks move and fire the large blue crosshair beside the red one; the
    screensaver stays held off in the two-player game.
  - Android emulator (`hotd_test`): the keyboard (adb's keyboard events) starts player 1, adb's gamepad A joins as
    player 2 and R1 fires its sight (the 2P HUD, the blue crosshair). The emulator draws the 3D white; the HUD shows.
  - The Mac app builds; not run with pads (no virtual pads on macOS).
  - Left for a player: two real pads on each of Windows, Mac, iPhone / iPad, Apple TV and an Android phone (SDL3's,
    XInput's and GameController's pad ids, a real disconnect and reconnect, the pads' player lights on Apple), the
    Siri Remote as player 2 beside a real pad on both Apple TVs, a light gun (a mouse) beside a pad, and the feel of
    two sights on a TV.

- **Rumble, touch haptics and trigger feedback** (2026-11-20, branch local-2p; "Rumble and trigger feedback" above):
  - Three bugs first: `host_player_playing` went by 0x51F4A0 == 1 (0 for both players in any two-player game, online
    too: the headsets' reload hints and the magazine reload's drop never came online), now by each player's state
    byte; `host_player_ammo(1)` read player 2's lives at 0x51F294, now 0x51F35C; the TV crosshair's outline was drawn
    in the player's colour (its routine ignored the colour it was given), now black. Callers checked: the flat apps'
    join rule and the Apple TV screensaver go by the state byte already; on the headsets the reload step, the magazine
    drop and the empty-gun click now work as player 2 online, and player 2's gun reads -1 (not a stale 0) while out.
  - Windows (HOTDVR.exe hidden, `FLAT_TEST_PAD=2`, `FLAT_TEST_PAD_KINDS=ps5,xbox`, `FLAT_TEST_RUMBLE=1`): pad 1 starts
    player 1, pad 2 joins as player 2; each pad's motors got only its own player's shots (70 ms), reloads (90 ms) and
    hits (450 ms; three each before each continue countdown), as SDL sent them to the virtual pads (start and end).
    The menu opened 70 ms into a shot's rumble: stopped at once, the DualSense's trigger let go, nothing while it was
    open, the weapon back when it closed. The DualSense's right trigger was the weapon only while its player played
    (off in scenes and on the continue), a shot's buzz for 100 ms then the weapon; the Xbox pad's right trigger motor
    buzzed on each shot, its left on the reload. The quit let the DualSense go; Rumble OFF in options.cfg: no
    request at all. Without SDL3.dll the XInput path runs (no pads here to rumble).
  - The GAME tab with Rumble and Trigger feedback (seven rows, a little closer), Display on SYSTEM.
  - Online, two HOTDVR.exe (`hostlan`, `joinip:127.0.0.1`, a virtual DualSense on the host and an Xbox pad on the
    guest): the host's pad got only player 1's events, the guest's only player 2's; **80 of 80** checksums, no desync.
    `crossplay_check.py`: 30 of 30 both ways, save states the same for 600 frames both ways.
  - PC VR on SteamVR's null driver (a play area and the null driver set for the test, the settings put back after),
    `QUEST_TEST_PLAY=1`: `rumble: hit, both hands` at each life lost. The Quest 3 ran the new APK without a wearer
    (the proximity override): the same lines; the 0.5.0 release APK was put back after.
  - iOS Simulator (iOS 27, a simulator made for the test and deleted after): touch shots asked for the light impact,
    hits the heavy one; a hidden virtual pad joined as player 2 and got only player 2's events (the simulator's pad
    has no motors: "none"); the menu stopped the rumble. Apple TV Simulator: the stand-in pad (player 1) got player 1's
    events and the weapon trigger, the Siri Remote (player 2) only player 2's (no motor). The Mac and visionOS apps
    build (CoreHaptics imported before AVFoundation on the Mac). The phone APK and the Quest APK build.
  - Left for real hardware: the feel of each event on real pads (an Xbox One / Series pad's body and trigger motors,
    a DualSense's adaptive trigger: the weapon's break point and strength, the shot's buzz, never left stiff after a
    pause, the menu, the quit or a disconnect; a DualShock 4, a Switch Pro) on Windows with SDL3, XInput alone, the Mac,
    an iPhone, an iPad and both Apple TVs; an iPhone's touch impacts; an Android phone's vibrator by touch and a pad's
    (VibratorManager); two pads in a two-player game; the hit on the Quest's, a PC headset's and the Sense controllers.

**Left for a player (the end check):** on the headset, a stage and an online game with this build (joining a game under way, both ways: the
headset here lost tracking on the desk, so its app couldn't be opened for a live join), and the headset's new
depth (figures over the floors and walls they dip into: the girl at the door, the first zombie's feet); on Windows, the
mouse, a pad and a light gun by hand, the menu's clicks, full screen, the ROM dialog, and voice with real speech;
a real phone (touch, a pad, the ROM picker, voice); a Steam Frame; an iPhone or iPad (a signed build, touch, voice);
the Mac app by hand; the Apple TVs (both have the build): aiming with the Siri Remote felt good to the player on both
generations (2026-11-20: a 2nd generation remote on the 4K, a 1st generation one on the HD); still to try there:
Play/Pause held, Menu in the game, in the menu and at its top (the Home screen), a pad (and the remote with a pad
connected), the ROM page from a phone (the QR code; no local-network prompt appeared when the PC sent the set, so
whether a phone's upload asks is a person's check), the storage after tvOS empties the caches, voice with a headset
if tvOS offers one, the screensaver held off in a game and coming in the attract scenes, and on the HD the picture and
the sound in the busiest scenes (frames are skipped there, so the picture is less smooth: is it acceptable?) and HD's
720-line picture if chosen. In the HD's menu, Picture's reset arrow sets HD (the shared default), not Original.

**Left, later:**
- Crash reports: the relay's GitHub token (the live test in `tools/vr/crash-relay/README.md`, "Live check"); a forced
  crash on a Quest, a phone, the Mac, an Apple TV and a Vision Pro. The published 0.6.0 Windows build's PDB was not
  kept (only later builds' are), so its reports get no names; its exe also names the build folder's paths (Opus'
  `__FILE__` and, before `/PDBALTPATH`, the PDB's).
- The crossplay check with clang-cl too (CI's compiler; not installed on the dev PC).
- The Quest's `QUEST_TEST_ONLINE` deletes all of env.txt before its restart, so other switches don't survive it;
  the flat hook drops only its own line.
- Phones: the navigation bar (the theme hides the status bar; immersive mode needs the UI thread); a 32-bit
  (armeabi-v7a) build for phones without a 64-bit userspace, if wanted.
- Apple: an app icon for both; CI for the IPA (as GoldenEye VR's plan: a macOS runner with ANGLE's xcframeworks
  from a release asset); a pad's gyro on Apple and Android (GCController.motion, the pad's sensor).

## Releases

Every download is public, so none may carry a build machine's folders or names. The releases up to 0.6.0 did
(found 2026-11-20): the Windows exe and SDL3.dll, the Android libraries and the Apple apps had `C:\Users\<name>\...`
or `/Users/<name>/...` in Opus's and SDL's assert messages (`__FILE__`), and the Apple apps' ANGLE frameworks and
dylibs in their debug maps (the object files' paths on the Mac that built ANGLE). Since 0.6.1:

- **Every compiler maps the folders out**: `cmake/prefix_map.cmake` (CMake's builds, and SDL3.dll's through
  `CMAKE_PROJECT_INCLUDE`) and `build_info.prefix_map_flags` (build_quest.py's own compiles). clang's
  `-ffile-prefix-map` turns the checkout into `.` and the home folder into `~`, in `__FILE__` and the debug
  information alike (the crash symbols' paths are relative to the repository, as the triage's digest reads them);
  MSVC's `/d1trimfile` cuts them from `__FILE__` (its PDB keeps them: it only goes to the private crash-symbols
  release). Only file names change, no floating-point or code-generation flag (`crossplay_check.py`: the same game).
- **ANGLE's copies are stripped** (`strip -S -x`: their debug maps): the Mac app's in `cmake/flat_apple.cmake`; the
  iPhone, Apple TV and Vision Pro apps' frameworks by `build_info.clean_angle` into `build_<platform>/angle`, which
  their Xcode projects embed (`HOTD_ANGLE_FW`). `~/angle` itself is never changed (GoldenEye VR's builds share it);
  ANGLE's release build has no `__FILE__` strings of its own.
- **The guard**: `python scripts/check_paths.py [PATH ...]` looks through every file of `build_release/<version>/`,
  into the zips, APKs and IPAs, for `C:\Users\<name>\`, `/Users/<name>/`, `/home/<name>/` and this machine's user and
  host names (from the environment, and `$HOTD_MAC`'s: none is written in the script), in 8-bit and UTF-16 text.
  A signed Apple app's signing (its provisioning profile and code signatures name the developer team, as the App
  Store does) and an App Store .ipa's `Symbols/` (Apple's crash symbols, which name the archive's place on the Mac;
  App Store Connect keeps them, no device gets them) don't count.
  `release.py` runs it once the downloads are built and stops on any hit, before anything is uploaded or published;
  the TestFlight archives (`build_apple.py`, `build_tvos.py`, `build_visionos.py --archive`) check their .ipa before
  sending it.

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
3. **Crossplay:** anyone joins anyone, a platform icon in the room list, no VR-only option. *Done (0.5.0): a tag
   with the host's platform (QUEST, FRAME, ANDROID, PC, PCVR, MAC, IPHONE, IPAD, APPLETV, VISIONPRO) beside each
   game in the browser, carried in the room's name and the LAN host's name, not in HELLO: the link version stays 3
   and 0.4.x still lists and joins every game (docs/vr.md, "Online play and voice chat").*
4. **Steam Frame:** the APK in Lepton for now; a native SteamOS build is not planned.
5. **macOS:** in scope, after iOS. **Linux:** dropped.
6. **Distribution:** sideload only, as GoldenEye VR: GitHub releases, and SideStore for iOS.
