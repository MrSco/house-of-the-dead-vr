#!/usr/bin/env python3
"""The game on Windows, flat (docs/platforms.md, P1): HOTDVR.exe, and a zip to hand out.

  python scripts/build_windows.py [--run] [--zip]

The app: src/flat (the flat shell: win/main_win.c, the picture, the app round the game), src/core (the game's bridge,
the renderer, the menu, voice chat), src/net (online play) and src/platform/windows (WASAPI, WGL, the OS), with the
runtime and the generated code. Built with Visual Studio's MSVC (/fp:strict: the same game as the Quest's, bit for
bit) and Ninja, in Visual Studio's own environment (vcvars64).

Needs the generated code first (python scripts/recompile.py --game houseofthedead). The ROM set is the player's:
files/arcade/hotd.zip (or hotd.7z) beside the exe.

  --run   start it afterwards (with the files folder beside it)
  --zip   build_windows/HOTDVR-<version>.zip: HOTDVR.exe and a README, no ROM
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quest_tools as port  # noqa: E402

SET = 'hotd'
BDIR = os.path.join(ROOT, 'build_houseofthedead', 'win', SET)
OUT = os.path.join(ROOT, 'build_windows')


def vs_env():
    """Visual Studio's x64 build environment (vcvars64.bat's), as a dict."""
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    paths = []
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, '-all', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                              '-format', 'json'], capture_output=True, text=True).stdout
        # the newest first (C11 atomics, voice.c's, came with MSVC 14.3x)
        found = sorted(json.loads(out or '[]'), key=lambda i: [int(x) for x in i.get('installationVersion', '0').split('.')], reverse=True)
        paths = [i['installationPath'] for i in found]
    for p in paths:
        bat = os.path.join(p, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
        if not os.path.exists(bat): continue
        out = subprocess.run('"%s" >nul && set' % bat, shell=True, capture_output=True, text=True).stdout
        env = dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
        if env.get('VCToolsInstallDir'):
            # Visual Studio's CMake and Ninja too
            ide = os.path.join(p, 'Common7', 'IDE', 'CommonExtensions', 'Microsoft', 'CMake')
            env['PATH'] = os.pathsep.join([os.path.join(ide, 'CMake', 'bin'), os.path.join(ide, 'Ninja'), env.get('PATH', '')])
            return env
    sys.exit('build_windows: no Visual Studio with the C++ tools (MSVC x64) found')


def version():
    text = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    m = re.search(r'android:versionName="([^"]+)"', text)
    return m.group(1) if m else '0'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run', action='store_true', help='start it afterwards')
    ap.add_argument('--zip', action='store_true', help='make the zip to hand out')
    a = ap.parse_args()
    os.chdir(ROOT)
    gen = os.path.join(ROOT, 'build_houseofthedead', 'recomp', SET, 'gen')
    if not os.path.isdir(gen): sys.exit('no generated code in %s: run python scripts/recompile.py --game houseofthedead' % gen)
    # the SDL2 headers and the voice libraries (shared with the Quest build's third_party/quest)
    port.quest_thirdparty()
    port.voice_jobs([])
    env = vs_env()
    subprocess.check_call(['cmake', '-S', ROOT, '-B', BDIR, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                           '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
                           '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON',
                           '-DM2_GEN_ROOT=' + gen.replace('\\', '/')], env=env)
    subprocess.check_call(['cmake', '--build', BDIR, '--target', 'hotdvr'], env=env)
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, 'HOTDVR.exe')
    shutil.copy2(os.path.join(BDIR, 'HOTDVR.exe'), exe)
    port.log('built %s' % exe)
    if a.zip:
        z = os.path.join(OUT, 'HOTDVR-%s-windows.zip' % version())
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as f:
            f.write(exe, 'HOTDVR/HOTDVR.exe')
            f.writestr('HOTDVR/files/arcade/PUT hotd.zip HERE.txt',
                       'Your own MAME ROM set of The House of the Dead (hotd, Revision A), as hotd.zip or hotd.7z, goes in this folder.\r\n')
        port.log('zip %s' % z)
    if a.run:
        subprocess.Popen([exe], cwd=OUT)


if __name__ == '__main__':
    main()
