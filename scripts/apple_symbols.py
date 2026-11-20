#!/usr/bin/env python3
"""The Apple apps' crash symbols, on the Mac: every dSYM of the app found, as one DWARF file per UUID.

  python3 scripts/apple_symbols.py --out DIR [--archives DIR ...] [--simulator]

Crash reports from iPhone, iPad, Mac, Apple TV and Vision Pro (src/platform/apple/crash_apple.m) give their frames as
offsets in the app's executable and its LC_UUID; the triage symbolizes them with the dSYM of that UUID. This copies
each dSYM's DWARF file (one architecture each: lipo) to DIR/<executable>-<uuid>.dwarf (spaces as _, the UUID's 32
lowercase digits). scripts/quest_tools.py symbols uploads them (gzipped) to the private repository's crash-symbols
release; scripts/release.py runs both.

It looks in this checkout's builds (build_ios, build_tvos, build_visionos: Xcode's builds and TestFlight archives;
build_houseofthedead/mac: the Mac app's, where cmake/flat_apple.cmake makes its dSYM), and in --archives (default:
$HOTD_ARCHIVES, where scripts/apple/testflight.sh moves each uploaded archive). A Mac app built before the dSYM was
made (0.6.0 and older) is taken itself: it isn't stripped, so its symbol table gives the function names. Simulator
builds are left out (no player runs one) unless --simulator.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_NAMES = ('HouseOfTheDeadVR', 'House of the Dead VR')


def uuids(path):
    """[(uuid, arch)] of a Mach-O file (dwarfdump --uuid)."""
    out = subprocess.run(['dwarfdump', '--uuid', path], capture_output=True, text=True).stdout
    return [(m.group(1).replace('-', '').lower(), m.group(2)) for m in re.finditer(r'UUID: ([0-9A-F-]{36}) \((\w+)\)', out)]


def candidates(archives, simulator=False):
    found = []
    for d in ('build_ios', 'build_tvos', 'build_visionos', os.path.join('build_houseofthedead', 'mac')):
        found += glob.glob(os.path.join(ROOT, d, '**', '*.dSYM'), recursive=True)
    for a in archives:
        found += glob.glob(os.path.join(a, '**', '*.dSYM'), recursive=True)
    dwarf = []
    for ds in found:
        if not simulator and 'simulator' in ds: continue
        for f in glob.glob(os.path.join(ds, 'Contents', 'Resources', 'DWARF', '*')):
            if os.path.basename(f) in APP_NAMES: dwarf.append(f)
    # the Mac app itself (unstripped up to 0.6.0: its symbol table)
    for app in glob.glob(os.path.join(ROOT, 'build_houseofthedead', 'mac', '*', 'House of the Dead VR.app', 'Contents', 'MacOS', '*')):
        dwarf.append(app)
    return dwarf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True, help='where the <executable>-<uuid>.dwarf files go')
    ap.add_argument('--archives', action='append', help='more folders with .xcarchive / .dSYM (default: $HOTD_ARCHIVES)')
    ap.add_argument('--simulator', action='store_true', help="the simulator builds' too (tests)")
    a = ap.parse_args()
    archives = a.archives or ([os.environ['HOTD_ARCHIVES']] if os.environ.get('HOTD_ARCHIVES') else [])
    out = os.path.expanduser(a.out)
    os.makedirs(out, exist_ok=True)
    n = 0
    for f in candidates([os.path.expanduser(x) for x in archives], a.simulator):
        name = os.path.basename(f).replace(' ', '_')
        ids = uuids(f)
        for uid, arch in ids:
            dst = os.path.join(out, '%s-%s.dwarf' % (name, uid))
            if os.path.exists(dst): continue   # (a dSYM comes before the Mac app of the same UUID: it wins)
            if len(ids) > 1:
                subprocess.check_call(['lipo', f, '-thin', arch, '-output', dst])
            else:
                shutil.copy2(f, dst)
            print('%s  %s (%s)' % (os.path.basename(dst), os.path.relpath(f, os.path.expanduser('~')), arch))
            n += 1
    print('apple_symbols: %d new in %s' % (n, out))


if __name__ == '__main__':
    sys.exit(main())
