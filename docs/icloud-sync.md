# iCloud sync (the Apple apps)

User, 2026-11-24: GoldenEye VR's iCloud sync, for this project too. Load the ROM set on one Apple device and have the
settings, the high scores and the set itself follow on the others. iPhone and iPad (`src/flat/ios`), Apple TV
(`src/flat/tvos`) and Vision Pro (`src/visionos`) are one App Store Connect app (`com.mrsco.hotdvr`), so one iCloud
store reaches all three. The Mac app ships as a GitHub zip (Developer ID, no App Store profile) and has none; Windows,
Android and the headsets are untouched (`src/core/hotd_icloud.h`'s calls are empty there).

Code: [src/platform/apple/icloud_apple.mm](../src/platform/apple/icloud_apple.mm), interface
[src/core/hotd_icloud.h](../src/core/hotd_icloud.h). It is GoldenEye VR's `port/ios/ios_icloud.mm`, with what its
first week taught (below, "Built in from the start").

## What moves, and how

| What | Where in iCloud | When |
| --- | --- | --- |
| `options.cfg` (flat apps), `vr-options.cfg` (Vision Pro) | key-value store `opt1` (shared) + `opt1@ios` / `@tv` / `@vision` (that kind of device's) | merged at the start; a change goes up within 20 s, and on going to the background |
| `online.cfg` (name, lobby server, voice) | `onl1` + `onl1@<family>` | the same |
| `save/` (the game's settings EEPROM and backup RAM: its options and high scores) | `save1`: `{ f: { file: bytes }, t: time }` | merged at the start; the game's writes go up within 20 s |
| The ROM set (`arcade/hotd.zip` or `hotd.7z`) | CloudKit private database, zone `HotdFiles` | at the start, back in the foreground, after a set is chosen or sent, every 15 min |

tvOS has no iCloud Drive (Apple QA1935), so the set goes through CloudKit on every platform. The key-value store holds
1 MB and 1024 keys: the settings are four dictionaries and the saves one (about 17 KB), not a key per setting.
`online-save/` (online play's fresh saves) never moves.

**Shared or the device's.** Only these keys are shared: `rumble`, `pad_trigger_feedback`, `music_volume`,
`effects_volume`, `voice_chat_volume`, `crash_reports`; in `online.cfg` `name`, `server`, `voice_volume`. Every other
key belongs to its kind of device (iPhone and iPad are one kind), and so does any key added later unless it is named
there: the game's volume (a phone's speaker and a living room's TV want their own, GoldenEye VR's lesson), the
picture, crosshair, brightness, gyro and remote rows, every headset row (the gun's fit, the view, the cabinet's place,
resolution, refresh), the LAN address, the mic, and the iCloud row itself.

**Joining.** A device pushes nothing to the key-value store until it joins: when the store already has the other
devices' settings (they win; only keys the cloud lacks go up), or when CloudKit has no `joined` record (it is the
first device; its settings go up and it leaves the record). Otherwise it waits; settings that arrive late are used from
the next start. Whether this device has joined is kept in the app's defaults (`hotd.icloud.joined1`), which outlive
tvOS emptying the caches.

**Settings** merge key by key against `.hotd-icloud/last-*` (the copy last synced): a key changed here goes up,
otherwise the cloud's comes down. Without that copy (a first sync, or tvOS emptied the caches) the cloud's values win.
A setting changed on another device while this one runs applies at its next start.

**Saves are never merged** (GoldenEye VR's owner: choose the right one). The two files are one set. Against the set
last synced (its sum in the app's defaults, `hotd.icloud.savesum1`): changed on one side only, it moves that way;
changed on both, the one the game wrote last wins, by the files' own time, carried in the store and put back on the
files when they come down. A device joining takes the cloud's set, so a new install's fresh scores never go over a
played one's. The sum, not the time, decides the one-sided case because `tv_store.m` puts the saves back with new
times after tvOS empties the caches. A replaced set is kept in `.hotd-icloud/backups/save-<date>/`, the last five.

**The ROM set** is one item, `rom`: a `HotdItem` record (`i-rom`: its file name, stamp, size, SHA-256, chunk count,
generation) and `HotdChunk` records of 32 MB, each one asset holding a slice of the file. Chunks go first and the item
last, so a reader never sees half an upload; a re-upload is a new generation and the old chunks are deleted after.
The item is read before and after an upload: if another device put the same content there meanwhile, ours is dropped.
Chunks no item names and a day old are deleted on each pass. A download is checked against the SHA-256 before it is
used, and goes in as `hotd.zip` or `hotd.7z` with the other kind removed (the game takes `hotd.zip` first: one set).
A download while a set is here waits in `.hotd-icloud/ready/` and is put in place at the next start. Uploads go on
Wi-Fi only (a set is 30 to 60 MB); downloads may use cellular, since the player is waiting for them. Nothing is deleted
from iCloud when a device removes its set.

**Already here stays.** `.hotd-icloud/files.txt` (the item's stamps as last synced) starts with `#cloud <id>`, the id
of the CloudKit database it was synced with: a `HotdJoined` record `files-db` holding a random id, made by the first
device to look. Development builds and TestFlight's use two databases, so after a switch every item counts as never
synced. A set a device has the first time it syncs with a database is kept as it is and recorded as synced, whatever
iCloud's copy holds: nothing already installed is downloaded again. Only a missing set comes down, and after that
first sync a set changed on another device does.

## On each device

- **The menu**: SYSTEM's last row, **iCloud sync** (ON), shown only in a build signed for iCloud, and the status line
  beside CHANGE ROM SET ("iCloud: up to date", "iCloud: downloading hotd.zip 40%", "iCloud: not signed in"). OFF
  stops everything on this device from then on; turned on again, the set moves at once and the settings and saves at
  the next start.
- **iPhone and iPad** (`main_ios.m`): "Checking iCloud..." while the start merges (a first sync may wait up to 5 s for
  the store, and up to 12 s more for CloudKit's `joined` record). Without a set: "Looking for your ROM set in
  iCloud...", then its download's progress, and the game starts when it is in; **Choose the file instead** opens the
  picker, as does iCloud having none. A set chosen in the picker goes up.
- **Apple TV** (`main_tvos.m`): the address page (its QR code, the LAN upload) shows as before when the set is missing,
  and iCloud's progress takes its status line while the set comes down; whichever arrives first starts the game. So
  when tvOS empties the caches, the set comes back from iCloud by itself. `tv_store.m`'s copy of the settings and
  saves in the app's defaults stays, as the local backup. A set sent from a phone goes up.
- **Vision Pro** (`xr_vos.m`): the setup page says "Looking for your ROM set in iCloud..." and the download's
  progress, and the game starts when it is in; pinching still opens the picker.

## Built in from the start (GoldenEye VR's first week)

- The joining rule (2026-11-22: a new iPad whose store hadn't come down in 5 s pushed its own settings over the
  iPhone's).
- `files.txt` per CloudKit database, and a device's own set kept on its first sync with one (2026-11-23: TestFlight
  installed over development builds downloaded packs the iPhone and Apple TV already had).
- The game's volume per device (2026-11-24: a phone's speaker and a living room's TV want their own).
- `aps-environment` beside iCloud on Apple TV (App Store Connect error 90575 refused GoldenEye VR's tvOS 9).
- Capability changes make the App Store profiles INVALID: remake them before the next upload (below).

## Turning it on (the Apple developer account, once)

Builds are unchanged until a build script runs with `HOTD_ICLOUD=1` on a signed build (a device build with
`DEVELOPMENT_TEAM`, or `--archive`); the simulator never gets it.

1. **Done (2026-11-24, the owner):** the container `iCloud.com.mrsco.hotdvr`, made and assigned to the App ID
   `com.mrsco.hotdvr` (the App ID's iCloud capability, CloudKit).
2. **Done (2026-11-24, App Store Connect API):** the App ID's **Push Notifications** capability, for the Apple TV's
   `aps-environment` (without it an upload fails, error 90575).
3. **Done (2026-11-24):** the App Store profiles made again with the same names, now carrying iCloud and push: "House
   of the Dead VR" (`IOS_APP_STORE`; it serves visionOS too) and "House of the Dead VR tvOS" (`TVOS_APP_STORE`).
   Changing the App ID's capabilities makes them **INVALID** (an INVALID profile fails the archive), so they are made
   again after any such change and installed on the Mac (`~/Library/MobileDevice/Provisioning Profiles`).
4. **Done (2026-11-24):** an Ad Hoc build ("HOTD Ad Hoc iOS" / "HOTD Ad Hoc
   tvOS" profiles, export method `release-testing`, `iCloudContainerEnvironment` Development: a distribution profile
   allows both environments, so this needs no development certificate) synced on an iPhone 16 Pro Max and the Living
   Room Apple TV 4K. It made the `HotdItem`, `HotdChunk` and `HotdJoined` record types in the Development
   environment; the owner deployed them to Production the same day (CloudKit Console, **Deploy Schema Changes**). TestFlight builds use
   Production; development builds use Development, so the two never see each other's files (the key-value store has
   no such split). The sync fetches records by name and by zone changes, never by query, so it needs no indexes.
5. Build with `HOTD_ICLOUD=1` (`scripts/build_apple.py`, `build_tvos.py`, `build_visionos.py`;
   `scripts/apple/testflight.sh` passes the environment on). An archive's export options get
   `iCloudContainerEnvironment` Production.

## Tests

- `bash tests/apple/test_icloud.sh [simulator]` (Mac, iOS simulator, no account): the settings files' rewrite and
  merge, which keys are shared, the saves' choice (joining, one side changed, both, the Apple TV's restored saves),
  the save folder's scan and backups, the ROM set's files (hotd.zip before hotd.7z, its hash kept by stamp, put in
  place with the other kind removed, a waiting download, a bad name refused), the set's moves, `files.txt` per database.

## State

- 2026-11-24, uncommitted on `t3/adaptive-triggers-icloud-sync`: the code, the menu row, the build switch and the test.
  `tests/apple/test_icloud.sh` passes (47 checks). The iPhone, Apple TV and Vision Pro simulator apps build with it.
  Run signed on devices, 2026-11-24 (Ad Hoc, Development environment), started seconds apart on an iPhone 16 Pro
  Max and an Apple TV 4K, both with hotd.7z: both joined as the first device (the key-value store was empty on
  both); both made `files-db` and the Apple TV's won (the iPhone read it on its second look); both uploaded the
  38 MB set, the Apple TV finished first, and the iPhone found the same content in iCloud and kept that copy.
- Open: deleting from iCloud; settings changed elsewhere apply only at the next start.
