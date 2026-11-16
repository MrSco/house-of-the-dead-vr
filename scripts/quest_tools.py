#!/usr/bin/env python3
"""The Quest app's build helpers (build_quest.py): the Android SDK, the OpenXR loader, SDL2's headers and the voice
libraries (fetched once into third_party/quest), compiling, packaging and signing the APK, installing it, and the
crash reports' symbols.

    python scripts/quest_tools.py symbols      # upload the builds' symbols for crash reports (private repository)

Paths are relative to the repository's root (build_quest.py works there).
"""
import concurrent.futures, hashlib, os, shutil, subprocess, sys, time, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # the repository
THIRD_PARTY = os.path.join(ROOT, 'third_party', 'quest')
# the SDL2 headers (the app implements the part of SDL it uses, src/quest/quest_sdl.c) and the Khronos OpenXR loader
SDL2_SRC = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip'
OPENXR_VER = '1.1.53'
OPENXR_AAR = ('https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/%s/'
              'openxr_loader_for_android-%s.aar' % (OPENXR_VER, OPENXR_VER))
QUEST_API = 29          # the APK's minimum (Quest 2 and later run Android 10+)
# Crash reports (src/quest/quest_report.c, tools/vr/crash-relay): the private repository whose issues they land in, and
# the release there that holds each build's unstripped libhotdvr.so, named by its GNU build id. The library holds the
# recompiled game, so it only ever goes to that private repository.
CRASH_REPO = os.environ.get('HOTD_CRASH_REPO', 'MrSco/house-of-the-dead-recomp-vr')
SYMBOLS_TAG = 'crash-symbols'
SYMBOLS_KEEP = 30       # builds kept in build_quest/symbols


def log(*a):
    print('[quest]', *a, flush=True)


def run(cmd, **kw):
    log(' '.join(str(c) for c in cmd) if len(cmd) < 12 else '%s ... (%d args)' % (cmd[0], len(cmd)))
    subprocess.check_call(cmd, **kw)


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def net_sources():
    """Online co-op for the native builds (src/net: the NMN2 client and LAN host, online.c)."""
    return sorted('src/net/' + f for f in os.listdir(os.path.join(ROOT, 'src', 'net')) if f.endswith('.c'))


def compile_all(jobs, out_dir, cc_for):
    """jobs: list of (source, flags). Rebuilds objects older than their source or any header."""
    hdr_time = max(os.path.getmtime(os.path.join(ROOT, d, f)) for d in (os.path.join('src', 'quest'), os.path.join('src', 'net'))
                   if os.path.isdir(os.path.join(ROOT, d)) for f in os.listdir(os.path.join(ROOT, d)) if f.endswith('.h'))
    objs, todo = [], []
    for s, flags in jobs:
        o = os.path.join(out_dir, s.replace(os.sep, '_').replace('/', '_')[:-2] + '.o')
        objs.append(o)
        sp = os.path.join(ROOT, s)
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(sp), hdr_time):
            todo.append(cc_for(sp, o, flags))
    os.makedirs(out_dir, exist_ok=True)
    if todo:
        log('compiling %d files' % len(todo))
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            for r in ex.map(lambda c: subprocess.run(c, cwd=ROOT, capture_output=True, text=True), todo):
                if r.returncode:
                    sys.stderr.write(r.stdout + r.stderr)
                    sys.exit('compile failed: %s' % r.args[-3])
    return objs


# ------------------------------------------------------------------ web


def _ver(name):
    return tuple(int(p) if p.isdigit() else -1 for p in name.replace('android-', '').replace('-', '.').split('.'))


