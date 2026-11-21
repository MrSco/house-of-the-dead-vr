# The House of the Dead (arcade) in VR: how it works and what was found

The aim: the arcade game, recompiled, in a Quest 3 headset in full 3D, with
online play and voice chat. The VR app (src/quest) and online play (src/net)
were first written for a VR port of the 1998 PC version and carried over;
this notebook follows the arcade version from its first frame on the desktop
(2026-11-15) on.

## The 3D is there

The geometrizer's output (`rt::GeoPoly`, src/runtime/video/geo.h) is still
3D: each vertex's x and y are the camera's times the focus, z is the depth,
and the renderers project it (`center + x / z`). With `GeoPoly::focus` the
camera-space point is `(x / fx, y / fy, z)`, x right, y up, z forward, the
same convention as the PC port's `VrScene` (main: src/d3d_vr.h), whose
Quest renderer already colours Model 2 textures per pixel
(`VR_TBLEND_ARCADE`).

## How much of the world the game sends

Test switches (environment, m2gpushot and the app):

- `M2_VR=1`: the geometrizer clips polygons only to just in front of the
  camera (`Geo::set_vr`), not to the screen's edges.
- `M2_ZOOMOUT=k`: the GPU renderer shows a view k times wider, no viewport
  clip.
- `M2_YAW=degrees`: the GPU renderer turns the camera (left +).

```
M2_VR=1 M2_ZOOMOUT=3 m2gpushot rom_cache 2400 --dump DIR --every 200 --inputs scripts/inputs/hotd_shoot.txt
```

Results (2026-11-15, Chapter 1's opening): the game sends the world well past
its picture. Three times wider, the courtyard is whole (the house, the wall,
the path, the sky). Turned 60 to 90 degrees the walls, the ground and the
zombies are still there, thinning out; behind (180 degrees) there is
nothing. That is the front half of a sphere, as with the PC port, where
the PC port's VR renderer filled the rest from earlier frames and extra passes.

## Speed on the Quest 3

m2run built for arm64 Android (NDK 28, `-G Ninja`, the toolchain file;
build_houseofthedead/android/hotd), run over adb from /data/local/tmp with the
ROM images: the 4,200 frames of scripts/inputs/hotd_shoot.txt in 48.5 s,
87 frames a second on one core, the sound board included, with the headset
asleep. The arcade runs at 57.52. The run executed the same 636,518,401 i960
instructions as on the PC. The game's idle loop is not found yet (every
frame runs to the instruction cap): more to gain.

## The Quest app (2026-11-15: it runs)

