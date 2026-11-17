#!/usr/bin/env python3
"""Crossplay check: every app must compute the same game, bit for bit, or online play desyncs (docs/platforms.md,
"The one thing that decides crossplay"). m2nettest records a host run's bundles (the cabinet input of each frame,
the game's state checksum every 60th) on one machine and replays them as the guest on another, which compares the
checksums: any difference in the game's RAM shows as a mismatch. Run it before every release and after any compiler,
flag or runtime change.

  python scripts/crossplay_check.py [--frames 1800] [--skip-build]

Both ways between this PC (Windows x64, MSVC as HOTDVR.exe) and the Quest over adb (arm64): the PC hosts and the Quest replays, then the
Quest hosts and the PC replays. Needs:
  - the Quest build configured: python scripts/build_quest.py --apk-only (its CMake folder builds m2nettest too);
  - the Windows app's build configured: python scripts/build_windows.py (its folder builds m2nettest with the same
    compiler and flags as HOTDVR.exe);
  - the ROM set's images: the PC's from the recompile (--pc-images), the headset's where the app keeps them.
ANDROID_SERIAL picks the headset when more than one device is attached.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_quest  # noqa: E402
import build_windows  # noqa: E402
import quest_tools as port  # noqa: E402

ANDROID_BUILD = os.path.join(ROOT, 'build_houseofthedead', 'android', build_quest.SET)
PC_BUILD = build_windows.BDIR
PC_IMAGES = os.path.join(ROOT, 'build_houseofthedead', 'recomp', build_quest.SET, 'rom_cache')
QUEST_IMAGES = '/sdcard/Android/data/%s/files/arcade/images' % build_quest.PKG
QUEST_DIR = '/data/local/tmp/m2det'
INPUTS = os.path.join(ROOT, 'scripts', 'inputs', 'hotd_2p.txt')
OUT = os.path.join(ROOT, 'build_houseofthedead', 'crossplay')


def cmake_build(bdir, extra=()):
    cmake, ninja_dir = build_quest.cmake_tools()
    env = dict(os.environ)
    if ninja_dir: env['PATH'] = ninja_dir + os.pathsep + env['PATH']
    subprocess.check_call([cmake, '--build', bdir, '--target', 'm2nettest'] + list(extra), env=env)


def pc_exe():
    for p in (os.path.join(PC_BUILD, 'Release', 'm2nettest.exe'), os.path.join(PC_BUILD, 'm2nettest.exe'),
              os.path.join(PC_BUILD, 'm2nettest')):
        if os.path.exists(p): return p
    sys.exit('crossplay_check: no m2nettest in %s' % PC_BUILD)


def result(text):
    """m2nettest's last line: (checksums matched, desynced)."""
    line = [l for l in text.splitlines() if l.startswith('m2nettest:')]
    if not line: return 0, True
    words = line[-1].replace(',', ' ').split()
    n = int(words[words.index('checksums') - 1]) if 'checksums' in words else 0
    return n, 'DESYNC' in line[-1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', type=int, default=1800, help='frames per run (a checksum every 60)')
    ap.add_argument('--pc-images', default=PC_IMAGES, help='the ROM set\'s images on this PC')
    ap.add_argument('--skip-build', action='store_true', help='use the m2nettest builds there are')
    a = ap.parse_args()
    adb = port.find_android()['adb'] or shutil.which('adb')
    if not adb: sys.exit('crossplay_check: no adb')
    if not os.path.isdir(ANDROID_BUILD): sys.exit('crossplay_check: run python scripts/build_quest.py --apk-only first')
    if not os.path.isdir(PC_BUILD): sys.exit('crossplay_check: run python scripts/build_windows.py first')
    if not a.skip_build:
        cmake_build(ANDROID_BUILD)
        subprocess.check_call(['cmake', '--build', PC_BUILD, '--target', 'm2nettest'], env=build_windows.vs_env())
    exe = pc_exe()
    os.makedirs(OUT, exist_ok=True)
    env = dict(os.environ, MSYS_NO_PATHCONV='1')

    def sh(cmd):
        p = subprocess.run([adb, 'shell', 'cd %s && %s' % (QUEST_DIR, cmd)], env=env, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True)
        return p.stdout + p.stderr

    def pc(*args):
        p = subprocess.run([exe, a.pc_images, str(a.frames)] + list(args), capture_output=True, text=True)
        return p.stdout + p.stderr

    subprocess.check_call([adb, 'shell', 'mkdir -p ' + QUEST_DIR], env=env)
    subprocess.check_call([adb, 'push', os.path.join(ANDROID_BUILD, 'm2nettest'), INPUTS, QUEST_DIR + '/'], env=env,
                          stdout=subprocess.DEVNULL)
    subprocess.check_call([adb, 'shell', 'chmod 755 %s/m2nettest' % QUEST_DIR], env=env)

    expect = a.frames // 60
    runs = []
    # the PC hosts, the Quest replays
    pc_bundles = os.path.join(OUT, 'pc.bundles')
    port.log('PC host, %d frames' % a.frames)
    pc('--host', pc_bundles, '--inputs', INPUTS)
    subprocess.check_call([adb, 'push', pc_bundles, QUEST_DIR + '/'], env=env, stdout=subprocess.DEVNULL)
    port.log('Quest guest')
    runs.append(('PC x64 host -> Quest arm64 guest', result(sh('./m2nettest %s %d --guest pc.bundles' % (QUEST_IMAGES, a.frames)))))
    # the Quest hosts, the PC replays
    port.log('Quest host, %d frames' % a.frames)
    sh('./m2nettest %s %d --host quest.bundles --inputs hotd_2p.txt' % (QUEST_IMAGES, a.frames))
    quest_bundles = os.path.join(OUT, 'quest.bundles')
    subprocess.check_call([adb, 'pull', QUEST_DIR + '/quest.bundles', quest_bundles], env=env, stdout=subprocess.DEVNULL)
    port.log('PC guest')
    runs.append(('Quest arm64 host -> PC x64 guest', result(pc('--guest', quest_bundles))))

    ok = True
    for name, (n, desync) in runs:
        good = n == expect and not desync
        ok &= good
        print('%-34s %d of %d checksums matched%s' % (name, n, expect, '' if good else '  FAILED'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