def find_android():
    """The Android SDK pieces the Quest build uses: NDK clang, aapt2, zipalign, apksigner, android.jar, java, adb."""
    home = os.path.expanduser('~')
    help_ = ('Install the Android SDK (Android Studio, or the command-line tools) with an NDK, build-tools, a platform '
             'and platform-tools, e.g.  sdkmanager "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35" '
             '"platform-tools"  plus a Java 17+ runtime, and set ANDROID_HOME if the SDK lives somewhere else.')
    sdk = next((c for c in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
                            os.path.join(os.environ.get('LOCALAPPDATA', home), 'Android', 'Sdk'),
                            os.path.join(home, 'Library', 'Android', 'sdk'), os.path.join(home, 'Android', 'Sdk'))
                if c and os.path.isdir(os.path.join(c, 'platforms'))), None)
    if not sdk: sys.exit('Android SDK not found. ' + help_)

    def newest(d, ok):
        names = sorted((n for n in (os.listdir(d) if os.path.isdir(d) else []) if ok(os.path.join(d, n))), key=_ver)
        return os.path.join(d, names[-1]) if names else None
    exe = '.exe' if os.name == 'nt' else ''
    host = 'windows-x86_64' if os.name == 'nt' else 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    ndk = (os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
           or newest(os.path.join(sdk, 'ndk'), lambda p: os.path.isdir(os.path.join(p, 'toolchains'))))
    nbin = os.path.join(ndk or '', 'toolchains', 'llvm', 'prebuilt', host, 'bin')
    bt = newest(os.path.join(sdk, 'build-tools'), lambda p: os.path.exists(os.path.join(p, 'aapt2' + exe))
                and os.path.exists(os.path.join(p, 'lib', 'apksigner.jar')))
    plat = newest(os.path.join(sdk, 'platforms'), lambda p: os.path.exists(os.path.join(p, 'android.jar')))
    java = os.path.join(os.environ['JAVA_HOME'], 'bin', 'java' + exe) if os.environ.get('JAVA_HOME') else shutil.which('java')
    adb = os.path.join(sdk, 'platform-tools', 'adb' + exe)
    missing = [n for n, ok in (('NDK', os.path.exists(os.path.join(nbin, 'clang' + exe))), ('build-tools', bt),
                               ('platform', plat), ('java', java)) if not ok]
    if missing: sys.exit('The Android SDK at %s is missing: %s. %s' % (sdk, ', '.join(missing), help_))
    return dict(ndk=ndk, clang=os.path.join(nbin, 'clang' + exe), strip=os.path.join(nbin, 'llvm-strip' + exe),
                aapt2=os.path.join(bt, 'aapt2' + exe), zipalign=os.path.join(bt, 'zipalign' + exe),
                apksigner=os.path.join(bt, 'lib', 'apksigner.jar'), android_jar=os.path.join(plat, 'android.jar'),
                java=java, adb=adb if os.path.exists(adb) else shutil.which('adb'), d8=os.path.join(bt, 'lib', 'd8.jar'))


def quest_thirdparty():
    """SDL2 headers and the OpenXR loader (headers + arm64 library), fetched once into third_party/quest/."""
    tp = THIRD_PARTY
    dl = os.path.join(tp, 'dl')
    os.makedirs(dl, exist_ok=True)
    sdl_name = os.path.basename(SDL2_SRC)[:-4]                      # SDL2-2.32.10
    sdl = os.path.join(tp, sdl_name + '-headers', 'include')
    if not os.path.exists(os.path.join(sdl, 'SDL.h')):
        z = os.path.join(dl, sdl_name + '.zip')
        if not os.path.exists(z):
            log('downloading the SDL2 headers (%s)' % sdl_name)
            urllib.request.urlretrieve(SDL2_SRC, z)
        os.makedirs(sdl, exist_ok=True)
        with zipfile.ZipFile(z) as zf:
            for n in zf.namelist():
                parts = n.split('/')
                if len(parts) == 3 and parts[1] == 'include' and parts[2].endswith('.h'):
                    open(os.path.join(sdl, parts[2]), 'wb').write(zf.read(n))
    xr = os.path.join(tp, 'openxr-loader-' + OPENXR_VER)
    if not os.path.exists(os.path.join(xr, 'lib', 'libopenxr_loader.so')):
        a = os.path.join(dl, 'openxr_loader_for_android-%s.aar' % OPENXR_VER)
        if not os.path.exists(a):
            log('downloading the Khronos OpenXR loader %s' % OPENXR_VER)
            urllib.request.urlretrieve(OPENXR_AAR, a)
        os.makedirs(os.path.join(xr, 'include', 'openxr'), exist_ok=True)
        os.makedirs(os.path.join(xr, 'lib'), exist_ok=True)
        with zipfile.ZipFile(a) as zf:
            for n in zf.namelist():
                if n.startswith('prefab/modules/headers/include/openxr/') and n.endswith('.h'):
                    open(os.path.join(xr, 'include', 'openxr', os.path.basename(n)), 'wb').write(zf.read(n))
            open(os.path.join(xr, 'lib', 'libopenxr_loader.so'), 'wb').write(zf.read('jni/arm64-v8a/libopenxr_loader.so'))
    return sdl, os.path.join(xr, 'include'), os.path.join(xr, 'lib')


