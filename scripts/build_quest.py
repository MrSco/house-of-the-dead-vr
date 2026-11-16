#!/usr/bin/env python3
"""The arcade game in the Quest VR app: an APK of its own (com.mrsco.hotdvr).

  python scripts/build_quest.py [--apk-only] [--no-launch]

The app: src/quest (OpenXR, the guns and hands, the menus, the cabinet room, voice; the bridge to the game
and the Model 2 shading) and src/net (online play), with the runtime and the generated code built by CMake for
arm64 Android. quest_tools.py finds the Android SDK, fetches the OpenXR loader, SDL2's headers and the voice
libraries, compiles, packages and signs.

Needs the generated code first (python scripts/recompile.py --game houseofthedead on the desktop). The ROM set is the
player's: the app asks for hotd.zip on its first start (ImportActivity), or
  adb push hotd.zip /sdcard/Android/data/com.mrsco.hotdvr/files/arcade/hotd.zip
"""
import argparse
import glob
import os
import subprocess
import sys

ARCADE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quest_tools as port  # noqa: E402

PKG = 'com.mrsco.hotdvr'
LABEL = 'House of the Dead VR'
SET = 'hotd'


def cmake_tools():
    """VS's CMake and Ninja on Windows (Strawberry's have bitten: LNK1168), else whatever is on PATH."""
    vs = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake'
    if os.name == 'nt' and os.path.isdir(vs):
        return os.path.join(vs, 'CMake', 'bin', 'cmake.exe'), os.path.join(vs, 'Ninja')
    return 'cmake', None


def build_core(tools, sdl_inc):
    """The runtime, the generated code and the bridge (CMake, arm64 Android): its static libraries."""
    cmake, ninja_dir = cmake_tools()
    env = dict(os.environ)
    if ninja_dir: env['PATH'] = ninja_dir + os.pathsep + env['PATH']
    bdir = os.path.join(ARCADE, 'build_houseofthedead', 'android', SET)
    gen = os.path.join(ARCADE, 'build_houseofthedead', 'recomp', SET, 'gen')
    if not os.path.isdir(gen): sys.exit('no generated code in %s: run python scripts/recompile.py --game houseofthedead' % gen)
    subprocess.check_call([cmake, '-S', ARCADE, '-B', bdir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                           '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(tools['ndk'], 'build', 'cmake', 'android.toolchain.cmake'),
                           # (API 32: every Quest that runs the app has Android 12 or later; the app's own code
                           # keeps port.py's minimum)
                           '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-32',
                           '-DM2_ROMSET=' + SET, '-DM2_GAME=houseofthedead', '-DM2_SPIN_SKIP=ON',
                           '-DM2_GEN_ROOT=' + gen.replace('\\', '/'),
                           '-DM2_QUEST_SDL2=' + sdl_inc.replace('\\', '/')], env=env)
    subprocess.check_call([cmake, '--build', bdir, '--target', 'arcade_quest'], env=env)
    libs = [os.path.join(bdir, n) for n in ('libarcade_quest.a', 'libruntime.a', 'libi960.a', 'libsoftfloat.a',
                                             'libymfm.a', 'liblzma7z.a', 'libtrace.a')]
    return [l for l in libs if os.path.exists(l)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apk-only', action='store_true', help='build the APK, do not install it')
    ap.add_argument('--no-launch', action='store_true', help='install but do not start it')
    a = ap.parse_args()
    os.chdir(ARCADE)                     # (quest_tools' paths are relative to the repository's root)
    tools = port.find_android()
    sdl_inc, xr_inc, xr_lib = port.quest_thirdparty()
    libs = build_core(tools, sdl_inc)

    out = os.path.join(ARCADE, 'build_quest', 'arcade')
    glue = os.path.join(ARCADE, 'build_quest', 'glue')
    os.makedirs(glue, exist_ok=True)
    for n in ('android_native_app_glue.c', 'android_native_app_glue.h'):
        s, d = os.path.join(tools['ndk'], 'sources', 'android', 'native_app_glue', n), os.path.join(glue, n)
        if not os.path.exists(d) or open(s, 'rb').read() != open(d, 'rb').read():
            import shutil
            shutil.copy2(s, d)
    target = '--target=aarch64-linux-android%d' % port.QUEST_API
    base = [tools['clang'], target, '-fPIC', '-O2', '-fno-strict-aliasing', '-ffp-contract=off']
    sflags = ['-Wall', '-Wno-unused-function', '-gline-tables-only', '-DHOTD_ARCADE', '-Isrc', '-Isrc/quest', '-Isrc/net',
              '-Ibuild_quest/glue', '-I' + sdl_inc, '-I' + xr_inc]
    quest = ['src/quest/quest_sdl.c', 'src/quest/quest_xr.c', 'src/quest/quest_gun.c', 'src/quest/quest_hand.c',
             'src/quest/quest_menu.c', 'src/quest/quest_report.c', 'src/quest/quest_canvas.c', 'src/quest/quest_options.c',
             'src/quest/quest_cabinet.c',
             'build_quest/glue/android_native_app_glue.c', 'src/quest/m2_gl.c']
    jobs = [(s, sflags) for s in quest] + [(s, sflags) for s in port.net_sources()]
    jobs += port.voice_jobs(sflags)
    objs = port.compile_all(jobs, os.path.join(out, 'obj'), lambda s, o, flags: base + flags + ['-c', s, '-o', o])
    so = os.path.join(out, 'libhotdvr.so')
    rsp = os.path.join(out, 'link.rsp')
    open(rsp, 'w').write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    clangxx = tools['clang'].replace('clang.exe', 'clang++.exe') if tools['clang'].endswith('.exe') else tools['clang'] + '++'
    port.run([clangxx, target, '-shared', '-o', so, '@' + rsp, '-Wl,--start-group'] + libs + ['-Wl,--end-group',
              '-static-libstdc++', '-L' + xr_lib, '-lopenxr_loader', '-landroid', '-llog', '-lEGL', '-lGLESv3', '-laaudio',
              '-lz', '-lm', '-Wl,-u,ANativeActivity_onCreate', '-Wl,-soname,libhotdvr.so', '-Wl,-z,max-page-size=16384',
              '-Wl,--build-id=sha1', '-Wl,--no-undefined'], cwd=ARCADE)
    port.keep_symbols(so)
    apk = port.package_apk(tools, out, so, xr_lib, PKG, LABEL)
    if not a.apk_only:
        port.quest_deploy(tools, apk, PKG, not a.no_launch)


if __name__ == '__main__':
    main()
