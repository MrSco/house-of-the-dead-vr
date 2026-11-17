#!/usr/bin/env python3
"""The game on iPhone and iPad, flat (docs/platforms.md, P3), built on the Mac.

  python3 scripts/build_apple.py simulator|device [--app] [--ipa]
  python3 scripts/build_apple.py mac              (House of the Dead VR.app, in build_houseofthedead/mac/hotd/)

From the PC: python scripts/apple_sync.py first (the sources, the generated code and the libraries this needs), then
this over ssh. CMake builds every part into static libraries (cmake/flat_apple.cmake: the game, the flat shell with
its iOS front end, src/net, voice chat), merged into build_ios/<slice>/libhotdvr_all.a; --app makes the Xcode project
(xcodegen, src/flat/ios/project.yml) and builds HouseOfTheDeadVR.app around it; --ipa packs a device build as an
unsigned IPA (SideStore signs it with the player's Apple ID). ANGLE (OpenGL ES 3 on Metal): ANGLE_DIR, ~/angle by
default, as GoldenEye VR's builds use it.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SET = 'hotd'


def run(cmd, **kw):
    print('[apple] ' + ' '.join(cmd[:6]) + (' ...' if len(cmd) > 6 else ''), flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('slice', choices=('simulator', 'device', 'mac'))
    ap.add_argument('--app', action='store_true', help='make the Xcode project and build the app')
    ap.add_argument('--ipa', action='store_true', help='an unsigned IPA of the device app')
    a = ap.parse_args()
    env = dict(os.environ)
    env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:' + env.get('PATH', '')
    angle = env.setdefault('ANGLE_DIR', os.path.expanduser('~/angle'))
    if not os.path.isdir(os.path.join(angle, 'xcframeworks' if a.slice != 'mac' else 'out/mac')): sys.exit('build_apple: no ANGLE in %s' % angle)
    gen = os.path.join(ROOT, 'build_houseofthedead', 'recomp', SET, 'gen')
    if not os.path.isdir(gen): sys.exit('build_apple: no generated code (python scripts/apple_sync.py, from the PC)')
    if a.slice == 'mac':   # the Mac app: CMake makes the bundle itself
        bdir = os.path.join(ROOT, 'build_houseofthedead', 'mac', SET)   # (m2nettest's too: the game compiled once)
        run(['cmake', '-S', ROOT, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
             '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON',
             '-DM2_GEN_ROOT=' + gen, '-DANGLE_DIR=' + angle], env=env)
        run(['cmake', '--build', bdir, '--target', 'hotdvr_mac'], env=env)
        print('[apple] app: %s' % os.path.join(bdir, 'House of the Dead VR.app'))
        return
    libs = os.path.join(ROOT, 'build_ios')
    bdir = os.path.join(libs, a.slice)
    sysroot = 'iphonesimulator' if a.slice == 'simulator' else 'iphoneos'
    run(['cmake', '-S', ROOT, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=iOS',
         '-DCMAKE_OSX_SYSROOT=' + sysroot, '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
         '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON', '-DM2_GEN_ROOT=' + gen,
         '-DANGLE_DIR=' + angle], env=env)
    run(['cmake', '--build', bdir, '--target', 'hotdvr_apple', 'arcade_core', 'opus', 'm2_speexdsp', 'm2_rnnoise'], env=env)
    # one library for the app to link
    parts = [p for p in glob.glob(os.path.join(bdir, '*.a')) + glob.glob(os.path.join(bdir, 'opus', '*.a'))
             if not p.endswith('libhotdvr_all.a')]
    allib = os.path.join(bdir, 'libhotdvr_all.a')
    if os.path.exists(allib): os.remove(allib)
    run(['libtool', '-static', '-o', allib] + sorted(parts), env=env)
    print('[apple] %s (%d libraries)' % (allib, len(parts)))
    if not (a.app or a.ipa): return
    quest = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    env['HOTD_VERSION'] = re.search(r'android:versionName="([^"]+)"', quest).group(1)
    env['HOTD_BUILD'] = re.search(r'android:versionCode="([^"]+)"', quest).group(1)
    env['HOTD_LIBS'] = libs
    env.setdefault('DEVELOPMENT_TEAM', '')
    # (beside its spec: xcodegen writes the paths outside the project relative to it)
    spec_dir = os.path.join(ROOT, 'src', 'flat', 'ios')
    run(['xcodegen', 'generate', '--spec', os.path.join(spec_dir, 'project.yml'), '--project', spec_dir], env=env)
    dd = os.path.join(libs, 'dd')
    sdk = 'iphonesimulator' if a.slice == 'simulator' else 'iphoneos'
    sign = ['CODE_SIGN_IDENTITY=-'] if a.slice == 'simulator' else (
        ['CODE_SIGNING_ALLOWED=NO'] if not env['DEVELOPMENT_TEAM'] else ['-allowProvisioningUpdates'])
    run(['xcodebuild', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme', 'HouseOfTheDeadVR',
         '-sdk', sdk, '-configuration', 'Release', '-derivedDataPath', dd, 'build'] + sign, env=env)
    app = os.path.join(dd, 'Build', 'Products', 'Release-' + sdk, 'HouseOfTheDeadVR.app')
    print('[apple] app: %s' % app)
    if a.ipa and a.slice == 'device':
        ipa = os.path.join(libs, 'HouseOfTheDeadVR-%s.ipa' % env['HOTD_VERSION'])
        with zipfile.ZipFile(ipa, 'w', zipfile.ZIP_DEFLATED) as z:
            for dp, _, fs in os.walk(app):
                for f in fs:
                    full = os.path.join(dp, f)
                    z.write(full, os.path.join('Payload', 'HouseOfTheDeadVR.app', os.path.relpath(full, app)))
        print('[apple] ipa: %s' % ipa)


if __name__ == '__main__':
    main()
