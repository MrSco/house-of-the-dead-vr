#!/usr/bin/env python3
"""The game on Apple TV, flat (docs/platforms.md, "Apple TV"), built on the Mac.

  python3 scripts/build_tvos.py simulator|device [--app] [--ipa] [--dest UDID]
  python3 scripts/build_tvos.py device --archive BUILD [--upload]   (TestFlight)

From the PC: python scripts/apple_sync.py first (the sources, the generated code and the libraries this needs), then
this over ssh. As scripts/build_apple.py does for iPhone and iPad: CMake builds every part into static libraries
(cmake/flat_apple.cmake with the tvOS front end, src/flat/tvos), merged into build_tvos/<slice>/libhotdvr_all.a; --app
makes the Xcode project (xcodegen, src/flat/tvos/project.yml) and builds HouseOfTheDeadVR.app around it; --ipa packs a
device build as an unsigned IPA (an Apple TV takes it only signed: Xcode with your team, see docs/platforms.md).

ANGLE (OpenGL ES 3 on Metal): ANGLE_DIR, ~/angle by default, with its Apple TV slices in xcframeworks-tvos (GoldenEye
VR's tools/build_angle_apple.sh tvos-device tvos-simulator makes them). A signed device build: DEVELOPMENT_TEAM=<team>,
with automatic signing; on a Mac with no account in Xcode, the team's App Store Connect key does the profiles
(HOTD_ASC_KEY_DIR with AuthKey_<id>.p8, HOTD_ASC_KEY_ID, HOTD_ASC_ISSUER: a file or the id, as build_apple.py), and
--dest UDID builds for that paired Apple TV, registering it with the team if it isn't yet. Codesign needs the
development identity's keychain unlocked: run it from the Mac's desktop session (Terminal), not over ssh.

--archive BUILD makes the App Store archive and .ipa with that build number (above every tvOS build already sent to
App Store Connect); --upload sends it to TestFlight. The account as for build_apple.py (nothing of it lives in this
repository): HOTD_TEAM, HOTD_PROFILE (the tvOS App Store profile's name: a TVOS_APP_STORE profile, not the iPhone
one), HOTD_KEYCHAIN, HOTD_KEYCHAIN_PASS, and for the upload HOTD_ASC_KEY_DIR, HOTD_ASC_KEY_ID, HOTD_ASC_ISSUER. The app
record needs Apple TV as a platform first (an App Store version for tvOS), or App Store Connect refuses the upload.

HOTD_ICLOUD=1 on a signed build (a device build with DEVELOPMENT_TEAM, or --archive) adds iCloud sync's entitlements
and aps-environment (docs/icloud-sync.md; the App ID and the profile must have iCloud and Push Notifications first).
Unset, the build is as before.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

import build_info

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
    ap.add_argument('--dest', metavar='UDID', help='a signed build for this paired Apple TV (registered if new)')
    ap.add_argument('--archive', metavar='BUILD', help='the App Store archive and .ipa with this build number')
    ap.add_argument('--upload', action='store_true', help='with --archive: send it to TestFlight')
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
         '-DANGLE_DIR=' + angle] + build_info.cmake_args(ROOT) + build_info.apple_debug_args(ROOT), env=env)
    run(['cmake', '--build', bdir, '--target', 'hotdvr_apple', 'arcade_core', 'opus', 'm2_speexdsp', 'm2_rnnoise'], env=env)
    # one library for the app to link
    parts = [p for p in glob.glob(os.path.join(bdir, '*.a')) + glob.glob(os.path.join(bdir, 'opus', '*.a'))
             if not p.endswith('libhotdvr_all.a')]
    allib = os.path.join(bdir, 'libhotdvr_all.a')
    if os.path.exists(allib): os.remove(allib)
    run(['libtool', '-static', '-o', allib] + sorted(parts), env=env)
    print('[tvos] %s (%d libraries)' % (allib, len(parts)))
    if not (a.app or a.ipa or a.archive): return
    quest = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    env['HOTD_VERSION'] = re.search(r'android:versionName="([^"]+)"', quest).group(1)
    env['HOTD_BUILD'] = a.archive or re.search(r'android:versionCode="([^"]+)"', quest).group(1)
    env['HOTD_LIBS'] = libs
    # ANGLE's frameworks without the paths of the machine that built them (build_info.clean_angle)
    env['HOTD_ANGLE_FW'] = build_info.clean_angle(os.path.join(angle, 'xcframeworks-tvos'), os.path.join(libs, 'angle'))
    env.setdefault('DEVELOPMENT_TEAM', '')
    icloud_export = build_info.icloud(ROOT, env, bool(a.archive) or (a.slice == 'device' and bool(env['DEVELOPMENT_TEAM'])),
                                      bool(a.archive), tvos=True, out_dir=libs)
    spec_dir = os.path.join(ROOT, 'src', 'flat', 'tvos')
    run(['xcodegen', 'generate', '--spec', os.path.join(spec_dir, 'project.yml'), '--project', spec_dir], env=env)
    if a.archive:
        if a.slice != 'device': sys.exit('build_tvos: --archive is for the device slice')
        archive(a, env, spec_dir, libs, icloud_export)
        return
    dd = os.path.join(libs, 'dd')
    sign = ['CODE_SIGN_IDENTITY=-'] if a.slice == 'simulator' else (
        ['CODE_SIGNING_ALLOWED=NO'] if not env['DEVELOPMENT_TEAM'] else ['-allowProvisioningUpdates', 'CODE_SIGN_STYLE=Automatic'])
    if a.slice == 'device' and env['DEVELOPMENT_TEAM']:
        if a.dest: sign += ['-destination', 'id=' + a.dest, '-allowProvisioningDeviceRegistration']
        if env.get('HOTD_ASC_KEY_ID'):   # the profiles through the team's App Store Connect key (no Xcode account)
            iss = env['HOTD_ASC_ISSUER']
            if os.path.isfile(iss): iss = open(iss).read().strip()
            sign += ['-authenticationKeyPath', os.path.join(env['HOTD_ASC_KEY_DIR'], 'AuthKey_%s.p8' % env['HOTD_ASC_KEY_ID']),
                     '-authenticationKeyID', env['HOTD_ASC_KEY_ID'], '-authenticationKeyIssuerID', iss]
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


def archive(a, env, spec_dir, libs, icloud_export=''):
    """The App Store archive, its .ipa (manual signing with HOTD_*), and with --upload TestFlight (altool)"""
    need = ['HOTD_TEAM', 'HOTD_PROFILE', 'HOTD_KEYCHAIN', 'HOTD_KEYCHAIN_PASS'] + (
        ['HOTD_ASC_KEY_DIR', 'HOTD_ASC_KEY_ID', 'HOTD_ASC_ISSUER'] if a.upload else [])
    missing = [k for k in need if not env.get(k)]
    if missing: sys.exit('build_tvos: --archive needs ' + ', '.join(missing))
    out = os.path.join(libs, 'testflight-%s-%s' % (env['HOTD_VERSION'], a.archive))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    pw = open(env['HOTD_KEYCHAIN_PASS']).read().strip()
    subprocess.check_call(['security', 'unlock-keychain', '-p', pw, env['HOTD_KEYCHAIN']])
    xa = os.path.join(out, 'HouseOfTheDeadVR.xcarchive')
    run(['xcodebuild', 'archive', '-project', os.path.join(spec_dir, 'HouseOfTheDeadVR.xcodeproj'), '-scheme',
         'HouseOfTheDeadVR', '-configuration', 'Release', '-destination', 'generic/platform=tvOS', '-archivePath', xa,
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
''' % (env['HOTD_TEAM'], 'com.mrsco.hotdvr', env['HOTD_PROFILE'], icloud_export))
    run(['xcodebuild', '-exportArchive', '-archivePath', xa, '-exportPath', os.path.join(out, 'export'),
         '-exportOptionsPlist', opts], env=env)
    ipas = glob.glob(os.path.join(out, 'export', '*.ipa'))
    if not ipas: sys.exit('build_tvos: the export made no .ipa')
    print('[tvos] App Store ipa: %s' % ipas[0])
    # (no build machine's folder or name in what goes to App Store Connect: scripts/check_paths.py)
    import check_paths
    if check_paths.check([ipas[0]]):
        sys.exit("build_tvos: the .ipa holds the build machine's paths or names (above): not sent")
    if not a.upload: return
    iss = env['HOTD_ASC_ISSUER']
    if os.path.isfile(iss): iss = open(iss).read().strip()
    for step in ('--validate-app', '--upload-app'):
        run(['xcrun', 'altool', step, '-f', ipas[0], '-t', 'appletvos', '--apiKey', env['HOTD_ASC_KEY_ID'], '--apiIssuer', iss],
            env=dict(env, API_PRIVATE_KEYS_DIR=env['HOTD_ASC_KEY_DIR']))
    print('[tvos] uploaded build %s to TestFlight (the archive stays in %s: its dSYMs)' % (a.archive, out))


if __name__ == '__main__':
    main()