# Voice chat (src/quest/voice.c): Opus, SpeexDSP's echo canceller and RNNoise, all BSD licences
# (src/quest/VOICE-LICENSES.md). Release archives checked by their sha256: the versions the Time Crisis 2 VR project uses.


# Voice chat (src/quest/voice.c): Opus, SpeexDSP's echo canceller and RNNoise, all BSD licences
# (src/quest/VOICE-LICENSES.md). Release archives checked by their sha256: the versions the Time Crisis 2 VR project uses.
VOICE_LIBS = [
    ('opus-1.5.2', 'https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
     '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'),
    ('speexdsp-1.2.1', 'https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz',
     '8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d'),
    ('rnnoise-0.2', 'https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz',
     '90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37'),
]


def voice_jobs(sflags):
    """Voice chat's libraries, unpacked once into third_party/quest/ (archives kept in third_party/quest/dl), and the Quest build's
    compile jobs for them and the voice code: (source, flags)."""
    import tarfile
    tp = THIRD_PARTY
    dl = os.path.join(tp, 'dl')
    os.makedirs(dl, exist_ok=True)
    for name, url, sha in VOICE_LIBS:
        if os.path.exists(os.path.join(tp, name, '.complete')): continue
        a = os.path.join(dl, os.path.basename(url))
        if not os.path.exists(a) or sha256(a) != sha:
            log('downloading %s' % name)
            urllib.request.urlretrieve(url, a + '.part')
            os.replace(a + '.part', a)
        if sha256(a) != sha: sys.exit('%s is not the expected archive (sha256 %s)' % (a, sha256(a)))
        shutil.rmtree(os.path.join(tp, name), ignore_errors=True)
        with tarfile.open(a) as t:
            t.extractall(tp, **({'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}))
        open(os.path.join(tp, name, '.complete'), 'w').write(url + '\n')
    opus, speex, rnn = ('third_party/quest/' + n for n, _, _ in VOICE_LIBS)
    types = os.path.join(ROOT, speex, 'include', 'speex', 'speexdsp_config_types.h')   # what its configure writes
    if not os.path.exists(types):
        open(types, 'w').write('#pragma once\n#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\n'
                               'typedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n')

    def mk(name, *lists):   # Opus's own source lists (celt_sources.mk, ...)
        text = open(os.path.join(ROOT, opus, name)).read().replace('\\\n', ' ')
        return [opus + '/' + f for line in text.splitlines() for k, _, v in [line.partition('=')]
                if k.strip() in lists for f in v.split()]
    # Opus as its CMake build configures it for arm64: floating point, NEON intrinsics presumed (no run-time check)
    opus_src = (mk('celt_sources.mk', 'CELT_SOURCES', 'CELT_SOURCES_ARM_NEON_INTR')
                + mk('silk_sources.mk', 'SILK_SOURCES', 'SILK_SOURCES_FLOAT', 'SILK_SOURCES_ARM_NEON_INTR')
                + mk('opus_sources.mk', 'OPUS_SOURCES', 'OPUS_SOURCES_FLOAT'))
    oflags = ['-w', '-DOPUS_BUILD', '-DVAR_ARRAYS', '-DHAVE_LRINT', '-DHAVE_LRINTF', '-DENABLE_HARDENING',
              '-DOPUS_ARM_MAY_HAVE_NEON_INTR', '-DOPUS_ARM_PRESUME_NEON_INTR', '-DPACKAGE_VERSION="1.5.2"', '-I' + opus,
              '-I%s/include' % opus, '-I%s/celt' % opus, '-I%s/silk' % opus, '-I%s/silk/float' % opus, '-I%s/silk/fixed' % opus]
    # SpeexDSP: the echo canceller alone, floating point with its own small FFT
    speex_src = [speex + '/libspeexdsp/' + f for f in ('mdf.c', 'fftwrap.c', 'smallft.c')]
    sxflags = ['-w', '-DFLOATING_POINT', '-DUSE_SMALLFT', '-DEXPORT=', '-I%s/include' % speex]
    # RNNoise with its trained model; its NEON code wants Opus's celt/os_support.h (left out of its archive)
    rnn_src = [rnn + '/src/' + f for f in ('denoise.c', 'rnn.c', 'pitch.c', 'kiss_fft.c', 'celt_lpc.c', 'nnet.c',
                                           'nnet_default.c', 'parse_lpcnet_weights.c', 'rnnoise_data.c', 'rnnoise_tables.c')]
    rflags = ['-w', '-I%s/include' % rnn, '-I%s/src' % rnn, '-I%s/celt' % opus, '-I%s/include' % opus]
    vflags = sflags + ['-I%s/include' % opus, '-I%s/include' % speex, '-I%s/include' % rnn]
    return ([(s, vflags) for s in ('src/quest/voice.c', 'src/quest/voice_dsp.c')] + [(s, oflags) for s in opus_src]
            + [(s, sxflags) for s in speex_src] + [(s, rflags) for s in rnn_src])


def quest_keystore(tools):
    """The usual Android debug key (~/.android/debug.keystore), made in build_quest/ if this machine has none."""
    ks = os.path.join(os.path.expanduser('~'), '.android', 'debug.keystore')
    if os.path.exists(ks): return ks
    ks = os.path.join(ROOT, 'build_quest', 'debug.keystore')
    if not os.path.exists(ks):
        keytool = os.path.join(os.path.dirname(tools['java']), 'keytool' + ('.exe' if os.name == 'nt' else ''))
        if not os.path.exists(keytool):
            props = subprocess.run([tools['java'], '-XshowSettings:properties', '-version'], capture_output=True, text=True).stderr
            home = next((l.split('=', 1)[1].strip() for l in props.splitlines() if l.strip().startswith('java.home')), '')
            keytool = os.path.join(home, 'bin', os.path.basename(keytool))
        run([keytool, '-genkeypair', '-keystore', ks, '-storepass', 'android', '-keypass', 'android',
             '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
             '-dname', 'CN=Android Debug,O=Android,C=US'])
    return ks


def quest_dex(tools, out):
    """src/quest/java (one small activity) compiled into classes.dex: javac, then the build-tools' d8."""
    srcs = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, 'src', 'quest', 'java')) for f in fs if f.endswith('.java')]
    dex = os.path.join(out, 'classes.dex')
    if os.path.exists(dex) and os.path.getmtime(dex) > max(os.path.getmtime(f) for f in srcs): return dex
    javac = (os.path.join(os.environ['JAVA_HOME'], 'bin', 'javac' + ('.exe' if os.name == 'nt' else '')) if os.environ.get('JAVA_HOME')
             else shutil.which('javac') or os.path.join(os.path.dirname(tools['java']), 'javac'))
    cls = os.path.join(out, 'classes')
    shutil.rmtree(cls, ignore_errors=True)
    os.makedirs(cls)
    run([javac, '-nowarn', '-Xlint:-options', '-source', '8', '-target', '8', '-classpath', tools['android_jar'], '-d', cls] + srcs)
    classes = [os.path.join(dp, f) for dp, _, fs in os.walk(cls) for f in fs if f.endswith('.class')]
    run([tools['java'], '-cp', tools['d8'], 'com.android.tools.r8.D8', '--lib', tools['android_jar'], '--min-api', str(QUEST_API),
         '--output', out] + classes)
    return dex


