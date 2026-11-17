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
  --zip   build_windows/HOTDVR-<version>-windows.zip: HOTDVR.exe, SDL3.dll (game pads), no ROM
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


def pdb_id(exe):
    """The PDB's GUID and age from the exe's CodeView debug record (the PE debug directory), as crash_win.c writes it
    (and a symbol server names it)."""
    import struct
    d = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    rva, size = struct.unpack_from('<II', d, opt + 112 + 6 * 8)   # PE32+: data directory 6, debug
    secs = opt + struct.unpack_from('<H', d, pe + 20)[0]

    def off(r):   # an RVA in the file
        for i in range(nsec):
            va, vsize, raw = struct.unpack_from('<III', d, secs + 40 * i + 12)[0:3:2] + (struct.unpack_from('<I', d, secs + 40 * i + 20)[0],)
            vsz = struct.unpack_from('<I', d, secs + 40 * i + 8)[0]
            if va <= r < va + max(vsz, 1): return raw + r - va
        return None
    base = off(rva)
    for i in range(size // 28 if base is not None else 0):
        typ, sz, addr, raw = struct.unpack_from('<IIII', d, base + 28 * i + 12)
        if typ != 2 or d[raw:raw + 4] != b'RSDS': continue   # IMAGE_DEBUG_TYPE_CODEVIEW
        g1, g2, g3 = struct.unpack_from('<IHH', d, raw + 4)
        age, = struct.unpack_from('<I', d, raw + 20)
        return '%08X%04X%04X%s%X' % (g1, g2, g3, d[raw + 12:raw + 20].hex().upper(), age)
    return ''


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
    # (/Z7: debug information in every object, for HOTDVR.pdb: the crash reports' frames get names from it)
    subprocess.check_call(['cmake', '-S', ROOT, '-B', BDIR, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                           '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
                           '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7', '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7',
                           '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON',
                           '-DM2_GEN_ROOT=' + gen.replace('\\', '/')], env=env)
    subprocess.check_call(['cmake', '--build', BDIR, '--target', 'hotdvr'], env=env)
    # SDL3.dll for the game pads (pad_win.c loads it at run time: HOTDVR.exe's own host_sdl.c is the SDL the game
    # sees, so it cannot link SDL itself): the gamepad, joystick and sensor parts only
    sdl_dir = os.path.join(ROOT, 'build_houseofthedead', 'win', 'sdl3dll')
    if os.path.exists(os.path.join(ROOT, 'extern', 'sdl3', 'CMakeLists.txt')):
        off = ['SDL_STATIC', 'SDL_TEST_LIBRARY', 'SDL_TESTS', 'SDL_EXAMPLES', 'SDL_AUDIO', 'SDL_VIDEO', 'SDL_RENDER', 'SDL_GPU',
               'SDL_CAMERA', 'SDL_DIALOG', 'SDL_POWER', 'SDL_TRAY', 'SDL_INSTALL']
        subprocess.check_call(['cmake', '-S', os.path.join(ROOT, 'extern', 'sdl3'), '-B', sdl_dir, '-G', 'Ninja',
                               '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=cl', '-DSDL_SHARED=ON']
                              + ['-D%s=OFF' % o for o in off], env=env)
        subprocess.check_call(['cmake', '--build', sdl_dir], env=env)
    else:
        port.log('no extern/sdl3 (python scripts/setup.py fetches it): pads through XInput only')
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, 'HOTDVR.exe')
    shutil.copy2(os.path.join(BDIR, 'HOTDVR.exe'), exe)
    if os.path.exists(os.path.join(sdl_dir, 'SDL3.dll')): shutil.copy2(os.path.join(sdl_dir, 'SDL3.dll'), os.path.join(OUT, 'SDL3.dll'))
    port.log('built %s' % exe)
    # its PDB, kept by build id (crash_win.c's: the PDB's GUID and age) for the crash reports' frames
    bid = pdb_id(exe)
    if bid and os.path.exists(os.path.join(BDIR, 'HOTDVR.pdb')):
        sym = os.path.join(OUT, 'symbols', bid)
        os.makedirs(sym, exist_ok=True)
        shutil.copy2(os.path.join(BDIR, 'HOTDVR.pdb'), os.path.join(sym, 'HOTDVR.pdb'))
        port.log('symbols: %s' % sym)
    if a.zip:
        z = os.path.join(OUT, 'HOTDVR-%s-windows.zip' % version())
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as f:
            f.write(exe, 'HOTDVR/HOTDVR.exe')
            if os.path.exists(os.path.join(OUT, 'SDL3.dll')): f.write(os.path.join(OUT, 'SDL3.dll'), 'HOTDVR/SDL3.dll')
            f.writestr('HOTDVR/files/arcade/PUT hotd.zip HERE.txt',
                       'Your own MAME ROM set of The House of the Dead (hotd, Revision A), as hotd.zip or hotd.7z, goes in this folder.\r\n')
        port.log('zip %s' % z)
    if a.run:
        subprocess.Popen([exe], cwd=OUT)


if __name__ == '__main__':
    main()
