#!/usr/bin/env python3
"""A release's downloads, every platform from the same commit (as GoldenEye VR's releases), and their table.

  python scripts/release.py [--apple] [--only quest,frame,phone,windows,mac,ios] [--publish]

From a clean checkout (no uncommitted changes to tracked files: the builds stamp the commit), the version being
src/quest/AndroidManifest.xml's versionName. Builds into build_release/<version>/, with the names the in-app updater
looks for (src/quest/java/.../UpdateVersion.java: the Quest's APK any name, the others by their suffix):

  house-of-the-dead-vr-<v>.apk              Meta Quest (scripts/build_quest.py --release)
  house-of-the-dead-vr-<v>-frame.apk        Steam Frame (--flavor frame)
  house-of-the-dead-vr-<v>-android.apk      Android phones and tablets (--flavor phone)
  house-of-the-dead-vr-<v>-windows-x64.zip  Windows (scripts/build_windows.py --zip)
  house-of-the-dead-vr-<v>-mac.zip          Mac, Apple silicon (--apple: scripts/apple_sync.py, build_apple.py mac)
  house-of-the-dead-vr-<v>-ios.ipa          iPhone and iPad, unsigned for SideStore (--apple: build_apple.py device)

Then their SHA-256 into docs/releases/v<version>.md's "## Downloads" (the notes, written by hand above it), and the
Quest's crash symbols to the private repo. --publish: the GitHub release (tag v<version>, the notes as its text; a
pre-release for a version with a suffix, 0.4.1-test.1), created, or its assets replaced when it exists. Publishing a
release also announces it on Discord (.github/workflows/discord-release.yml).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = 'MrSco/house-of-the-dead-vr'
MAC = 'rocco@rjuliano-mac'
MAC_DIR = 'src/house-of-the-dead-vr'
# (key, file suffix, what it is for, how to say it in the table)
PLATFORMS = [
    ('quest', '.apk', 'Meta Quest 3 (and 3S)'),
    ('windows', '-windows-x64.zip', 'Windows PC (flat: mouse, pads, light guns)'),
    ('mac', '-mac.zip', 'Mac, Apple silicon (flat; experimental)'),
    ('phone', '-android.apk', 'Android phones and tablets (experimental)'),
    ('ios', '-ios.ipa', 'iPhone and iPad via SideStore (experimental)'),
    ('frame', '-frame.apk', 'Steam Frame (experimental, untested)'),
]


def log(s):
    print('[release] ' + s, flush=True)


def run(cmd, **kw):
    log(' '.join(cmd[:6]) + (' ...' if len(cmd) > 6 else ''))
    subprocess.check_call(cmd, **kw)


def git(*a):
    return subprocess.run(['git'] + list(a), cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def build(key, out, name):
    py = sys.executable
    if key in ('quest', 'frame', 'phone'):
        run([py, os.path.join(ROOT, 'scripts', 'build_quest.py'), '--release', '--apk-only', '--flavor', key])
        src = {'quest': ('arcade', 'hotdvr.apk'), 'frame': ('arcade', 'hotdvr-frame.apk'),
               'phone': ('phone', 'hotdvr-android.apk')}[key]
        shutil.copy2(os.path.join(ROOT, 'build_quest', *src), os.path.join(out, name))
    elif key == 'windows':
        run([py, os.path.join(ROOT, 'scripts', 'build_windows.py'), '--zip'])
        z = [f for f in os.listdir(os.path.join(ROOT, 'build_windows')) if f.endswith('-windows.zip')]
        z = max(z, key=lambda f: os.path.getmtime(os.path.join(ROOT, 'build_windows', f)))
        shutil.copy2(os.path.join(ROOT, 'build_windows', z), os.path.join(out, name))
    elif key in ('mac', 'ios'):
        remote = '~/' + MAC_DIR
        if key == 'mac':
            run(['ssh', MAC, 'cd %s && python3 scripts/build_apple.py mac && cd build_houseofthedead/mac/hotd && '
                             'rm -f /tmp/hotdvr-mac.zip && ditto -c -k --keepParent "House of the Dead VR.app" /tmp/hotdvr-mac.zip'
                 % remote])
            run(['scp', MAC + ':/tmp/hotdvr-mac.zip', os.path.join(out, name)])
        else:
            run(['ssh', MAC, 'cd %s && python3 scripts/build_apple.py device --app --ipa' % remote])
            run(['scp', MAC + ':%s/build_ios/HouseOfTheDeadVR-%s.ipa' % (remote, version()), os.path.join(out, name)])


def version():
    man = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    return re.search(r'android:versionName="([^"]+)"', man).group(1)


def table(out, v, commit):
    rows = ['## Downloads', '', '| File | For | SHA-256 |', '| --- | --- | --- |']
    for key, suffix, what in PLATFORMS:
        f = 'house-of-the-dead-vr-%s%s' % (v, suffix)
        if os.path.exists(os.path.join(out, f)):
            rows.append('| `%s` | %s | `%s` |' % (f, what, sha256(os.path.join(out, f))))
    rows += ['', 'Build %s. Every download is built from the same commit. None contains any of the game: bring your '
                 'own MAME ROM set (`hotd.zip` or `hotd.7z`, Revision A).' % commit, '']
    return '\n'.join(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apple', action='store_true', help='the Mac app and the iOS IPA too, on the Mac (%s)' % MAC)
    ap.add_argument('--only', help='these platforms only (comma-separated keys)')
    ap.add_argument('--publish', action='store_true', help='create or update the GitHub release')
    a = ap.parse_args()
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('release: uncommitted changes (the builds stamp the commit): commit them first')
    v, commit = version(), git('rev-parse', '--short=8', 'HEAD')
    keys = [k for k, _, _ in PLATFORMS if (a.apple or k not in ('mac', 'ios'))]
    if a.only: keys = [k for k in keys if k in a.only.split(',')]
    out = os.path.join(ROOT, 'build_release', v)
    os.makedirs(out, exist_ok=True)
    log('v%s at %s: %s' % (v, commit, ', '.join(keys)))
    if 'mac' in keys or 'ios' in keys: run([sys.executable, os.path.join(ROOT, 'scripts', 'apple_sync.py')])
    for key, suffix, _ in PLATFORMS:
        if key in keys: build(key, out, 'house-of-the-dead-vr-%s%s' % (v, suffix))
    if 'quest' in keys: run([sys.executable, os.path.join(ROOT, 'scripts', 'quest_tools.py'), 'symbols'])
    # the notes' downloads table (written over the old one)
    notes = os.path.join(ROOT, 'docs', 'releases', 'v%s.md' % v)
    os.makedirs(os.path.dirname(notes), exist_ok=True)
    text = open(notes, encoding='utf-8').read() if os.path.exists(notes) else '# House of the Dead VR %s\n\n' % v
    text = re.sub(r'\n## Downloads\n.*', '\n', text, flags=re.S).rstrip('\n') + '\n\n' + table(out, v, commit)
    open(notes, 'w', encoding='utf-8', newline='\n').write(text)
    log('notes: %s' % notes)
    files = [os.path.join(out, f) for f in sorted(os.listdir(out))]
    for f in files: log('  %s  %s' % (sha256(f)[:16], os.path.basename(f)))
    if not a.publish: return
    tag = 'v' + v
    exists = subprocess.run(['gh', 'release', 'view', tag, '-R', REPO], capture_output=True).returncode == 0
    if exists:
        run(['gh', 'release', 'upload', tag, '-R', REPO, '--clobber'] + files)
        run(['gh', 'release', 'edit', tag, '-R', REPO, '--notes-file', notes])
    else:
        run(['gh', 'release', 'create', tag, '-R', REPO, '--title', 'House of the Dead VR ' + v, '--notes-file', notes,
             '--target', git('rev-parse', 'HEAD')] + (['--prerelease'] if '-' in v else ['--latest']) + files)
    log('published %s' % tag)


if __name__ == '__main__':
    main()
