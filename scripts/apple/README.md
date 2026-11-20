# Mac helpers: TestFlight, dev installs, test runs

Shell scripts for the Mac that builds the Apple apps (`docs/platforms.md`: P3, P5, Apple TV). Each builds from a
checkout `scripts/apple_sync.py` made (`--dest src/<any folder>`): `-C DIR`, or the current folder, or else the
checkout the script is in. Run them with `bash` (a synced copy may not keep the execute bit):

| Script | What it does |
| --- | --- |
| `testflight.sh ios\|tvos\|visionos BUILD [--upload]` | The App Store archive and .ipa with that build number (`build_apple.py`, `build_tvos.py`, `build_visionos.py` `device --archive`); `--upload` sends it to TestFlight and moves the archive (its dSYMs) to `$HOTD_ARCHIVES/<platform>/<version>-<build>` |
| `testflight.sh ios\|tvos\|visionos next` | The next build number for that platform: the highest App Store Connect has, plus one (numbers rise per platform) |
| `desktop.sh [--timeout S] COMMAND...` | Runs a command in the Mac's desktop session (a temporary `.command` opened in Terminal), from ssh: waits, shows its output, returns its exit code. For what needs the logged-in keychain |
| `dev-install.sh [--build-only] [--launch] DEVICE` | A development build of the Apple TV app signed for one paired Apple TV (the account's App Store Connect key does the profiles and registers the device), installed with `devicectl` |
| `cycle.sh DEVICE TAG [SECONDS]` | `dev-install.sh --launch`, a wait (60 s), then the app's `port.log` to `pulled/TAG.log` |
| `run-env.sh DEVICE ENVFILE TAG SECONDS [SHOT_AT...]` | The installed app with ENVFILE as its `env.txt` (the test switches), started afresh; screenshots at those seconds (`shots/TAG_<s>.png`), then `pulled/TAG.log` |
| `asc.sh METHOD PATH [BODY]` | The App Store Connect API with the account's key |

`testflight.sh` runs over plain ssh for all three platforms (manual signing, the signing keychain unlocked with its
password file), screen locked or not. Only the build in `dev-install.sh` (Xcode's automatic signing for a device) goes
through `desktop.sh` by itself over ssh; for that the Mac's user must be logged in at the desktop, screen unlocked.

    # on the PC
    python scripts/apple_sync.py --dest src/house-of-the-dead-vr
    ssh $HOTD_MAC 'cd ~/src/house-of-the-dead-vr && bash scripts/apple/testflight.sh tvos next'
    ssh $HOTD_MAC 'cd ~/src/house-of-the-dead-vr && bash scripts/apple/testflight.sh tvos 12 --upload'
    ssh $HOTD_MAC 'cd ~/src/house-of-the-dead-vr && bash scripts/apple/dev-install.sh --launch 4k'

`python scripts/release.py --testflight` does the TestFlight part of a release for all three platforms.

## The settings (outside this repository)

Nothing of the account lives here. The scripts source a shell file, `$HOTD_TOOLS_ENV` (default
`~/src/hotd-tools/env.sh`), which exports:

| Variable | What it is |
| --- | --- |
| `HOTD_TEAM` | The Apple developer team id |
| `HOTD_KEYCHAIN`, `HOTD_KEYCHAIN_PASS` | The keychain with the Apple Distribution identity, and a file with its password |
| `HOTD_ASC_KEY_DIR`, `HOTD_ASC_KEY_ID`, `HOTD_ASC_ISSUER` | The App Store Connect API key: the folder with `AuthKey_<id>.p8`, its id, the issuer id (or a file with it) |
| `HOTD_ASC_APP_ID` | The app's App Store Connect id (for `testflight.sh ... next`) |
| `HOTD_PROFILE_IOS`, `HOTD_PROFILE_TVOS`, `HOTD_PROFILE_VISIONOS` | Each platform's App Store provisioning profile, by name (the iOS one serves visionOS too) |
| `HOTD_ARCHIVES` | Where uploaded builds' archives go (by platform, then version and build) |
| `HOTD_DEVICE_<NAME>` | Device names for the scripts' DEVICE (`HOTD_DEVICE_4K=<udid>` makes `4k` work) |
| `HOTD_DEVICE_BUSY` | Optional: a regex of processes that mean someone else is using a device (the scripts then refuse it without `--force`) |
| `HOTD_ASC_SWIFT` | Optional: the App Store Connect helper (default `hotd-asc.swift` beside the settings file) |
| `HOTD_TEST_OUT` | Optional: where `pulled/` and `shots/` go (default the settings file's folder) |

It must not set `HOTD_PROFILE` or `HOTD_BUNDLE_ID` (the scripts set the profile per platform; the bundle id is the
app's own).