`python scripts/build_quest.py` builds com.mrsco.hotdvr ("House
of the Dead VR"): the Quest app (src/quest, compiled with `HOTD_ARCADE`), its
game the recompiled arcade game.

- src/core/arcade.cpp: SDL_main (the game thread, against
  host_sdl.c's SDL subset): rt::GameLoop on files/arcade/hotd.zip or
  hotd.7z (Revision A; the original revision's program ROMs differ), paced at
  57.52 Hz; each frame's polygons in the game camera's space as a VrScene
  (one batch: the guns' aim hits them) and an M2Scene (m2_vr.h) with their
  texture state; the tilemap layers as the overlay; the trigger, reload, and
  the skip button as two coins and Start; the SCSP's sound; the EEPROM and
  backup RAM saved in files/save. The rest of what the app calls into its
  game is stubbed (online play's lockstep among it).
- src/core/m2_gl.c: m2.hlsl's ps_poly in GLSL ES 3.0 (texture RAM,
  luma and the gamma'd colour translation as integer textures), in 3D.
- src/quest/quest_xr.c: m2_gl_init/sync/draw, the ROM
  set as the game files (ImportActivity's arcade mode), its setup page.

First run on the Quest 3: the attract mode in full 3D and stereo, textured as
the arcade's rasterizer does it; the game thread about 44% of a core (paced,
so full speed), the VR thread 18%.

## The whole world, all round

What seemed missing behind the player was the geometrizer's doing, not the
game's: it drops polygons behind the camera (the near-plane clip, and
check_culling's max z < 0 and master z clip). In VR mode (Geo::set_vr) it
keeps everything, unclipped; the headset's renderer clips to its own view.
The game's display list holds the whole area: turned 90, 180 and 270 degrees
(M2_YAW) the courtyard, the house, the road and the sky are all there. The
texture level comes from the distance (arcade_quest.cpp), not the arcade
camera's z. On the Quest 3: 120 frames a second, the GPU about 85% busy.

## What the game itself leaves out (2026-11-16)

The i960 builds the display list itself (every object command goes through the geometrizer port, written by
0x14120 or inline copies of it); the TGPx4 only does the maths. Two choices limit what it sends:

- **The area's list.** The scenery is drawn by 0x4b300..0x4b644, one list of pieces per area: the list at
  `[0x5430b0] + area * 50` (area: byte 0x51fdd4; bytes, 0xff ends it), each piece 24 bytes at
  `[0x5430b4] + i * 24` (model, x, y, z). Stage 1 has 45 areas; their lists overlap, hold alternative versions
  of some pieces and keep the mansion's inside apart from its outside. Area 63 lists every piece at once (both
  versions, inside and out): not usable as it is. Not changed.
- **The view test.** For each piece of the list the i960 sends the TGPx4 FIFO command 0x3d807b7b with the
  piece's centre and its model's bounding radius (`[0x2eec390 + model * 4] + 0x18`); at 0x4b484
  `cmpr g4,+0.0; ble` skips the piece unless the answer is above 0. This is what drops the mansion's wing when
  the gargoyle cut scene looks up (frame 4320 of the stage 1 script).

In VR (M2Board::set_vr, or M2_VR=1) the hook `hotd_scenery_cull` (seeds/hotd_hooks.txt, src/games/hotd.cpp)
makes that answer 1.0, so every piece of the area's list is sent. `M2_VR_ALLROUND=0` switches it off for
comparison. The same test (0x3d807b7b) is used in seven other places; they delete effects and actors that leave
the view, or start effects only on screen, which is game logic, so they are left alone.

Measured on the stage 1 script (24,000 frames, player 1 kept alive): the frames with 3D carry 1,357 polygons on
average before, 1,450 after (+6.8%; at most 2,784 before, 3,103 after); the game's own view (turned 0.01
degrees) is the same pixel for pixel, and without VR the picture is byte-identical. Game progress is the same
(the same phase changes; RAM the same except the wait-for-vblank's spare-time count, 0x51f9a4, and a table of
such counts at 0x51e530..0x51e66f). Time: +0.02 ms a frame on the desktop (TGPx4 +1.5% instructions).

Online: a VR game and a desktop guest without VR run different i960 paths, so those spare-time bytes differ and
the RAM checksum does too; both sides need the same setting (two headsets do).

## Online play and voice chat

src/core/arcade_net.cpp: a lockstep with the netplay.h API (the PC port's) and
bundle layout, so online.c (rooms on the lobby server, the built-in LAN host,
late joining, voice) carries it unchanged. The host (player 1) makes each
frame's cabinet input (both guns, both triggers, coins and Starts) from its
own controls and the guest's gun, sends it as that frame's bundle and runs the
frame with it; the guest (player 2) runs frame N with bundle N. Every 60th
bundle carries a checksum of main and work RAM (M2Board::state_hash). Player
2's gun: Inputs gun2_x / gun2_y, IN1 0x02, Start 0x20 (hotd.cpp, the IoBoard's
gun report). The arcade's rooms have their own game id ("ha", rooms.h), so it
never meets the PC game's.

The browser shows each game's platform (0.5.0): a tag after its name (QUEST,
FRAME, ANDROID, PC, PCVR, MAC, IPHONE, IPAD, APPLETV, VISIONPRO), which each
app's shell sets at its start (online_set_platform; the Quest and Frame by the
package, Windows by VR or the screen, iOS by the device). It rides in the free
text the server and the LAN beacon only pass along: a room is named "NAME
PLATFORM CODE" (24 characters, HELLO's limit: the name gives way, never the
code), a LAN host answers discovery as "NAME PLATFORM" (16). The code stays the
last word, where 0.4.x finds it for JOIN BY CODE and QUICK JOIN; 0.4.x shows
the platform as part of the name, and a 0.4.x host's game shows without a tag.
Checked on zonesync.net and the LAN with 0.4.2 both ways (browser, code, room
and IP joins: every checksum matched). PORT_ONLINE_LOG=1 logs the browser's
rows; QMENU_TEST_SCREEN=browse|lan opens that page with FLAT_MENU_AT.

