#!/usr/bin/env python3
"""Crash triage, the digest (.github/workflows/crash-triage.yml, in the private crash repository): one crash issue (or
one occurrence comment on it) in, one deterministic digest out, as GoldenEye VR's reports digest. No AI: the issue and
its digest are written to be read on their own (by a person, or Copilot Chat on github.com).

For the report's platform and build id (the relay's issue: tools/vr/crash-relay/src/relay.js):
  - the build's symbols from the release `crash-symbols` (scripts/quest_tools.py symbols uploads them): Android's
    unstripped libhotdvr-<id>.so, Windows' HOTDVR-<id>.exe and .pdb, Apple's <executable>-<uuid>.dwarf (a dSYM's);
  - every app frame symbolized with llvm-symbolizer (function, inlined calls, file:line; return addresses one byte
    back, at the call);
  - the disassembly at the faulting instruction (llvm-objdump; not from a dSYM, which holds no code);
  - each app frame's source lines at the report's commit (the public repository, checked out with its history), and
    whether that function has changed since (at the newest commit);
  - the end of port.log; the settings that differ from the defaults are left to the issue;
  - a signature label `sig-xxxxxxxx` from the symbolized function names and the kind of fault (the same in every
    build and on every platform that runs the same code), the other issues with it, and for a new issue whose label an
    older open crash issue already has, that issue as its duplicate (post.py closes it there).

Writes into --out: digest.md (the comment), labels.txt, meta.json (what post.py acts on), and for the optional AI
analysis context.md and report.md.

Offline (tests): --issue-json FILE (as `gh issue view --json number,title,body,labels,state`), --comment-json FILE,
--others-json FILE (a list like it), --symbols-dir DIR (the assets, unzipped, as named in the release). Only the
standard library; gh does the GitHub calls.
"""
import argparse, glob, gzip, hashlib, json, os, re, shutil, struct, subprocess, sys

SIG = re.compile(r'<!-- crash-sig: ([0-9a-f]{12}) -->')
SYMBOLS_TAG = 'crash-symbols'
CODE_REPO = 'MrSco/house-of-the-dead-vr'
FAMILY = {'quest': 'android', 'frame': 'android', 'android': 'android', 'windows': 'windows', 'pcvr': 'windows',
          'ios': 'apple', 'mac': 'apple', 'tvos': 'apple', 'visionos': 'apple'}
APP_BINARIES = {'android': ['libhotdvr.so'], 'windows': ['HOTDVR.exe'], 'apple': ['HouseOfTheDeadVR', 'House_of_the_Dead_VR']}
# a fault, the same word on every platform (for the signature): Linux / Apple signals and Windows exceptions
FAULT_KIND = {'SIGSEGV': 'bad-access', 'SIGBUS': 'bad-access', 'access violation': 'bad-access', 'SIGILL': 'illegal-instruction',
              'illegal instruction': 'illegal-instruction', 'SIGFPE': 'arithmetic', 'divide by zero': 'arithmetic',
              'SIGABRT': 'abort', 'fast fail (stack buffer)': 'abort', 'SIGTRAP': 'trap', 'stack overflow': 'stack-overflow'}
