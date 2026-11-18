#!/usr/bin/env python3
"""The Apple builds' sources onto the Mac (docs/platforms.md, P3): the Apple apps are built there (Xcode), from this
checkout as it is, with what git doesn't hold.

  python scripts/apple_sync.py [--host USER@MAC] [--dest src/house-of-the-dead-vr] [--images]

The Mac is --host, or the HOTD_MAC environment variable (user@host, as ssh takes it).

Sends over ssh, as one tar stream: the tracked files (and new ones git doesn't ignore), the generated code
(build_houseofthedead/recomp/hotd/gen), the libraries the builds fetch (extern: SoftFloat, LZMA, ymfm;
third_party/quest: SDL2's headers, Opus, SpeexDSP, RNNoise). --images: the ROM set's images too
(build_houseofthedead/recomp/hotd/rom_cache, for m2nettest on the Mac). Nothing of the player's ROM leaves the
user's own machines.
"""
import argparse
import io
import os
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTRA = ['build_houseofthedead/recomp/hotd/gen', 'extern/softfloat', 'extern/lzma', 'extern/ymfm',
         'third_party/quest/SDL2-2.32.10-headers', 'third_party/quest/opus-1.5.2', 'third_party/quest/speexdsp-1.2.1',
         'third_party/quest/rnnoise-0.2']
IMAGES = 'build_houseofthedead/recomp/hotd/rom_cache'


def files():
    out = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard'], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if os.path.isfile(os.path.join(ROOT, f))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default=os.environ.get('HOTD_MAC'), help='user@host of the Mac (default: $HOTD_MAC)')
    ap.add_argument('--dest', default='src/house-of-the-dead-vr', help='under the home folder on the Mac')
    ap.add_argument('--images', action='store_true', help='the ROM set\'s images too (m2nettest)')
    a = ap.parse_args()
    if not a.host: sys.exit('apple_sync: which Mac? --host user@host, or set HOTD_MAC')
    extra = EXTRA + ([IMAGES] if a.images else [])
    for d in extra:
        if not os.path.isdir(os.path.join(ROOT, d)): sys.exit('apple_sync: no %s here' % d)
    p = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', a.host, 'mkdir -p "$HOME/%s" && tar -xf - -C "$HOME/%s"' % (a.dest, a.dest)],
                         stdin=subprocess.PIPE)
    n = 0
    with tarfile.open(fileobj=p.stdin, mode='w|') as t:
        for f in files():
            t.add(os.path.join(ROOT, f), f, recursive=False)
            n += 1
        for d in extra:
            # (through junctions: the worktree may link these from the main checkout)
            for dp, _, fs in os.walk(os.path.join(ROOT, d)):
                for f in fs:
                    full = os.path.join(dp, f)
                    t.add(full, os.path.relpath(full, ROOT).replace('\\', '/'), recursive=False)
                    n += 1
    p.stdin.close()
    if p.wait(): sys.exit('apple_sync: ssh or tar failed')
    print('apple_sync: %d files to %s:~/%s' % (n, a.host, a.dest))


if __name__ == '__main__':
    main()
