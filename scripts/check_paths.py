#!/usr/bin/env python3
"""No build machine's home folder or user name in a download: a release's guard (scripts/release.py runs it).

  python scripts/check_paths.py [PATH ...] [--names a,b] [--quiet]

Looks through every file under each PATH (default: build_release/<version>/, the version being
src/quest/AndroidManifest.xml's versionName), into zips, APKs and IPAs and the archives inside them, and through every
member's bytes (executables, libraries, frameworks, assets) for:

  - a Windows user folder: C:\\Users\\<name>\\ or C:/Users/<name>/ (any drive)
  - a Unix one: /Users/<name>/ (macOS) or /home/<name>/
  - this machine's names, read at run time: USERNAME, USER, LOGNAME, the last part of HOME and USERPROFILE, the host's
    name, $HOTD_MAC's user and host (the Mac that builds the Apple apps), and any in --names or $HOTD_PRIVATE_NAMES
    (comma-separated)

in 8-bit text and in UTF-16 strings (Windows' wide ones). The shared folders (C:\\Users\\Public, /Users/Shared, ...)
don't count. Exit status 1 when anything is found (it prints where, with the text around it), 0 when clean.

The builds keep these out (docs/platforms.md, "Releases"): every compiler maps the checkout's folder to . and the home
folder to ~ (cmake/prefix_map.cmake, scripts/build_info.py), and the Apple builds strip ANGLE's libraries
(build_info.py clean_angle). Never write a real name into this file: it is public.
"""
import argparse
import io
import os
import re
import socket
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVES = ('.zip', '.apk', '.ipa', '.aar', '.jar', '.aab')
SHARED = {'public', 'default', 'default user', 'all users', 'shared', 'guest'}
NAME_CH = rb'[^\\/\x00-\x1f"<>|:*?]{1,64}'      # one folder's name
WIDE = re.compile(rb'(?:[\x20-\x7e]\x00){4,}')   # a UTF-16 string (ASCII letters)


def own_names(extra=()):
    """this machine's user and host names (none shorter than 3 letters: too common to mean anything)"""
    names = set(extra)
    for k in ('USERNAME', 'USER', 'LOGNAME'):
        names.add(os.environ.get(k, ''))
    for k in ('HOME', 'USERPROFILE'):
        names.add(os.path.basename(os.environ.get(k, '').rstrip('\\/')))
    try:
        names.add(socket.gethostname().split('.')[0])
    except OSError:
        pass
    user, _, host = os.environ.get('HOTD_MAC', '').rpartition('@')
    names.update([user, host.split('.')[0]])
    names.update(os.environ.get('HOTD_PRIVATE_NAMES', '').split(','))
    names = {n.strip() for n in names}
    return sorted(n for n in names if len(n) >= 3 and n.lower() not in SHARED | {'localhost', 'root'})


def patterns(names):
    """[(what, compiled pattern)] over 8-bit text (UTF-16 strings are scanned as 8-bit: scan_bytes)"""
    out = [('Windows user folder', re.compile(rb'(?i)(?<![A-Za-z])[A-Z]:[\\/]+Users[\\/]+(' + NAME_CH + rb')[\\/]')),
           ('macOS user folder', re.compile(rb'/Users/(' + NAME_CH + rb')/')),
           ('Linux home folder', re.compile(rb'/home/(' + NAME_CH + rb')/'))]
    for n in names:
        b = re.escape(n.encode('utf-8'))
        out.append(("this machine's name", re.compile(rb'(?i)(?<![A-Za-z0-9])(' + b + rb')(?![A-Za-z0-9])')))
    return out


def scan_bytes(name, data, pats, hits):
    """data's 8-bit text, then its UTF-16 strings (Windows' wide ones: MSVC's assert messages, resources)"""
    wide = b'\n'.join(m.group(0)[::2] for m in WIDE.finditer(data))
    for text, enc in ((data, ''), (wide, ' (UTF-16)')):
        for what, p in pats:
            for m in p.finditer(text):
                if what.endswith('folder') and m.group(1).decode('utf-8', 'replace').strip().lower() in SHARED:
                    continue
                ctx = text[max(0, m.start() - 24):m.end() + 48]
                hits.setdefault(name, []).append((what + enc, ctx.replace(b'\x00', b'|').decode('latin-1')))


def scan_member(label, data, pats, hits):
    """a file's bytes, or an archive's members (recursively)"""
    if label.lower().endswith(ARCHIVES) or data[:4] == b'PK\x03\x04':
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for info in z.infolist():
                    if not info.is_dir() and info.file_size:
                        scan_member('%s!%s' % (label, info.filename), z.read(info), pats, hits)
            return
        except zipfile.BadZipFile:
            pass
    scan_bytes(label, data, pats, hits)


def scan_path(path, pats, hits):
    """every file under path (a file, or a folder: an .app bundle, a release's folder); the number of files"""
    files = [path] if os.path.isfile(path) else [os.path.join(dp, f) for dp, _, fs in os.walk(path) for f in fs]
    base = os.path.dirname(os.path.abspath(path))
    for f in sorted(files):
        if os.path.islink(f): continue
        with open(f, 'rb') as fh:
            scan_member(os.path.relpath(os.path.abspath(f), base), fh.read(), pats, hits)
    return len(files)


def version():
    man = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    return re.search(r'android:versionName="([^"]+)"', man).group(1)


def check(paths, names=(), quiet=False, log=print):
    """the hits ({file: [(what, text around it)]}), printed; {} when clean"""
    pats = patterns(own_names(names))
    hits, n = {}, 0
    for p in paths:
        if not os.path.exists(p): sys.exit('check_paths: no %s' % p)
        n += scan_path(p, pats, hits)
    for f, hs in sorted(hits.items()):
        log('check_paths: %s: %d' % (f, len(hs)))
        if quiet: continue
        shown = []
        for h in hs:
            if h in shown: continue
            if len(shown) == 6:
                log('    ...')
                break
            shown.append(h)
            log('    %s: %r' % h)
    log('check_paths: %d files, %s' % (n, 'clean' if not hits else '%d hits in %d members'
                                        % (sum(len(v) for v in hits.values()), len(hits))))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', help='files or folders (default: build_release/<version>)')
    ap.add_argument('--names', default='', help='more names to look for (comma-separated)')
    ap.add_argument('--quiet', action='store_true', help='counts only')
    a = ap.parse_args()
    paths = a.paths or [os.path.join(ROOT, 'build_release', version())]
    sys.exit(1 if check(paths, [n for n in a.names.split(',') if n], a.quiet) else 0)


if __name__ == '__main__':
    main()
