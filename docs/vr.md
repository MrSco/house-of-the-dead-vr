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

## In play or not (the game's RAM)

The reload gestures need to know when the game takes shots: not on the title screen, in the attract demo or in a
cut scene. I found the bytes with m2probe RAM dumps. First I took a dump at every trigger pull of a rapid-fire run and
labelled each pull by whether the bullet count dropped. Then I compared those dumps with others from an attract-mode
run and a run that ends at game over.

| Address | Meaning |
|---|---|
| 0x51F4A0 | game mode: 0 title or attract, 1 a game, 2 the attract demo (back to 0 at game over) |
| 0x51F4B4, 0x51F4B5 | phase for players 1 and 2: 4 a cut scene; 1, 2 and 3 play (taking shots) |
| 0x51F290, 0x51F294 | lives |
| 0x51F2C1, 0x51F38D | bullets |

In play means mode 1, phase 1 to 3, and lives above 0. arcade_quest.cpp's update_ammo publishes it as
host_player_playing. The magazine only drops in play.

The empty gun's message, "SHOOT OUTSIDE OF THE SCREEN !", is orange letters outlined in pure blue. For player 1 it
sits at x 34-189, y 217-254, under the RELOAD sign (x 34-197, y 163-211). With a reload gesture on, draw_shoot_outside
writes the gesture's step there instead.

## Next

1. Play it in the headset: the gun's aim and hits, reloads, the sound, the HUD
   overlay, the world's scale (QUEST_SCALE; 0.11 m a unit assumed, the PC
   port's), and the gaps in the sky dome's edge and below.
2. Two headsets online: the partner's gun in view, voice.
3. The game's idle loop (spin skip) for headroom.
