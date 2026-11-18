# Getting started

From a fresh checkout to the game on your Quest, your PC, a Mac, a phone or an iPhone. The README's
[Playing it](../README.md#playing-it) is the short version for players who download a release; this page is for
building it yourself.

Everything is driven from a Windows PC: it recompiles the game from your ROM set and builds the Quest, Android and
Windows apps. The Mac, iPhone and iPad apps are built on a Mac, from the PC's checkout.

## What you need

1. **Your own ROM set of The House of the Dead**, MAME's `hotd` (Revision A), as `hotd.zip` or `hotd.7z`. No game
   data comes with this project: the game is recompiled from your copy, and nothing derived from it is committed. A
   merged archive (with `hotdo` or `hotdp` inside) works too: the importer finds the files by their CRC.
2. **Windows 10 or 11** with winget (Microsoft Store's App Installer).
3. For the Quest, Steam Frame and Android phone apps: the **Android SDK** with an NDK, build-tools, a platform and
   platform-tools, and **Java 17** or later. Android Studio's SDK Manager installs them.
4. For the Apple apps: a **Mac** with Apple silicon, **Xcode**, **xcodegen** (`brew install xcodegen`) and **ANGLE**
   built in `~/angle` (as GoldenEye VR's builds use it), reachable from the PC over ssh.

## 1. Get the code and put your ROM set in place

    git clone https://github.com/MrSco/house-of-the-dead-vr.git
    cd house-of-the-dead-vr
    mkdir roms
    copy C:\path\to\your\hotd.zip roms\hotd.zip

Git ignores `roms/`, and everything made from it under `build_houseofthedead/`.

## 2. Set up the build tools

In PowerShell:

    powershell -ExecutionPolicy Bypass -File setup.ps1

It installs Git, CMake, Ninja, Python and the Visual Studio 2022 Build Tools with their C++ workload with winget,
then fetches the pinned libraries into `extern/` (SoftFloat, SDL 3, ymfm, the LZMA SDK, Dear ImGui) and builds and
tests the recompiler's tools in `build_houseofthedead/pc/hotd/`. Everything is built with MSVC, the compiler
HOTDVR.exe ships with; `setup.ps1 --clang` uses Visual Studio's clang-cl instead (it adds the Clang tools, and
Windows asks for permission: the Visual Studio Installer needs administrator rights). If it says Python "is not on
PATH yet", close PowerShell, open a new one and run the same command again.

If your ROM set is already in `roms/`, setup recompiles the game too, and step 3 is done.

## 3. Recompile the game

    python scripts/recompile.py

This imports your ROM set, recompiles the arcade's i960, 68000 and DSP programs to C++ and builds the desktop tools,
all under `build_houseofthedead/`. If the ROM set is wrong or incomplete, the `m2import:` line says which file is
missing; `hotd` must be Revision A, other sets have different programs. Run it again after pulling changes to the
recompiler or the game's hooks.

## 4. Build an app

**Quest** (headset connected over USB, developer mode on):

    python scripts/build_quest.py

It fetches the OpenXR loader, SDL2's headers and the voice libraries into `third_party/quest`, then builds, installs
and starts `hotdvr.apk`. `--apk-only` only builds it, `--no-launch` installs without starting it. The app asks for
the ROM set the first time, or push it yourself:

    adb push roms/hotd.zip /sdcard/Android/data/com.mrsco.hotdvr/files/arcade/hotd.zip

**Steam Frame and Android phones:** the same script with `--flavor frame` (`hotdvr-frame.apk`) or `--flavor phone`
(`hotdvr-android.apk`, 64-bit Android 10 or later).

**Windows:**

    python scripts/build_windows.py --run

It builds `build_windows/HOTDVR.exe` with Visual Studio's MSVC (strict floating point, so it computes the same game
as the Quest, bit for bit) and starts it. Its files folder, with the ROM set in `files/arcade/`, is beside the exe.
`--zip` makes the zip to hand out, with `SDL3.dll` for game pads.

**iPhone, iPad and the Mac:** on the PC, send the checkout, the generated code and the fetched libraries to the Mac:

    python scripts/apple_sync.py --host you@your-mac

Then, on the Mac, in the copy it made (`~/src/house-of-the-dead-vr` by default):

    python3 scripts/build_apple.py simulator --app     # the iOS Simulator
    python3 scripts/build_apple.py device --ipa        # an unsigned IPA for SideStore
    python3 scripts/build_apple.py mac                 # House of the Dead VR.app

## 5. Check crossplay

Online play is a lockstep: every app must compute the same game, bit for bit. After any change to the compiler, its
flags or the runtime, run:

    python scripts/crossplay_check.py

It records a game on the PC and replays it on the Quest, then the other way round, and compares the game's
checksums (30 of 30 is a pass). [platforms.md](platforms.md) has the details, and how the Mac's build was checked.

## Releases

    python scripts/release.py [--apple] [--publish]

From a clean checkout, it builds every platform's download from the same commit into `build_release/<version>/`,
writes their SHA-256 into `docs/releases/v<version>.md` and, with `--publish`, makes the GitHub release that the
apps' updater finds. The version is `src/quest/AndroidManifest.xml`'s `versionName`.

## Troubleshooting

**`m2import: missing ...`, then the ROM set is rejected.** The archive isn't MAME's `hotd` Revision A, or it is
incomplete. Check the name (`roms/hotd.zip`, not `hotd.zip.zip`: Windows can hide the extension).

**`build_quest.py` can't find the SDK or the NDK.** Set `ANDROID_HOME` (or `ANDROID_SDK_ROOT`) to the SDK, and
install an NDK from the SDK Manager.

**`build_windows.py` can't find Visual Studio.** It needs the C++ tools (the "Desktop development with C++"
workload, or the Build Tools' C++ workload); the newest Visual Studio is best, for C11 atomics.

**Online play desyncs between two platforms.** Only builds with the same online version share a room, so a desync
means the two compute the game differently. If you built one yourself, run the crossplay check above.

**Starting again from scratch.** Delete `build_houseofthedead/` and recompile; your ROM sets in `roms/` are kept.
Each app keeps its settings (`vr-options.cfg` or `options.cfg`), saves and its copy of the ROM set in its files
folder:

- Quest, Steam Frame, Android: `/sdcard/Android/data/<package>/files` (`com.mrsco.hotdvr`, `.frame`, `.android`)
- Windows: `files` beside `HOTDVR.exe`
- Mac: `~/Library/Application Support/House of the Dead VR`
- iPhone and iPad: the app's Documents folder (the Files app shows it)