LINUX_SIGNALS = {4: 'SIGILL', 5: 'SIGTRAP', 6: 'SIGABRT', 7: 'SIGBUS', 8: 'SIGFPE', 11: 'SIGSEGV'}
APPLE_SIGNALS = {**LINUX_SIGNALS, 7: 'SIGEMT', 10: 'SIGBUS'}


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def run(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace', timeout=300).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ''


def find_tool(name):
    for n in [name] + ['%s-%d' % (name, v) for v in range(22, 13, -1)]:
        p = shutil.which(n)
        if p: return p
    c = sorted(glob.glob('/usr/lib/llvm-*/bin/' + name), reverse=True)
    for env in ('ANDROID_NDK_LATEST_HOME', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT'):
        if os.environ.get(env): c += glob.glob(os.path.join(os.environ[env], 'toolchains/llvm/prebuilt/*/bin/' + name + '*'))
    return c[0] if c else None


def fence(text, lang=''):
    longest = max([2] + [len(m) for m in re.findall(r'`+', text)])
    f = '`' * (longest + 1)
    return '%s%s\n%s\n%s' % (f, lang, text.rstrip('\n'), f)


def safe(s):
    """report text (untrusted) on one line, outside code: no markers, no mentions, no markdown breakouts"""
    return re.sub(r'[`<>|@\r\n]', ' ', str(s))[:200]


# ------------------------------------------------------------------ the issue as the relay filed it
def fenced_blocks(text):
    return [m.group(2) for m in re.finditer(r'^(`{3,})[a-z]*\n(.*?)\n\1\s*$', text, re.S | re.M)]


def crash_text(body):
    """the crash block of an issue body (after '### Crash') or of an occurrence comment (its first block)"""
    i = body.find('### Crash')
    b = fenced_blocks(body[i:] if i >= 0 else body)
    return b[0] if b else ''


def log_text(body):
    i = body.find('<summary>port.log')
    b = fenced_blocks(body[i:]) if i >= 0 else []
    return b[0] if b else ''


def report_facts(issue, comment):
    """platform, build id, version, commit of the report (the comment's for an occurrence)"""
    body = issue.get('body') or ''
    labels = [l['name'] if isinstance(l, dict) else l for l in issue.get('labels', [])]
    plat = next((l.split(':', 1)[1] for l in labels if l.startswith('platform:') and l.split(':', 1)[1] in FAMILY), None)
    m = re.search(r'\(`platform:(\w+)`\)', body)
    if m and m.group(1) in FAMILY: plat = m.group(1)
    text = comment or body
    if comment:
        m = re.search(r'build `([0-9a-fA-F]*|unknown)` \(`([^`]*)` / `([^`]*)`\)', comment)
        bid, version, commit = (m.group(1), m.group(2), m.group(3)) if m else ('', '', '')
        names = {v['name']: k for k, v in PLATFORM_NAMES.items()}
        m = re.search(r'`, ([^,]+), ', comment)
        if m and m.group(1) in names: plat = names[m.group(1)]
    else:
        m = re.search(r'^\| Build id \| `([0-9a-fA-F]*|unknown)`', body, re.M)
        bid = m.group(1) if m else ''
        m = re.search(r'^\| Version / commit \| `([^`]*)` / `([^`]*)`', body, re.M)
        version, commit = (m.group(1), m.group(2)) if m else ('', '')
    crash = crash_text(text)
    if not bid or bid == 'unknown':
        m = re.search(r'^\s*build-id ([0-9a-fA-F]+)', crash, re.M)
        bid = m.group(1) if m else ''
    return dict(platform=plat or 'quest', build_id=bid, version=version, commit=commit, crash=crash, log=log_text(text))


PLATFORM_NAMES = {'quest': {'name': 'Meta Quest'}, 'frame': {'name': 'Steam Frame'}, 'android': {'name': 'Android phone / tablet'},
                  'windows': {'name': 'Windows'}, 'pcvr': {'name': 'Windows PC VR'}, 'ios': {'name': 'iPhone / iPad'},
                  'mac': {'name': 'Mac'}, 'tvos': {'name': 'Apple TV'}, 'visionos': {'name': 'Apple Vision Pro'}}


def parse_crash(text, family):
    """the fault and the frames, as the relay's parseCrash"""
    head = next((l for l in text.splitlines() if l.startswith('crash: ')), '')
    m = re.match(r'crash: signal (\d+) code (-?\d+) addr (\S+)', head)
    w = None if m else re.match(r'crash: ([a-z][a-z ()]*?) \((0x[0-9A-Fa-f]{8})\)(?:, (reading|writing|running) (0x[0-9a-f]+))?', head)
    if m:
        name = (APPLE_SIGNALS if family == 'apple' else LINUX_SIGNALS).get(int(m.group(1)), 'signal ' + m.group(1))
        fault = dict(name=name, kind=FAULT_KIND.get(name, name), detail='%s code %s at %s' % (name, m.group(2), m.group(3)))
    elif w:
        fault = dict(name=w.group(1), kind=FAULT_KIND.get(w.group(1), w.group(1)),
                     detail='%s %s%s' % (w.group(1), w.group(2), (', %s %s' % (w.group(3), w.group(4))) if w.group(3) else ''))
    else:
        fault = dict(name='?', kind='?', detail=head[:120])
    frames = []
    for line in text.splitlines():
        f = re.match(r'^\s+(?:#\d+\s+)?(pc|lr|at)\s+(\S+?)\+0x([0-9a-f]+)(?:\s+\((.*)\))?\s*$', line)
        if f: frames.append(dict(kind=f.group(1), lib=f.group(2), offset=int(f.group(3), 16), symbol='' if f.group(4) in (None, '?') else f.group(4)))
    # Windows: the first frame is the fault, the rest are return addresses (StackWalk64 numbers them all "pc")
    if family == 'windows':
        for i, f in enumerate(frames): f['kind'] = 'pc' if i == 0 else 'at'
    return fault, frames


# ------------------------------------------------------------------ symbols
def fetch_symbols(repo, family, bid, out, local=None):
    """the build's symbol file (and Windows' PDB beside it) in out, or None"""
    pats = {'android': ['libhotdvr-%s.so.gz' % bid], 'windows': ['HOTDVR-%s.exe.gz' % bid, 'HOTDVR-%s.pdb.gz' % bid],
            'apple': ['*-%s.dwarf.gz' % bid.lower()]}[family]
    d = os.path.join(out, 'symbols')
    os.makedirs(d, exist_ok=True)
    for p in pats:
        if local:
            for f in glob.glob(os.path.join(local, p)): shutil.copy2(f, d)
        else:
            try:
                gh('release', 'download', SYMBOLS_TAG, '--repo', repo, '--pattern', p, '--dir', d, '--clobber')
            except subprocess.CalledProcessError as e:
                sys.stderr.write('no symbols %s: %s\n' % (p, e.stderr.strip()[:200]))
    got = []
    for gz in glob.glob(os.path.join(d, '*.gz')):
        name = os.path.basename(gz)[:-3]
        if family == 'windows': name = 'HOTDVR.' + name.rsplit('.', 1)[1]   # (exe and pdb side by side, as linked)
        with gzip.open(gz, 'rb') as f, open(os.path.join(d, name), 'wb') as g: shutil.copyfileobj(f, g)
        os.remove(gz)
        got.append(os.path.join(d, name))
    main = [g for g in got if not g.endswith('.pdb')]
    if family == 'windows' and not any(g.endswith('.pdb') for g in got): return None
    return main[0] if main else None


def image_base(path, family):
    """the address the file's offset 0 maps to: ELF 0, a PE's ImageBase, a Mach-O's __TEXT vmaddr"""
    d = open(path, 'rb').read(1 << 16)
    if family == 'windows' and d[:2] == b'MZ':
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        magic = struct.unpack_from('<H', d, pe + 24)[0]
        return struct.unpack_from('<Q', d, pe + 24 + 24)[0] if magic == 0x20b else struct.unpack_from('<I', d, pe + 24 + 28)[0]
    if family == 'apple' and d[:4] == b'\xcf\xfa\xed\xfe':
        ncmds = struct.unpack_from('<I', d, 16)[0]
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', d, off)
            if cmd == 0x19 and d[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
                return struct.unpack_from('<Q', d, off + 24)[0]
            off += size
    return 0


def repo_path(p):
    """a build's source path as one in the repository (src/..., gen/... for the recompiled game). Builds since 0.6.1
    give them relative to the checkout already (./src/...: cmake/prefix_map.cmake), the home folder's as ~/...; older
    builds' absolute ones lose the build machine's folders (the Mac's checkouts were ~/src/<folder>/)."""
    p = p.replace('\\', '/')
    # (a relative name after its compilation folder: build_houseofthedead/android/hotd/./src/..., .../hotd/~/...)
    m = re.search(r'^.*/(\./|~/)', p)
    if m: p = p[m.start(1):]
    rel = not re.match(r'^(?:[A-Za-z]:)?/|^~', p)
    while p.startswith('./'): p = p[2:]
    m = re.search(r'(?:^|/)build_houseofthedead/recomp/[^/]+/(gen/.*)$', p)
    if m: return m.group(1)
    if rel: return p
    p = re.sub(r'^/(?:Users|home)/[^/]+/src/[^/]+/', '', p)
    m = re.search(r'(?:^|/)((?:src|gen|build_quest|third_party|extern|tools)/.*)$', p)
    return m.group(1) if m else p


def symbolize(path, family, addrs):
    """{address: [(function, file, line)] innermost first}"""
    tool = find_tool('llvm-symbolizer')
    if not tool or not path or not addrs: return {}
    out = run([tool, '--obj=' + path, '--inlining', '--functions=linkage', '--demangle', '--output-style=LLVM']
              + ['0x%x' % a for a in addrs])
    res = {}
    for a, block in zip(addrs, out.strip().split('\n\n')):
        lines = block.strip().splitlines()
        got = []
        for i in range(0, len(lines) - 1, 2):
            fn, loc = lines[i].strip(), lines[i + 1].strip()
            m = re.match(r'^(.*?):(\d+)(?::\d+)?$', loc)
            f, ln = (repo_path(m.group(1)), int(m.group(2))) if m else ('?', 0)
            got.append((fn, f, ln))
        if got and not (len(got) == 1 and got[0][0] == '??' and got[0][1] in ('?', '??')): res[a] = got
    return res


def disassemble(path, family, addr):
    tool = find_tool('llvm-objdump')
    if not tool or not path or path.endswith('.dwarf') and dsym_has_no_code(path): return ''
    out = run([tool, '-d', '--no-show-raw-insn', '--demangle', '--start-address=0x%x' % max(addr - 0x40, 0),
               '--stop-address=0x%x' % (addr + 0x14), path])
    lines = []
    for l in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s*(.*)$', l)
        if m:
            lines.append(('=> ' if int(m.group(1), 16) == addr else '   ') + l.strip())
    # (a dSYM's sections hold no code: llvm-objdump prints <unknown>)
    real = [l for l in lines if '<unknown>' not in l]
    return '\n'.join(lines) if any(l.startswith('=>') for l in real) and len(real) > len(lines) // 2 else ''


def dsym_has_no_code(path):
    """a dSYM's DWARF file keeps the load commands but not the code (its __text has no file contents)"""
    d = open(path, 'rb').read(1 << 16)
    if d[:4] != b'\xcf\xfa\xed\xfe': return False
    ncmds, off = struct.unpack_from('<I', d, 16)[0], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19 and d[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            return struct.unpack_from('<Q', d, off + 48)[0] == 0   # fileoff... filesize: 0 in a dSYM
        off += size
    return False


# ------------------------------------------------------------------ the source at the report's commit
def resolve_ref(code, commit, version):
    c = (commit or '').replace('-dirty', '')
    if c and re.fullmatch(r'[0-9a-f]{7,40}', c) and run(['git', 'cat-file', '-t', c + '^{commit}'], cwd=code).strip() == 'commit':
        return c, commit.endswith('-dirty')
    tag = 'v' + version.lstrip('v') if version else ''
    if tag and run(['git', 'rev-parse', '--verify', '-q', tag + '^{commit}'], cwd=code).strip():
        return tag, False
    return None, False


def show(code, ref, path):
    return run(['git', 'show', '%s:%s' % (ref, path)], cwd=code)


def function_at(text, name, line):
    """the lines of the C function `name` around `line` (1-based) in text: from its definition to its closing brace"""
    lines = text.splitlines()
    base = re.sub(r'\(.*$', '', name).split('::')[-1].strip()
    if not re.fullmatch(r'[A-Za-z_]\w*', base): return None
    start = None
    for i in range(min(line, len(lines)) - 1, -1, -1):
        if re.match(r'^[A-Za-z_].*\b%s\s*\(' % re.escape(base), lines[i]) and not lines[i].rstrip().endswith(';'):
            start = i
            break
    if start is None: return None
    depth, opened = 0, False
    for j in range(start, min(len(lines), start + 2000)):
        depth += lines[j].count('{') - lines[j].count('}')
        opened |= '{' in lines[j]
        if opened and depth <= 0: return start, lines[start:j + 1]
    return start, lines[start:]


def find_function(text, name):
    base = re.sub(r'\(.*$', '', name).split('::')[-1].strip()
    if not re.fullmatch(r'[A-Za-z_]\w*', base): return None
    for i, l in enumerate(text.splitlines()):
        if re.match(r'^[A-Za-z_].*\b%s\s*\(' % re.escape(base), l) and not l.rstrip().endswith(';'):
            return function_at(text, name, i + 1)
    return None


# ------------------------------------------------------------------ the digest
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    ap.add_argument('--issue', type=int)
    ap.add_argument('--comment-id', default='')
    ap.add_argument('--code', default='code', help='the public repository, checked out with its history')
    ap.add_argument('--out', required=True)
    ap.add_argument('--issue-json'); ap.add_argument('--comment-json'); ap.add_argument('--others-json'); ap.add_argument('--symbols-dir')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    issue = json.load(open(a.issue_json, encoding='utf-8')) if a.issue_json else \
        json.loads(gh('issue', 'view', str(a.issue), '--repo', a.repo, '--json', 'number,title,body,labels,state'))
    comment = ''
    if a.comment_json: comment = json.load(open(a.comment_json, encoding='utf-8'))['body']
    elif a.comment_id: comment = json.loads(gh('api', 'repos/%s/issues/comments/%s' % (a.repo, a.comment_id)))['body']
    n = issue['number']
    mode = 'retriage' if comment else 'new'
    r = report_facts(issue, comment)
    family = FAMILY[r['platform']]
    fault, frames = parse_crash(r['crash'], family)
    app = APP_BINARIES[family]
    bid = r['build_id']
    sym = fetch_symbols(a.repo, family, bid, a.out, a.symbols_dir) if bid else None
    base = image_base(sym, family) if sym else 0
    addr = {}
    for i, f in enumerate(frames):
        if f['lib'] in app: addr[i] = base + f['offset'] - (0 if f['kind'] == 'pc' else 1)
    syms = symbolize(sym, family, sorted(set(addr.values()))) if sym else {}
    code = a.code if os.path.exists(os.path.join(a.code, '.git')) else None
    ref, dirty = resolve_ref(code, r['commit'], r['version']) if code else (None, False)
    head = run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=code).strip() if code else ''

    md = ['<!-- crash-digest -->', '## Crash digest%s' % (' (occurrence on a new build)' if comment else '')]
    md.append('%s, build `%s`, version `%s`, commit `%s`. Fault: **%s**.' % (
        PLATFORM_NAMES[r['platform']]['name'], safe(bid or '?'), safe(r['version'] or '?'), safe(r['commit'] or '?'), safe(fault['detail'])))
    if not bid: md.append('\nNo build id in the report: no symbols to look up.')
    elif not sym: md.append('\n**No symbols for this build** in the release `%s` (`python scripts/quest_tools.py symbols%s` on the '
                            'build machine uploads them); the frames below are as the app wrote them.' % (SYMBOLS_TAG, ' --apple' if family == 'apple' else ''))
    elif not find_tool('llvm-symbolizer'): md.append('\nSymbols found, but no llvm-symbolizer on the runner.')
    md.append('')

    # the frames
    rows = ['| # | frame | function | source |', '|---|---|---|---|']
    key = []
    for i, f in enumerate(frames):
        where = '%s %s+0x%x' % (f['kind'], f['lib'], f['offset'])
        got = syms.get(addr.get(i)) if i in addr else None
        if got:
            fn = ' <- '.join('`%s`' % safe(g[0]) for g in got)
            src = ' <- '.join(('[`%s:%d`](https://github.com/%s/blob/%s/%s#L%d)' % (g[1], g[2], CODE_REPO, ref, g[1], g[2])
                               if ref and g[1].startswith('src/') else '`%s:%d`' % (safe(g[1]), g[2])) for g in got)
            key.append(got[0][0])
        else:
            fn = '`%s`%s' % (safe(f['symbol']), '' if f['lib'] in app else ' (system)') if f['symbol'] else ('?' if f['lib'] in app else '(system)')
            src = ''
            if f['lib'] in app and f['symbol'] and family == 'android': key.append(f['symbol'])
        rows.append('| %d | %s | %s | %s |' % (i, where, fn, src))
    md += ['### Frames', '', '_Innermost first; return addresses looked up one byte back, at the call. `src/` is the port '
           '(the public repository), `gen/` the recompiled game (from the player\'s own game files: in no repository)._', '']
    md += rows if frames else ['(no frames in the report)']
    md.append('')

    # the disassembly
    top = next((i for i in sorted(addr) if frames[i]['kind'] == 'pc'), None)
    if sym and top is not None:
        dis = disassemble(sym, family, addr[top])
        if dis: md += ['### Disassembly at frame %d (`=>` is the faulting instruction)' % top, '', fence(dis, 'asm'), '']
        elif family == 'apple': md += ['_No disassembly: a dSYM holds no code (the faulting instruction is in the app\'s binary)._', '']

    # the source at the report's commit, and whether it changed since
    if code and ref and syms:
        md.append('### Source at `%s`%s, compared with `%s` (newest)' % (ref, ' (built with uncommitted changes)' if dirty else '', head))
        md.append('')
        seen, shown = set(), False
        for i in sorted(addr):
            for fn, path, line in syms.get(addr[i], [])[:3]:
                if (fn, path) in seen or not path.startswith('src/'): continue
                seen.add((fn, path))
                then = show(code, ref, path)
                if not then:
                    md.append('- `%s` at `%s:%d`: not in `%s`' % (safe(fn), path, line, ref))
                    continue
                now = show(code, 'HEAD', path)
                a_fn = function_at(then, fn, line)
                b_fn = find_function(now, fn) if now else None
                moved = ''
                if not b_fn:   # (moved to another file since? the definition, anywhere in the code now)
                    base = re.sub(r'\(.*$', '', fn).split('::')[-1].strip()
                    hit = run(['git', 'grep', '-n', '-E', r'^[A-Za-z_].*\b%s\s*\(' % re.escape(base), 'HEAD', '--',
                               '*.c', '*.m', '*.cpp'], cwd=code) if re.fullmatch(r'[A-Za-z_]\w*', base) else ''
                    for h in hit.splitlines():
                        parts = h.split(':', 3)
                        if len(parts) == 4 and not parts[3].rstrip().endswith(';'):
                            now, moved = show(code, 'HEAD', parts[1]), parts[1]
                            b_fn = find_function(now, fn)
                            break
                state = ('moved to `%s`, ' % moved if moved else '') + (
                    'gone' if not now else 'unchanged' if then == now else
                    'function unchanged' if a_fn and b_fn and a_fn[1] == b_fn[1] else 'function changed' if a_fn and b_fn else 'file changed')
                md.append('- `%s` at [%s:%d](https://github.com/%s/blob/%s/%s#L%d): **%s** since' % (safe(fn), path, line, CODE_REPO, ref, path, line, state))
                if not shown:
                    lines = then.splitlines()
                    lo, hi = max(1, line - 12), min(len(lines), line + 6)
                    snippet = '\n'.join('%s%5d  %s' % ('=>' if k == line else '  ', k, lines[k - 1]) for k in range(lo, hi + 1))
                    md.append('\n<details open><summary>%s:%d at %s</summary>\n\n%s\n</details>\n' % (path, line, ref, fence(snippet, 'c')))
                    shown = True
        if not seen: md.append('(no frame in `src/`: the crash is in the recompiled game or a library)')
        md.append('')
    elif syms and not ref:
        md += ['_The report\'s commit (`%s`) is not in the repository: no source shown._' % safe(r['commit'] or '?'), '']

    # the log's end
    if r['log'].strip():
        tail = '\n'.join(r['log'].rstrip('\n').splitlines()[-40:])
        md += ['<details><summary>port.log, last 40 lines</summary>', '', fence(tail), '</details>', '']

    # the signature: the kind of fault and the top four app functions (symbolized, else Android's dynamic names; with
    # neither, the relay's own, so nothing is grouped by mere offsets)
    names = []
    for k in key:
        k = re.sub(r'\(.*$', '', k).strip()
        if not names or names[-1] != k: names.append(k)
    names = names[:4]
    labels, dup = [], None
    if names:
        sig = 'sig-' + hashlib.sha1(('%s|%s' % (fault['kind'], '|'.join(names))).encode()).hexdigest()[:8]
        labels.append(sig)
        md.append('Signature `%s`: %s in %s. Every issue with this label crashed the same way (any build, any platform).' % (
            sig, fault['kind'], ' > '.join('`%s`' % safe(x) for x in names)))
        others = json.load(open(a.others_json, encoding='utf-8')) if a.others_json else (
            json.loads(gh('issue', 'list', '--repo', a.repo, '--label', sig, '--state', 'all', '--limit', '100', '--json', 'number,state,title'))
            if a.repo else [])
        others = [o for o in others if o['number'] != n and sig in [l['name'] if isinstance(l, dict) else l for l in o.get('labels', [{'name': sig}])]]
        if others:
            md.append('Same signature: ' + ', '.join('#%d (%s)' % (o['number'], o['state'].lower()) for o in sorted(others, key=lambda o: o['number'])) + '.')
            older_open = [o['number'] for o in others if o['state'].upper() == 'OPEN' and o['number'] < n]
            if mode == 'new' and older_open: dup = min(older_open)
    else:
        md.append('_No signature label: no function names for this crash (no symbols)._')
    md.append('')
    md.append('<sub>Digest by tools/vr/crash-relay/triage/digest.py; rerun: `gh workflow run crash-triage.yml -f issue=%d`.</sub>' % n)

    digest = '\n'.join(md)[:64000]
    open(os.path.join(a.out, 'digest.md'), 'w', encoding='utf-8', newline='\n').write(digest)
    open(os.path.join(a.out, 'labels.txt'), 'w', encoding='utf-8', newline='\n').write(''.join(l + '\n' for l in labels))
    json.dump(dict(issue=n, mode=mode, platform=r['platform'], build_id=bid, symbols=bool(sym and syms), signatures=SIG.findall(issue.get('body') or ''),
                   labels=labels, duplicate_of=dup), open(os.path.join(a.out, 'meta.json'), 'w'), indent=1)
    # for the optional AI analysis: the digest and the report
    open(os.path.join(a.out, 'context.md'), 'w', encoding='utf-8').write(digest + '\n')
    rep = '# Issue #%d: %s\n\n%s\n' % (n, issue.get('title', ''), issue.get('body', ''))
    if comment: rep += '\n\n# The occurrence to look at (comment)\n\n' + comment + '\n'
    open(os.path.join(a.out, 'report.md'), 'w', encoding='utf-8').write(rep)
    print(digest)


if __name__ == '__main__':
    main()