def package_apk(tools, out, so, xr_lib, pkg, label):
    man = os.path.join(out, 'AndroidManifest.xml')
    text = open(os.path.join(ROOT, 'src', 'quest', 'AndroidManifest.xml'), encoding='utf-8').read()
    open(man, 'w', encoding='utf-8').write(text.replace('@PACKAGE@', pkg).replace('@LABEL@', label))
    unaligned, aligned = os.path.join(out, 'unaligned.apk'), os.path.join(out, 'aligned.apk')
    dex = quest_dex(tools, out)
    run([tools['aapt2'], 'link', '-o', unaligned, '--manifest', man, '-I', tools['android_jar'],
         '--min-sdk-version', str(QUEST_API), '--target-sdk-version', '32'])
    stripped = os.path.join(out, 'libhotdvr.stripped.so')
    run([tools['strip'], '--strip-unneeded', '-o', stripped, so])
    with zipfile.ZipFile(unaligned, 'a', zipfile.ZIP_DEFLATED) as z:
        z.write(stripped, 'lib/arm64-v8a/libhotdvr.so')
        z.write(os.path.join(xr_lib, 'libopenxr_loader.so'), 'lib/arm64-v8a/libopenxr_loader.so')
        z.write(dex, 'classes.dex')                                  # RestartActivity (online play restarts the app)
        z.writestr('assets/build-info.txt', build_info(text, so))   # for crash reports (quest_report.c)
        # the gun model (CC0) and the hands (CC BY 4.0; see their .json) and anything else in src/quest/assets
        adir = os.path.join(ROOT, 'src', 'quest', 'assets')
        for n in sorted(os.listdir(adir)) if os.path.isdir(adir) else []:
            if not n.endswith('.json'): z.write(os.path.join(adir, n), 'assets/' + n)
    run([tools['zipalign'], '-p', '-f', '4', unaligned, aligned])
    apk = os.path.join(out, pkg.rsplit('.', 1)[-1] + '.apk')
    run([tools['java'], '-jar', tools['apksigner'], 'sign', '--ks', quest_keystore(tools), '--ks-pass', 'pass:android',
         '--key-pass', 'pass:android', '--ks-key-alias', 'androiddebugkey', '--out', apk, aligned])
    log('APK: %s (%.1f MB)' % (apk, os.path.getsize(apk) / 1e6))
    return apk