Joining a game under way (link version 3, 2026-11-16): the guest doesn't run
the host's frames from its start any more. When it comes, the host saves its
game after the frame it is at (GameLoop::save_state, runtime/board/state.h:
every part's fields, the same bytes on every platform; 17 ms on the PC, 50 ms
on the Quest), packs it on a thread of its own (state_pack.h: the runs copied
from the ROMs, texture, character and sound RAM, go as references, the rest
LZMA: 8.7 MB to about 180 KB; 0.2 s on the PC, 0.5 s on the Quest) and sends
it in 'S' datagrams; the guest keeps every piece, acknowledges how far they run
without a gap, loads the state (60 ms on the Quest) and runs on from the host's
bundles. Through zonesync.net a join took 2.4-3 s from the session's start
(the old way: about 45 s for a game 3 minutes in, longer the longer it ran).
tools/m2statetest checks it: a loaded game against the one that saved, frame
for frame (checksums, screens, sound, and the states at the end), or a state
saved on another platform (crossplay_check.py does both ways with the Quest:
the PC's state and the Quest's of the same frame are the same bytes).

Tests:
- tools/m2nettest: host with two scripted players, record the bundles; replay
  them as the guest: 70 of 70 checksums (scripts/inputs/hotd_2p.txt).
- tools/m2netguest: a desktop player 2 over the real network (src/net,
  sdl_mini.cpp for its eight SDL calls). Against the Quest hosting on the LAN
  (env.txt PORT_ONLINE=host-lan): joined, caught up, 500 of 500 checksums,
  the Quest's arm64 game and the PC's x64 one frame for frame; its skip
  button brought player 2 in, who played on to the ranking. Voice chat
  switched on with the session (the Quest asked for the microphone); the
  desktop has no voice, so two headsets will tell how it sounds.

## Importing the ROM set

ImportActivity's arcade_game mode (the arcade app): hotd.zip or hotd.7z,
Revision A. A zip's program ROM is checked (the original revision, hotdo, is
refused with a message); a 7z by its signature (the game checks every ROM when
it starts). The set goes to files/arcade/hotd.zip or hotd.7z.

## Updates (src/quest/java: Updater, UpdateActivity; as the GoldenEye VR launcher's)

The app reads this repo's GitHub releases (the API's list, no token) once a launch and from the SYSTEM tab's CHECK FOR
UPDATES, and offers the newest one newer than its versionName that has its own APK: the Quest's
`house-of-the-dead-vr-X.apk`, the Steam Frame's `...-frame.apk`, the phones' `...-android.apk` (UpdateVersion: semver
order, a pre-release before its release). Drafts never; pre-releases only with TEST BUILDS on. UPDATE opens
UpdateActivity, a 2D panel in a process of its own: "Install unknown apps" if it isn't allowed yet, the download
(files/updates), its size and GitHub's SHA-256, then that it is this package, no older (versionCode) and signed with
this app's key, then a PackageInstaller session; the system asks you to confirm, and the update ends the app. The next
launch says "Updated to vX" once. Cancel abandons a session whose prompt was closed without an answer.

Publishing: a release as before (tag vX.Y.Z, its APK named house-of-the-dead-vr-X.Y.Z.apk); a test build as a GitHub
pre-release, tag vX.Y.Z-test.N, versionName X.Y.Z-test.N and a higher versionCode than the last build (Android takes no
lower one), its APK house-of-the-dead-vr-X.Y.Z-test.N.apk. All signed with the release key: another key's APK is
refused before the installer sees it. Tests: tools/updater_test (UpdateVersion on a desktop JVM); QUEST_TEST_UPDATE=
seconds in env.txt presses UPDATE (a build whose versionName is set back sees the latest release as an update);
FLAT_TEST_UPDATE shows the SYSTEM tab's line in the Windows app with a given status.

## The cabinet room

The VIEW tab's Cabinet room setting (QOPT_ROOM) picks what surrounds the cabinet view:

- **DARK**: the floor grid in the dark.
- **PASSTHROUGH**: your own room (XR_FB_passthrough). A reconstruction layer is submitted under the eyes' layer. The
  eyes' layer clears to alpha 0 and is blended by its alpha. Passthrough starts only while the cabinet view shows this
  room. The manifest asks for com.oculus.feature.PASSTHROUGH (not required).
