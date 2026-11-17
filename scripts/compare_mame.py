#!/usr/bin/env python3
"""The original game (MAME) beside ours, frame for frame: for finding graphical differences (a figure behind a wall,
a missing texture) and telling the game's from a renderer's.

  python scripts/compare_mame.py --frames 1900,1950,2000 [--inputs scripts/inputs/X.txt] [--depth order|key] [--out DIR]

(m2run dumps every n-th frame up to the last, n the frames' greatest common divisor: pick round numbers.)

For each of our frames: MAME's matching frame (found by picture among those around it: MAME's frame count runs about
235 ahead of ours, drifting a few), our software rasterizer's (m2run: MAME's own algorithm), and the Windows app's
(m2_gl, as the apps draw: --depth order is the flat apps' arcade order, key the headset's depth), with where the
app differs from MAME marked. One sheet per frame in --out (build_houseofthedead/cmp/compare by default).

Needs: MAME 0.289 in build_houseofthedead/mame (mame.exe, roms/hotd.7z: the official release, unpacked), the
Windows build (python scripts/build_windows.py; m2run: cmake --build its folder --target m2run), the ROM set's
images from the recompile, Pillow and NumPy. With an input script MAME's settings are made ours (USA, red blood:
HOTD_POKE in tools/mame-plugins/hotd_shots.lua; its Japanese ones add a briefing to the game); the attract runs
frame for frame without. The app runs from a clean copy of its files (no saved settings, Brightness 0%).
"""
import argparse
import functools
import math
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, 'build_houseofthedead')
MAME = os.path.join(B, 'mame')
WIN = os.path.join(ROOT, 'build_windows')
IMAGES = os.path.join(B, 'recomp', 'hotd', 'rom_cache')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_windows  # noqa: E402


def rgb(path):
    return Image.fromarray(np.fromfile(path, dtype=np.uint8).reshape(-1, 496, 4)[:, :, [2, 1, 0]])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', required=True, help='our frames, comma separated')
    ap.add_argument('--inputs', default='', help='an input script (none: the attract mode)')
    ap.add_argument('--offset', type=int, default=235, help="MAME's frame for our frame 0, about")
    ap.add_argument('--search', type=int, default=12, help='MAME frames looked at either side')
    ap.add_argument('--depth', choices=('order', 'key'), default='order')
    ap.add_argument('--out', default=os.path.join(B, 'cmp', 'compare'))
    a = ap.parse_args()
    frames = sorted(int(f) for f in a.frames.split(','))
    inputs = os.path.abspath(a.inputs) if a.inputs else ''
    work = os.path.join(a.out, 'work')
    for d in ('mame', 'sw', 'app'):
        os.makedirs(os.path.join(work, d), exist_ok=True)

    # MAME: every frame around each of ours
    mframes = sorted({f + a.offset + k for f in frames for k in range(-a.search, a.search + 1)})
    snaps = os.path.join(work, 'mame', 'snap')
    if os.path.isdir(snaps):
        shutil.rmtree(snaps)
    env = dict(os.environ, HOTD_SHOTS=','.join(map(str, mframes)), HOTD_INPUTS=inputs, HOTD_OFFSET=str(a.offset),
               HOTD_POKE='51f729=1,51f72d=0' if inputs else '')   # (the attract runs as MAME's settings make it)
    subprocess.run([os.path.join(MAME, 'mame.exe'), 'hotd', '-rompath', 'roms', '-video', 'none', '-sound', 'none',
                    '-nothrottle', '-skip_gameinfo', '-snapshot_directory', snaps, '-autoboot_script',
                    os.path.join(ROOT, 'tools', 'mame-plugins', 'hotd_shots.lua')], cwd=MAME, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    mame = {f: np.asarray(Image.open(os.path.join(snaps, 'hotd', '%04d.png' % i)).convert('RGB'), dtype=np.float32)
            for i, f in enumerate(mframes)}

    # ours, the software rasterizer (m2run dumps every n-th frame: n the frames' greatest common divisor; the
    # others are dropped)
    sw = os.path.join(work, 'sw')
    m2run = os.path.join(build_windows.BDIR, 'm2run.exe')
    every = functools.reduce(math.gcd, frames)
    args = [m2run, IMAGES, str(frames[-1] + 1), '--dump', sw, '--every', str(every)]
    if inputs:
        args += ['--inputs', inputs]
    subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    for n in os.listdir(sw):
        if n.endswith('.rgb') and int(n[4:9]) not in frames:
            os.remove(os.path.join(sw, n))

    # ours, the app: a clean copy of it
    app = os.path.join(work, 'app')
    shutil.copy(os.path.join(WIN, 'HOTDVR.exe'), app)
    for dll in os.listdir(WIN):
        if dll.endswith('.dll'):
            shutil.copy(os.path.join(WIN, dll), app)
    files = os.path.join(app, 'files')
    if os.path.isdir(files):
        shutil.rmtree(files)
    os.makedirs(files)
    shutil.copytree(os.path.join(WIN, 'files', 'arcade'), os.path.join(files, 'arcade'))
    open(os.path.join(files, 'options.cfg'), 'w').write('brightness_level_pct=0\n')
    env = dict(os.environ, M2_SHOTS=','.join(map(str, frames)), FLAT_QUIT_AT=str(frames[-1] / 57.5 + 8),
               M2_GL_ORDER='1' if a.depth == 'order' else '0')
    if inputs:
        env['M2_INPUT_SCRIPT'] = inputs
    subprocess.run([os.path.join(app, 'HOTDVR.exe')], cwd=app, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)

    W, H = 496, 384
    for f in frames:
        s = rgb(os.path.join(sw, 'run_%05d.rgb' % f))
        s32 = np.asarray(s, dtype=np.float32)
        near = [k for k in mame if abs(k - f - a.offset) <= a.search]
        diff, mf = min((float(np.abs(mame[k] - s32).mean()), k) for k in near)
        m = Image.fromarray(mame[mf].astype(np.uint8))
        shot = os.path.join(files, 'shot_%05d.ppm' % f)
        p = Image.open(shot).convert('RGB').resize((W, H), Image.BOX) if os.path.exists(shot) else Image.new('RGB', (W, H))
        mark = np.asarray(p).copy()
        mark[np.abs(np.asarray(p).astype(int) - mame[mf].astype(int)).sum(2) > 90] = [255, 0, 255]
        sheet = Image.new('RGB', (W * 4, H + 16), 'black')
        for c, im in enumerate((m, s, p, Image.fromarray(mark))):
            sheet.paste(im, (c * W, 16))
        ImageDraw.Draw(sheet).text((4, 2), 'MAME %d | ours %d: software | the app (%s) | the app, differences from MAME '
                                   'marked (MAME to software: %.2f)' % (mf, f, a.depth, diff), fill='yellow')
        sheet.save(os.path.join(a.out, 'frame_%05d.png' % f))
        print('frame %d: MAME %d, software differs by %.2f' % (f, mf, diff))
    print('sheets in', a.out)


if __name__ == '__main__':
    main()
