#!/usr/bin/env python3
"""The VR game on Apple Vision Pro (docs/platforms.md, P5), built on the Mac.

  python3 scripts/build_visionos.py simulator|device [--app]
  python3 scripts/build_visionos.py device --archive BUILD [--upload]   (TestFlight)

From the PC: python scripts/apple_sync.py first (the sources, the generated code and the libraries this needs), then
this over ssh. CMake builds every part into static libraries (cmake/visionos.cmake: the game, the VR shell in
src/visionos, the Quest's model code, src/net, voice chat), merged into build_visionos/<slice>/libhotdvr_all.a; --app
makes the Xcode project (xcodegen, src/visionos/project.yml) and builds HouseOfTheDeadVR.app around it. ANGLE (OpenGL
ES 3 on Metal): ANGLE_DIR, ~/angle by default, with its visionOS slices (xcframeworks-visionos, as GoldenEye VR's
tools/build_angle_apple.sh makes them).

HOTD_BUNDLE_ID sets the app's bundle id (com.mrsco.hotdvr by default). --archive BUILD makes the App Store archive and
.ipa with that build number; --upload sends it to TestFlight. The account as for build_apple.py (nothing of it lives
in this repository): HOTD_TEAM, HOTD_PROFILE (the App Store profile's name: the iPhone app's serves visionOS too),
HOTD_KEYCHAIN, HOTD_KEYCHAIN_PASS, and for the upload HOTD_ASC_KEY_DIR, HOTD_ASC_KEY_ID, HOTD_ASC_ISSUER.

HOTD_ICLOUD=1 on a signed build (a device build with DEVELOPMENT_TEAM, or --archive) adds iCloud sync's entitlements
(docs/icloud-sync.md; the App ID and the profile must have iCloud first). Unset, the build is as before.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

import build_info

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SET = 'hotd'


def run(cmd, **kw):
    print('[visionos] ' + ' '.join(cmd[:6]) + (' ...' if len(cmd) > 6 else ''), flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('slice', choices=('simulator', 'device'))
    ap.add_argument('--app', action='store_true', help='make the Xcode project and build the app')
    ap.add_argument('--archive', metavar='BUILD', help='the App Store archive and .ipa with this build number')
    ap.add_argument('--upload', action='store_true', help='with --archive: send it to TestFlight')
    a = ap.parse_args()
    env = dict(os.environ)
    env['PATH'] = '/opt/homebrew/bin:/usr/local/bin:' + env.get('PATH', '')
    angle = env.setdefault('ANGLE_DIR', os.path.expanduser('~/angle'))
    if not os.path.isdir(os.path.join(angle, 'xcframeworks-visionos')):
        sys.exit('build_visionos: no ANGLE visionOS slices in %s/xcframeworks-visionos' % angle)
    gen = os.path.join(ROOT, 'build_houseofthedead', 'recomp', SET, 'gen')
    if not os.path.isdir(gen): sys.exit('build_visionos: no generated code (python scripts/apple_sync.py, from the PC)')
    libs = os.path.join(ROOT, 'build_visionos')
    bdir = os.path.join(libs, a.slice)
    sysroot = 'xrsimulator' if a.slice == 'simulator' else 'xros'
    run(['cmake', '-S', ROOT, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_NAME=visionOS',
         '-DCMAKE_OSX_SYSROOT=' + sysroot, '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0',
         '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON', '-DM2_GEN_ROOT=' + gen,
         '-DANGLE_DIR=' + angle] + build_info.cmake_args(ROOT) + build_info.apple_debug_args(ROOT), env=env)
    run(['cmake', '--build', bdir, '--target', 'hotdvr_visionos', 'arcade_core', 'opus', 'm2_speexdsp', 'm2_rnnoise'], env=env)
    parts = [p for p in glob.glob(os.path.join(bdir, '*.a')) + glob.glob(os.path.join(bdir, 'opus', '*.a'))
             if not p.endswith('libhotdvr_all.a')]
    allib = os.path.join(bdir, 'libhotdvr_all.a')
    if os.path.exists(allib): os.remove(allib)
    run(['libtool', '-static', '-o', allib] + sorted(parts), env=env)
    print('[visionos] %s (%d libraries)' % (allib, len(parts)))
    if not (a.app or a.archive): return
    quest = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    env['HOTD_VERSION'] = re.search(r'android:versionName="([^"]+)"', quest).group(1).split('-')[0]
    env['HOTD_BUILD'] = a.archive or re.search(r'android:versionCode="([^"]+)"', quest).group(1)
    env['HOTD_LIBS'] = libs
    # ANGLE's frameworks without the paths of the machine that built them (build_info.clean_angle)
    env['HOTD_ANGLE_FW'] = build_info.clean_angle(os.path.join(angle, 'xcframeworks-visionos'), os.path.join(libs, 'angle'))
    env.setdefault('HOTD_BUNDLE_ID', 'com.mrsco.hotdvr')
    env.setdefault('DEVELOPMENT_TEAM', '')
    icloud_export = build_info.icloud(ROOT, env, bool(a.archive) or (a.slice == 'device' and bool(env['DEVELOPMENT_TEAM'])),
                                      bool(a.archive))
    spec_dir = os.path.join(ROOT, 'src', 'visionos')
    run(['xcodegen', 'generate', '--spec', os.path.join(spec_dir, 'project.yml'), '--project', spec_dir], env=env)
    if a.archive:
        if a.slice != 'device': sys.exit('build_visionos: --archive is for the device slice')
        archive(a, env, spec_dir, libs, icloud_export)
        return
    dd = os.path.join(libs, 'dd')
    sdk = sysroot
    sign = ['CODE_SIGN_IDENTITY=-'] if a.slice == 'simulator' else (
        ['CODE_SIGNING_ALLOWED=NO'] if not env['DEVELOPMENT_TEAM'] else ['-allowProvisioningUpdates'])
    run(['xcodebuild', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme', 'HouseOfTheDeadVR',
         '-sdk', sdk, '-configuration', 'Release', '-derivedDataPath', dd, 'build'] + sign, env=env)
    print('[visionos] app: %s' % os.path.join(dd, 'Build', 'Products', 'Release-' + sdk, 'HouseOfTheDeadVR.app'))


def archive(a, env, spec_dir, libs, icloud_export=''):
    """The App Store archive, its .ipa (manual signing with HOTD_*), and with --upload TestFlight (altool)"""
    need = ['HOTD_TEAM', 'HOTD_PROFILE', 'HOTD_KEYCHAIN', 'HOTD_KEYCHAIN_PASS'] + (
        ['HOTD_ASC_KEY_DIR', 'HOTD_ASC_KEY_ID', 'HOTD_ASC_ISSUER'] if a.upload else [])
    missing = [k for k in need if not env.get(k)]
    if missing: sys.exit('build_visionos: --archive needs ' + ', '.join(missing))
    out = os.path.join(libs, 'testflight-%s-%s' % (env['HOTD_VERSION'], a.archive))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    pw = open(env['HOTD_KEYCHAIN_PASS']).read().strip()
    subprocess.check_call(['security', 'unlock-keychain', '-p', pw, env['HOTD_KEYCHAIN']])
    xa = os.path.join(out, 'HouseOfTheDeadVR.xcarchive')
    run(['xcodebuild', 'archive', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme',
         'HouseOfTheDeadVR', '-configuration', 'Release', '-destination', 'generic/platform=visionOS', '-archivePath', xa,
         '-derivedDataPath', os.path.join(libs, 'dd-archive'), 'CODE_SIGN_STYLE=Manual',
         'DEVELOPMENT_TEAM=' + env['HOTD_TEAM'], 'CODE_SIGN_IDENTITY=Apple Distribution',
         'PROVISIONING_PROFILE_SPECIFIER=' + env['HOTD_PROFILE'], 'OTHER_CODE_SIGN_FLAGS=--keychain ' + env['HOTD_KEYCHAIN']],
        env=env)
    opts = os.path.join(out, 'ExportOptions.plist')
    open(opts, 'w').write('''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>app-store-connect</string>
  <key>teamID</key><string>%s</string>
  <key>signingStyle</key><string>manual</string>
  <key>signingCertificate</key><string>Apple Distribution</string>
  <key>provisioningProfiles</key><dict><key>%s</key><string>%s</string></dict>
  <key>uploadSymbols</key><true/>
  %s
</dict></plist>
''' % (env['HOTD_TEAM'], env['HOTD_BUNDLE_ID'], env['HOTD_PROFILE'], icloud_export))
    run(['xcodebuild', '-exportArchive', '-archivePath', xa, '-exportPath', os.path.join(out, 'export'),
         '-exportOptionsPlist', opts], env=env)
    ipas = glob.glob(os.path.join(out, 'export', '*.ipa'))
    if not ipas: sys.exit('build_visionos: the export made no .ipa')
    print('[visionos] App Store ipa: %s' % ipas[0])
    # (no build machine's folder or name in what goes to App Store Connect: scripts/check_paths.py)
    import check_paths
    if check_paths.check([ipas[0]]):
        sys.exit("build_visionos: the .ipa holds the build machine's paths or names (above): not sent")
    if not a.upload: return
    iss = env['HOTD_ASC_ISSUER']
    if os.path.isfile(iss): iss = open(iss).read().strip()
    for step in ('--validate-app', '--upload-app'):
        run(['xcrun', 'altool', step, '-f', ipas[0], '-t', 'visionos', '--apiKey', env['HOTD_ASC_KEY_ID'], '--apiIssuer', iss],
            env=dict(env, API_PRIVATE_KEYS_DIR=env['HOTD_ASC_KEY_DIR']))
    print('[visionos] uploaded build %s to TestFlight (the archive stays in %s: its dSYMs)' % (a.archive, out))


if __name__ == '__main__':
    main()