- **COURTYARD** (the default): the mansion's courtyard, in front of the fountain, as a still 3D backdrop. It is the
  game's own data, so the app can't carry it. arcade_quest.cpp's make_courtyard makes it once on the headset, from the
  player's ROM set, on a spare core while you play: the cabinet room is dark for those first seconds. Two games side by
  side give the same frames (`M2_TWIN=1 m2gpushot ...` runs a second game on another thread: the dumps are unchanged).
  Warping to the fountain instead isn't possible without a saved state, which would be the game's data. The steps:
  1. A second game runs, without sound, to frame 4080 (about 15 s on the Quest 3). It gets two coins and Start, keeps
     player 1 alive (lives poked) and shoots every 15 frames, every 7th shot off the screen, as in
     scripts/inputs/hotd_courtyard.txt. Frame 4080 is in front of the fountain, the mansion behind it, Sophie fallen
     by the fountain just before the gargoyle takes her. (`M2_NOSOUND=1 m2gpushot ... --inputs
     scripts/inputs/hotd_courtyard.txt` shows the same frames; the sound board doesn't change them.)
  2. Its world is saved, everything round the camera, as VR mode keeps it (with all the scenery: the scenery hook
     below). So are the texture RAM, the luma and colour tables, and the sky's up: files/arcade/courtyard3.m2w. The
     earlier files (courtyard.m2w, courtyard2.m2w: the road outside the gate) are deleted.
  3. m2_gl.c draws it with the game's own shading. It is levelled by the saved up, with the camera at your eye height.
  4. A new ROM set's images make it again.

Player 2 online gets the cabinet's blue gun (arcade-gun-blue.tcgun, from export.py like the red one). Its cord runs to
the blue gun's holster, the model's left one.

## Your partner's body online (2026-11-21)

Online, a headset drew its partner as their guns and hands alone. Now they stand beside you as the game's own agent:
Rogan for player 1, G for player 2, their head, arms and legs following the player's.

**The figures are the game's.** Its attract intro shows both agents (Rogan in his coat about frame 2300, G in his
suit about 2500; the man in sunglasses after them is neither). The Model 2 has no skinning: an agent is 19 rigid
models, each drawn with a matrix of its own (the pelvis, two front flaps, per leg a thigh, shin, foot and toes, the
chest, Rogan's coat, the head, per arm an upper arm, forearm and hand), each followed by its shadow on the floor.
`tools/m2figure` lists a frame's models with their matrices and reads each back from the model ROM
(`rt::Geo::model`: all its polygons in its own space, where the frame's display list has only the ones facing the
camera; the geometrizer's object record now keeps the texture addresses and count that needs).

**One skeleton, exact.** Over 180 frames each child's origin sits at the same point of its parent's space to the
last digit, the same for both agents: every model's origin is its joint (src/xr/quest_avatar.c has the table). A
bone runs along its model's x; knees and elbows hinge about z. Lengths in game units: thigh 4.527, shin 4.755,
upper arm 3.093, forearm 2.629, waist to neck 3.8; the ankle is 0.56 over the floor, so the neck stands 17.3 high
and a unit is about 9 cm for a player of average height.

**Two things the display list doesn't give as models.** Rogan's coat is a mesh animated by the frame (50 models
0x241 words apart from 0x828661, looping every 100 frames): the figure wears the one drawn at its frame, tilted
back 0.1 rad so the seat of his trousers doesn't cut through it at rest. G's waist is not in the ROM at all: the
game joins his chest to his pelvis with 16 triangles it writes into polygon RAM every frame, from the pelvis's top
ring of points to the chest's bottom ring. `make_figures` reads that ring at G's frame and sorts its corners by
which model they belong to (the chest's are the chest model's own points); the renderer moves each with its own
part, so the ring stretches as the game's does.

**Made on the headset, like the courtyard.** The figures are the game's data, so the app can't carry them:
`make_figures` (arcade.cpp) runs a second game without sound through the attract mode to those two frames (2.4 s
on the PC), on the courtyard's thread after it, and writes files/arcade/figures3.m2f: each part's triangles
(M2Vert, with the polygon's normal and light), texture RAM (the same at both frames), the luma and the colour
tables. A new ROM set's images make it again. The flat apps don't make it (they never draw a partner).

**Drawn with the game's shading.** m2_gl.c's figure program is the world's fragment shader behind a vertex shader
that does the geometrizer's part as the figure turns: the polygon's luma from its normal (|n . light| x diffuse +
ambient, where the light and the eye are on the same side), and its back-face test (a polygon facing away goes to a
point). Finest texture level, real depth, after the world as the guns are.

**Posed from three points** (quest_avatar.c, `avatar_solve`): the head and the two wrists. The neck hangs under the
eyes by the head's own turn; the hips follow the neck with a lag, so a quick lean bends the spine and a step moves
the body; the body's heading follows the head's, drawn half way to where the hands are held; each arm is two bones
from shoulder to wrist with the elbow down, out and back; each foot stays planted and steps after its hip once it is
21 cm or 40 degrees away, one at a time; a head that comes down bends the knees. The figure's size is set by the
player's own (the neck's height standing: 8.8 to 11 cm a unit: never smaller than an adult of average height, whose arms a seated player's hands would outreach); a player lower than that for long (seated) gets a
figure standing with its feet under the floor rather than squatting for the whole game. tests/app/test_avatar.cpp
holds the solver to the skeleton in six moving poses (every joint where its parent has it, the eyes and wrists
where the pose has them, the feet on the floor).

**No mixing: a body has its own hands.** The game has two hand models: an open left hand (each agent's own;
fingers along +x, thumb +y, palm -z) and one right hand for both agents with the game's pistol in it (0x8e7f31, the
model after the right forearm's shadow: the wrist at the origin, the barrel along -y beside and over it, the gun's
top +z). On a figure, a hand with a gun is that model, its barrel on the player's aim (mirrored in a left hand); a
hand without is the open hand (the right: the left mirrored), fingers along the aim, thumb up. They sit at the
forearm's end, turned as the player's hand is (`avatar_wrist`, `avatar_hand`). The player's own gun model is not
drawn then: no slide, no flash, no magazine base, no fingers moving, and the pistol whatever gun they chose. An arm
with no hand to follow hangs with the open hand on it.

**The settings** (xr_shell.c `my_hands`, `my_body`, `draw_partner`):
- VIEW > Your body (`your_body`, on by default), online or not: your player's agent (Rogan for player 1 and alone,
  G for player 2) solved as a partner's is, from your head and what your hands hold, on the floor under you, drawn
  without its head (your eyes are inside it), with its own hands and pistol.
- HANDS > Your hands (`your_hands`; QPHANDS_*), for when Your body is OFF (the row is dimmed while it is on): MODERN,
  the app's hands and the gun you chose; ORIGINAL, the agent's hands and pistol alone at your controllers (9 cm to
  the game's unit); NONE, your gun alone.
- VIEW > Partner (`partner_body`; QPARTNER_*): BODY (the default), their figure with its own hands and pistol,
  whatever they have set; HANDS, no figure, their hands and gun alone as they have their own: each app sends how
  its player's hands are (the pose block's last byte: ORIGINAL with their body on, else their Your hands). An app
  that doesn't say (0.6.1) is MODERN. A partner on a screen shows only with BODY.
Until the figures' file is made, everything is the app's hands.

**The game's pistol in play.** It has no parts that move, so it does what the cabinet's gun does: the whole gun, the
hand with it, kicks back and up on a shot and comes back with the slide pulled by hand (`kick_pose`, quest_gun.c's
own numbers), and flashes at its muzzle (`qgun_draw_flash`). The magazine's base plate, the dropped magazine, where
a magazine goes in and where the sight starts are the pistol's own points (AVATAR_GUN_GRIP_*, AVATAR_GUN_MUZZLE_Z,
through `gun_grip_base`, `gun_muzzle_z`). A player's arm longer than the figure's grows to the wrist (up to 1.5:
`AvatarPose.stretch`), so the hand stays on the arm at full reach.

