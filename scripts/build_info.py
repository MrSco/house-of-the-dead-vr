"""What a build says about itself in its crash reports (tools/vr/crash-relay): the version and the commit, as CMake
definitions for src/platform/windows/crash_win.c and src/platform/apple/crash_apple.m (the Android builds have
assets/build-info.txt instead: quest_tools.py), and the Apple builds' debug information for their dSYMs.

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


def apple_debug_args(root):
    """Line tables in every Apple library, for the app's dSYM (Xcode's dsymutil collects them; the Mac app's from
    cmake/flat_apple.cmake), with the checkout's folder left out of the paths (as the repository's own: src/...)."""
    flags = '-gline-tables-only -ffile-prefix-map=%s=.' % os.path.abspath(root)
    return ['-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags, '-DCMAKE_OBJC_FLAGS=' + flags]
