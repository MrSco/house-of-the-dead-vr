"""What a build says about itself in its crash reports (tools/vr/crash-relay): the version and the commit, as CMake
definitions for src/platform/windows/crash_win.c and src/platform/apple/crash_apple.m (the Android builds have
assets/build-info.txt instead: quest_tools.py), the Apple builds' debug information for their dSYMs, and no build
machine's folders in any of them (prefix_map_flags, clean_angle).

The commit is git's, or, in a copy apple_sync.py sent to the Mac (no .git there), the build-commit.txt it wrote.
"""
import os
import re
import subprocess


def version(root):
    text = open(os.path.join(root, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    m = re.search(r'android:versionName="([^"]+)"', text)
    return m.group(1) if m else ''


def commit(root):
    """The checkout's commit (12 digits, -dirty with uncommitted changes to tracked files), or ''."""
    try:
        c = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=root, capture_output=True, text=True).stdout.strip()
        if c:
            if subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root,
                              capture_output=True, text=True).stdout.strip():
                c += '-dirty'
            return c
    except OSError:
        pass
    p = os.path.join(root, 'build-commit.txt')
    return open(p, encoding='utf-8').read().strip() if os.path.exists(p) else ''


def cmake_args(root):
    """-D options for the version and the commit (CMakeLists.txt: HOTD_BUILD_VERSION, HOTD_BUILD_COMMIT)."""
    return ['-DHOTD_BUILD_VERSION=' + version(root), '-DHOTD_BUILD_COMMIT=' + commit(root)]


def prefix_map_flags(root):
    """clang's and GCC's flags for no build machine's folders in a binary (its __FILE__s, its debug information): the
    checkout's folder as ., the home folder as ~ (as cmake/prefix_map.cmake, for the builds without CMake:
    build_quest.py). The checkout's last: the one that wins when both match."""
    home = os.path.expanduser('~')
    flags = ['-ffile-prefix-map=%s=~' % home] if home and home != '~' else []
    return flags + ['-ffile-prefix-map=%s=.' % os.path.abspath(root), '-fdebug-compilation-dir=.']


def apple_debug_args(root):
    """Line tables in every Apple library, for the app's dSYM (Xcode's dsymutil collects them; the Mac app's from
    cmake/flat_apple.cmake), its paths relative to the checkout (cmake/prefix_map.cmake: src/...)."""
    flags = '-gline-tables-only'
    return ['-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags, '-DCMAKE_OBJC_FLAGS=' + flags]


def clean_angle(src, dest):
    """ANGLE's two xcframeworks (src: ~/angle/xcframeworks, -tvos or -visionos) copied to dest without their debug
    maps (strip -S -x: the paths of the object files on the machine that built ANGLE), for the app's project to embed
    (HOTD_ANGLE_FW: Xcode signs them as it copies them in, as before: ANGLE's are unsigned). ~/angle itself is never
    changed: GoldenEye VR's builds share it. Returns dest."""
    import shutil
    shutil.rmtree(dest, ignore_errors=True)
    for n in ('libGLESv2', 'libEGL'):
        shutil.copytree(os.path.join(src, n + '.xcframework'), os.path.join(dest, n + '.xcframework'), symlinks=True)
        for dp, _, fs in os.walk(os.path.join(dest, n + '.xcframework')):
            if n in fs and not os.path.islink(os.path.join(dp, n)):
                subprocess.check_call(['strip', '-S', '-x', os.path.join(dp, n)])
    return dest


def icloud(root, env, signed, archive, tvos=False, out_dir=None):
    """iCloud sync (docs/icloud-sync.md) for a build: HOTD_ICLOUD=1 in the environment and a signed build (a device
    build with a team, or an archive; never the simulator) give the app src/platform/apple/hotd-icloud.entitlements
    and Info.plist's HOTDICloud YES. The App ID and the profile must have iCloud. The Apple TV's entitlements also get
    aps-environment (production for an archive, else development): App Store Connect refuses tvOS iCloud without it
    (error 90575, GoldenEye VR's tvOS 9). Sets HOTD_ICLOUD and HOTD_ENTITLEMENTS for the Xcode project (empty: as before)
    and returns the export options' line (iCloudContainerEnvironment Production for an archive), '' when off."""
    on = env.get('HOTD_ICLOUD', '') in ('1', 'YES', 'yes', 'true') and signed
    env['HOTD_ICLOUD'] = 'YES' if on else 'NO'
    env['HOTD_ENTITLEMENTS'] = ''
    if not on:
        return ''
    ent = os.path.join(root, 'src', 'platform', 'apple', 'hotd-icloud.entitlements')
    if tvos:
        text = open(ent, encoding='utf-8').read()
        aps = 'production' if archive else 'development'
        text = text.replace('</dict>\n</plist>', '\t<key>aps-environment</key>\n\t<string>%s</string>\n</dict>\n</plist>' % aps, 1)
        os.makedirs(out_dir, exist_ok=True)
        ent = os.path.join(out_dir, 'hotd-icloud-tvos.entitlements')
        open(ent, 'w', encoding='utf-8', newline='\n').write(text)
    env['HOTD_ENTITLEMENTS'] = ent
    print('[icloud] signed with %s' % ent, flush=True)
    return '<key>iCloudContainerEnvironment</key><string>Production</string>' if archive else ''