**The pose block** (xr_shell.c) grew by the head, 14 bytes after the two hands, and one byte for how the player
has their own hands (63 of ONLINE_POSE_MAX's 64), same version byte. 0.6.1 and before send 48 bytes and read no more of a longer block, so old and new still draw each
other's guns and hands; a new app gives an old one's player a head made up from where their hands are.

**A partner on a screen** (Windows, a Mac, a phone, a tablet, a TV) has no head or hands to send, and sends no
pose. They stand there all the same (`partner_stand_in`): the lockstep's own input says where their gun aims
(`host_player_aim`, the crosshair's pixel), so the figure holds the cabinet's gun in their colour two-handed at eye
height and aims it at that point (on the cabinet's screen in the cabinet view; along that pixel's ray into the
world in the immersive view), the head turned that way. Off the screen (their reload) or out of the game, the gun
is lowered and the left arm hangs. Their shots kick the slide and flash by the game's own count of them
(`host_player_events`). Nothing changes in the flat apps, and it works against 0.6.1 on a screen.

Tests on a PC without a headset (HOTDVR.exe, flat): `M2_FIGURES=1` makes the figures' file on a screen too, and
`FLAT_TEST_AVATAR=pose[,turn[,distance]]` draws both figures over the picture in one of six poses
(`FLAT_TEST_AVATAR_PARTS`: only some parts; `FLAT_TEST_AVATAR_HANDS=1`: the figures' own hands on the tracked
arms, the right one's with the gun; `FLAT_SHOT` for the picture). Checked that way: both figures textured and whole
from every side, standing, aiming, crouching, reaching, stepping about; the original hands in the aiming pose, the
pistol upright in the right hand and pointing ahead. **Not checked in a headset yet:** the partner beside you in VR,
where the forearm meets the hand model's sleeve, Your body (how the collar and chest sit under your eyes when you look down), the hands' and
Partner's choices there (the menu's rows among them), and the stand-in's aim against a real flat partner.

## In play or not (the game's RAM)

The reload gestures need to know when the game takes shots: not on the title screen, in the attract demo or in a
cut scene. I found the bytes with m2probe RAM dumps. First I took a dump at every trigger pull of a rapid-fire run and
labelled each pull by whether the bullet count dropped. Then I compared those dumps with others from an attract-mode
run and a run that ends at game over.

| Address | Meaning |
|---|---|
| 0x51F4A0 | game mode: 0 title or attract, 1 a game, 2 the attract demo (back to 0 at game over) |
| 0x51F4B4, 0x51F4B5 | phase for players 1 and 2: 4 a cut scene; 1, 2 and 3 play (taking shots) |
| 0x51F290, 0x51F35C | lives (0x51F294, long taken for player 2's, isn't: below) |
| 0x51F2C1, 0x51F38D | bullets |

In play means the player's state byte 5 (below; mode 1 at first), phase 1 to 3, and lives above 0. arcade.cpp's
update_ammo publishes it as host_player_playing. The magazine only drops in play.

Two players (the flat apps' local player 2, 2026-11-20: 512 KB dumps of 0x500000 on, in each state, diffed) showed
more. Player 2's block is 0xCC after player 1's: lives 0x51F290 / 0x51F35C, bullets 0x51F2C1 / 0x51F38D (0x51F294,
read as player 2's lives above, is something else: it stayed 3 through a demo where both players lost lives). Each
player has a state byte, 0x51F284 / 0x51F350: 9 not in the game (the title and attract, or a player 2 who never
joined), 5 in it (playing or in a scene), 4 out (the continue countdown, and after it while the other plays), 6 the
game over's ranking. 0x51F4A0 counts the players in the game: 2 while both are, so `mode == 1` above is false for
both in any two-player game, online too (the attract mode's gameplay demo has 2 there as well, with both state bytes
9). host_player_state publishes the state byte. Since then (the rumble work, 2026-11-20) host_player_playing goes by
the state byte instead of `mode == 1` (it read 0 for both players in every two-player game: on the headsets the
online reload hints and the magazine reload's drop never came), and player 2's lives are read at 0x51F35C. Checked in
a scripted two-player game: both players read playing, and both out of play together in a scene; three hits each
before each player's continue countdown (state 4), so 0x51F35C counts player 2's lives.

From the same bytes update_ammo counts each player's events for every shell's rumble (host_player_events): a
bullet less (a shot), the magazine back to 6 from less while in play (a reload), a life less (a hit), each only while
that player's state byte was 5 and only in steps of one (docs/platforms.md, "Rumble and trigger feedback").

The empty gun's message, "SHOOT OUTSIDE OF THE SCREEN !", is orange letters outlined in pure blue. For player 1 it
sits at x 34-189, y 217-254, under the RELOAD sign (x 34-197, y 163-211). With a reload gesture on, draw_shoot_outside
writes the gesture's step there instead.

## Next

1. Play it in the headset: the gun's aim and hits, reloads, the sound, the HUD
   overlay, the world's scale (QUEST_SCALE; 0.11 m a unit assumed, the PC
   port's), and the gaps in the sky dome's edge and below.
2. Two headsets online: the partner's figure, gun and hands in view, voice.
3. The game's idle loop (spin skip) for headroom.
