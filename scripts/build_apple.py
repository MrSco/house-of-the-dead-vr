#!/usr/bin/env python3
"""The game on iPhone and iPad, flat (docs/platforms.md, P3), built on the Mac.

  python3 scripts/build_apple.py simulator|device [--app] [--ipa]
  python3 scripts/build_apple.py device --archive BUILD [--upload]   (TestFlight)
  python3 scripts/build_apple.py mac              (House of the Dead VR.app, in build_houseofthedead/mac/hotd/)

From the PC: python scripts/apple_sync.py first (the sources, the generated code and the libraries this needs), then
this over ssh. CMake builds every part into static libraries (cmake/flat_apple.cmake: the game, the flat shell with
its iOS front end, src/net, voice chat), merged into build_ios/<slice>/libhotdvr_all.a; --app makes the Xcode project
(xcodegen, src/flat/ios/project.yml) and builds HouseOfTheDeadVR.app around it; --ipa packs a device build as an
unsigned IPA (SideStore signs it with the player's Apple ID). ANGLE (OpenGL ES 3 on Metal): ANGLE_DIR, ~/angle by
default, as GoldenEye VR's builds use it.

--archive BUILD makes the App Store archive and .ipa with that build number (above every build already sent to App
Store Connect); --upload sends it to TestFlight. As GoldenEye VR's tools/build_visionos_native.sh, and with the same
account (nothing of it lives in this repository): HOTD_TEAM, HOTD_PROFILE (the App Store profile's name),
HOTD_KEYCHAIN (the keychain with the Apple Distribution identity), HOTD_KEYCHAIN_PASS (a file with its password), and
for the upload HOTD_ASC_KEY_DIR (AuthKey_<id>.p8), HOTD_ASC_KEY_ID, HOTD_ASC_ISSUER (a file, or the id itself).
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
    ap.add_argument('--archive', metavar='BUILD', help='the App Store archive and .ipa with this build number')
    ap.add_argument('--upload', action='store_true', help='with --archive: send it to TestFlight')
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
    if not (a.app or a.ipa or a.archive): return
    quest = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    env['HOTD_VERSION'] = re.search(r'android:versionName="([^"]+)"', quest).group(1)
    env['HOTD_BUILD'] = a.archive or re.search(r'android:versionCode="([^"]+)"', quest).group(1)
    env['HOTD_LIBS'] = libs
    env.setdefault('DEVELOPMENT_TEAM', '')
    # (beside its spec: xcodegen writes the paths outside the project relative to it)
    spec_dir = os.path.join(ROOT, 'src', 'flat', 'ios')
    run(['xcodegen', 'generate', '--spec', os.path.join(spec_dir, 'project.yml'), '--project', spec_dir], env=env)
    if a.archive:
        if a.slice != 'device': sys.exit('build_apple: --archive is for the device slice')
        archive(a, env, spec_dir, libs)
        return
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


def archive(a, env, spec_dir, libs):
    """The App Store archive, its .ipa (manual signing with HOTD_*), and with --upload TestFlight (altool)"""
    need = ['HOTD_TEAM', 'HOTD_PROFILE', 'HOTD_KEYCHAIN', 'HOTD_KEYCHAIN_PASS'] + (
        ['HOTD_ASC_KEY_DIR', 'HOTD_ASC_KEY_ID', 'HOTD_ASC_ISSUER'] if a.upload else [])
    missing = [k for k in need if not env.get(k)]
    if missing: sys.exit('build_apple: --archive needs ' + ', '.join(missing))
    out = os.path.join(libs, 'testflight-%s-%s' % (env['HOTD_VERSION'], a.archive))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    pw = open(env['HOTD_KEYCHAIN_PASS']).read().strip()
    subprocess.check_call(['security', 'unlock-keychain', '-p', pw, env['HOTD_KEYCHAIN']])
    xa = os.path.join(out, 'HouseOfTheDeadVR.xcarchive')
    run(['xcodebuild', 'archive', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme',
         'HouseOfTheDeadVR', '-configuration', 'Release', '-destination', 'generic/platform=iOS', '-archivePath', xa,
         '-derivedDataPath', os.path.join(libs, 'dd-archive'), 'CODE_SIGN_STYLE=Manual',
         'DEVELOPMENT_TEAM=' + env['HOTD_TEAM'], 'CODE_SIGN_IDENTITY=Apple Distribution',
         'PROVISIONING_PROFILE_SPECIFIER=' + env['HOTD_PROFILE'], 'OTHER_CODE_SIGN_FLAGS=--keychain ' + env['HOTD_KEYCHAIN']],
        env=env)
    opts = os.path.join(out, 'ExportOptions.plist')
    bundle = 'com.mrsco.hotdvr'
    open(opts, 'w').write('''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>app-store-connect</string>
  <key>teamID</key><string>%s</string>
  <key>signingStyle</key><string>manual</string>
  <key>signingCertificate</key><string>Apple Distribution</string>
  <key>provisioningProfiles</key><dict><key>%s</key><string>%s</string></dict>
  <key>uploadSymbols</key><true/>
</dict></plist>
''' % (env['HOTD_TEAM'], bundle, env['HOTD_PROFILE']))
    run(['xcodebuild', '-exportArchive', '-archivePath', xa, '-exportPath', os.path.join(out, 'export'),
         '-exportOptionsPlist', opts], env=env)
    ipas = glob.glob(os.path.join(out, 'export', '*.ipa'))   # (named after the app's name: "House of the Dead VR.ipa")
    if not ipas: sys.exit('build_apple: the export made no .ipa')
    ipa = ipas[0]
    print('[apple] App Store ipa: %s' % ipa)
    if not a.upload: return
    iss = env['HOTD_ASC_ISSUER']
    if os.path.isfile(iss): iss = open(iss).read().strip()
    for step in ('--validate-app', '--upload-app'):
        run(['xcrun', 'altool', step, '-f', ipa, '-t', 'ios', '--apiKey', env['HOTD_ASC_KEY_ID'], '--apiIssuer', iss],
            env=dict(env, API_PRIVATE_KEYS_DIR=env['HOTD_ASC_KEY_DIR']))
    print('[apple] uploaded build %s to TestFlight (the archive stays in %s: its dSYMs)' % (a.archive, out))


if __name__ == '__main__':
    main()
