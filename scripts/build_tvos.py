#!/usr/bin/env python3
"""The game on Apple TV, flat (docs/platforms.md, "Apple TV"), built on the Mac.

  python3 scripts/build_tvos.py simulator|device [--app] [--ipa]

From the PC: python scripts/apple_sync.py first (the sources, the generated code and the libraries this needs), then
this over ssh. As scripts/build_apple.py does for iPhone and iPad: CMake builds every part into static libraries
(cmake/flat_apple.cmake with the tvOS front end, src/flat/tvos), merged into build_tvos/<slice>/libhotdvr_all.a; --app
makes the Xcode project (xcodegen, src/flat/tvos/project.yml) and builds HouseOfTheDeadVR.app around it; --ipa packs a
device build as an unsigned IPA (an Apple TV takes it only signed: Xcode with your team, see docs/platforms.md).

ANGLE (OpenGL ES 3 on Metal): ANGLE_DIR, ~/angle by default, with its Apple TV slices in xcframeworks-tvos (GoldenEye
VR's tools/build_angle_apple.sh tvos-device tvos-simulator makes them). A signed device build: DEVELOPMENT_TEAM=<team>.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SET = 'hotd'


def run(cmd, **kw):
    print('[tvos] ' + ' '.join(cmd[:6]) + (' ...' if len(cmd) > 6 else ''), flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('slice', choices=('simulator', 'device'))
    ap.add_argument('--app', action='store_true', help='make the Xcode project and build the app')
    ap.add_argument('--ipa', action='store_true', help='an unsigned IPA of the device app')
    a = ap.parse_args()
    env = dict(os.environ)
    env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:' + env.get('PATH', '')
    angle = env.setdefault('ANGLE_DIR', os.path.expanduser('~/angle'))
    if not os.path.isdir(os.path.join(angle, 'xcframeworks-tvos')): sys.exit('build_tvos: no ANGLE Apple TV slices in %s/xcframeworks-tvos' % angle)
    gen = os.path.join(ROOT, 'build_houseofthedead', 'recomp', SET, 'gen')
    if not os.path.isdir(gen): sys.exit('build_tvos: no generated code (python scripts/apple_sync.py, from the PC)')
    libs = os.path.join(ROOT, 'build_tvos')
    bdir = os.path.join(libs, a.slice)
    sdk = 'appletvsimulator' if a.slice == 'simulator' else 'appletvos'
    run(['cmake', '-S', ROOT, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=tvOS',
         '-DCMAKE_OSX_SYSROOT=' + sdk, '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=17.0',
         '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON', '-DM2_GEN_ROOT=' + gen,
         '-DANGLE_DIR=' + angle], env=env)
    run(['cmake', '--build', bdir, '--target', 'hotdvr_apple', 'arcade_core', 'opus', 'm2_speexdsp', 'm2_rnnoise'], env=env)
    # one library for the app to link
    parts = [p for p in glob.glob(os.path.join(bdir, '*.a')) + glob.glob(os.path.join(bdir, 'opus', '*.a'))
             if not p.endswith('libhotdvr_all.a')]
    allib = os.path.join(bdir, 'libhotdvr_all.a')
    if os.path.exists(allib): os.remove(allib)
    run(['libtool', '-static', '-o', allib] + sorted(parts), env=env)
    print('[tvos] %s (%d libraries)' % (allib, len(parts)))
    if not (a.app or a.ipa): return
    quest = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    env['HOTD_VERSION'] = re.search(r'android:versionName="([^"]+)"', quest).group(1)
    env['HOTD_BUILD'] = re.search(r'android:versionCode="([^"]+)"', quest).group(1)
    env['HOTD_LIBS'] = libs
    env.setdefault('DEVELOPMENT_TEAM', '')
    spec_dir = os.path.join(ROOT, 'src', 'flat', 'tvos')
    run(['xcodegen', 'generate', '--spec', os.path.join(spec_dir, 'project.yml'), '--project', spec_dir], env=env)
    dd = os.path.join(libs, 'dd')
    sign = ['CODE_SIGN_IDENTITY=-'] if a.slice == 'simulator' else (
        ['CODE_SIGNING_ALLOWED=NO'] if not env['DEVELOPMENT_TEAM'] else ['-allowProvisioningUpdates', 'CODE_SIGN_STYLE=Automatic'])
    run(['xcodebuild', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme', 'HouseOfTheDeadVR',
         '-sdk', sdk, '-configuration', 'Release', '-derivedDataPath', dd, 'build'] + sign, env=env)
    app = os.path.join(dd, 'Build', 'Products', 'Release-' + sdk, 'HouseOfTheDeadVR.app')
    print('[tvos] app: %s' % app)
    if a.ipa and a.slice == 'device':
        ipa = os.path.join(libs, 'HouseOfTheDeadVR-%s-tvos.ipa' % env['HOTD_VERSION'])
        with zipfile.ZipFile(ipa, 'w', zipfile.ZIP_DEFLATED) as z:
            for dp, _, fs in os.walk(app):
                for f in fs:
                    full = os.path.join(dp, f)
                    z.write(full, os.path.join('Payload', 'HouseOfTheDeadVR.app', os.path.relpath(full, app)))
        print('[tvos] ipa: %s' % ipa)


if __name__ == '__main__':
    main()