def elf_build_id(path):
    """The GNU build id of an ELF64 little-endian file (hex), or ''."""
    import struct
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2: return ''
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    for i in range(phnum):
        ptype, _, off, _, _, filesz = struct.unpack_from('<IIQQQQ', d, phoff + i * phentsize)
        if ptype != 4: continue                    # PT_NOTE
        p, end = off, off + filesz
        while p + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from('<III', d, p)
            name, desc = p + 12, p + 12 + ((namesz + 3) & ~3)
            if ntype == 3 and d[name:name + namesz] == b'GNU\0': return d[desc:desc + descsz].hex()
            p = desc + ((descsz + 3) & ~3)
    return ''


def build_info(manifest, so):
    """assets/build-info.txt: what a crash report says about the build (no paths, no names)."""
    ver = manifest.split('android:versionName="', 1)[1].split('"', 1)[0] if 'android:versionName="' in manifest else ''
    try:
        commit = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        if commit and subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ROOT,
                                     capture_output=True, text=True).stdout.strip(): commit += '-dirty'
    except OSError:
        commit = ''
    return 'version=%s\ncommit=%s\nbuilt=%s\nlib_build_id=%s\n' % (
        ver, commit, time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), elf_build_id(so))


def keep_symbols(so):
    """build_quest/symbols/<build id>/libhotdvr.so: the unstripped library of every build, for crash reports
    (python scripts/quest_tools.py symbols uploads them)."""
    bid = elf_build_id(so)
    if not bid: return
    root = os.path.join(ROOT, 'build_quest', 'symbols')
    d = os.path.join(root, bid)
    if not os.path.exists(os.path.join(d, 'libhotdvr.so')):
        os.makedirs(d, exist_ok=True)
        shutil.copy2(so, os.path.join(d, 'libhotdvr.so'))
    old = sorted((os.path.getmtime(os.path.join(root, n)), n) for n in os.listdir(root))
    for _, n in old[:-SYMBOLS_KEEP]: shutil.rmtree(os.path.join(root, n), ignore_errors=True)
    log('build id %s (symbols kept in build_quest/symbols; python scripts/quest_tools.py symbols uploads them)' % bid)


def quest_symbols():
    """Upload build_quest/symbols/* as gzipped assets libhotdvr-<build id>.so.gz of the release SYMBOLS_TAG in the
    private CRASH_REPO (the gh CLI, logged in), where the crash triage workflow finds them."""
    import gzip
    gh = shutil.which('gh')
    if not gh: sys.exit('the GitHub CLI (gh) is needed: https://cli.github.com, then gh auth login')
    root = os.path.join(ROOT, 'build_quest', 'symbols')
    ids = sorted(n for n in os.listdir(root) if os.path.exists(os.path.join(root, n, 'libhotdvr.so'))) if os.path.isdir(root) else []
    if not ids: sys.exit('no symbols yet: build with python scripts/build_quest.py first')
    vis = subprocess.run([gh, 'repo', 'view', CRASH_REPO, '--json', 'visibility', '-q', '.visibility'],
                         capture_output=True, text=True).stdout.strip()
    if vis != 'PRIVATE':
        sys.exit("%s is not a private repository you can reach (%r): the symbols are made from the game's code and "
                 "must stay private" % (CRASH_REPO, vis))
    r = subprocess.run([gh, 'release', 'view', SYMBOLS_TAG, '--repo', CRASH_REPO, '--json', 'assets', '-q', '.assets[].name'],
                       capture_output=True, text=True)
    if r.returncode:
        run([gh, 'release', 'create', SYMBOLS_TAG, '--repo', CRASH_REPO, '--prerelease', '--title', 'Crash symbols',
             '--notes', 'Unstripped libhotdvr.so of each Quest build, named by GNU build id, for symbolizing crash '
                        "reports (python scripts/quest_tools.py symbols). Private: made from the game's code."])
        have = set()
    else:
        have = set(r.stdout.split())
    out = os.path.join(ROOT, 'build_quest', 'symbols-upload')
    os.makedirs(out, exist_ok=True)
    todo = [b for b in ids if 'libhotdvr-%s.so.gz' % b not in have]
    for b in todo:
        gz = os.path.join(out, 'libhotdvr-%s.so.gz' % b)
        with open(os.path.join(root, b, 'libhotdvr.so'), 'rb') as f, gzip.open(gz, 'wb', 9) as g: shutil.copyfileobj(f, g)
        run([gh, 'release', 'upload', SYMBOLS_TAG, gz, '--repo', CRASH_REPO, '--clobber'])
        os.remove(gz)
    log('symbols: %d uploaded, %d already there (%s, release %s)' % (len(todo), len(ids) - len(todo), CRASH_REPO, SYMBOLS_TAG))


def quest_deploy(tools, apk, pkg, launch=True):
    """Install over USB (adb) and start it."""
    adb = tools['adb']
    if not adb: return log('adb not found: install %s yourself (SideQuest, or adb install)' % apk)
    lines = subprocess.check_output([adb, 'devices']).decode(errors='replace').splitlines()[1:]
    devs = [l.split()[0] for l in lines if l.strip().endswith('device')]
    if not devs:
        return log('no headset over USB (developer mode on, USB debugging allowed in the headset?): install %s later' % apk)
    if len(devs) > 1 and not os.environ.get('ANDROID_SERIAL'):
        return log('several Android devices (%s): set ANDROID_SERIAL to the headset\'s' %', '.join(devs))
    run([adb, 'install', '-r', apk])
    if not launch: return log('installed; start it from Library > Unknown Sources')
    run([adb, 'shell', 'am', 'start', '-n', pkg + '/android.app.NativeActivity'])
    log('started on the headset. Log: adb logcat -s hotdvr hotdarcade')


if __name__ == '__main__':
    if sys.argv[1:] == ['symbols']: quest_symbols()
    else: sys.exit(__doc__)
