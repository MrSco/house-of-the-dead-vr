#!/usr/bin/env python3
"""Crash triage, step 1 (.github/workflows/crash-triage.yml): everything the analysis needs, in files.

For a `crash` issue (or one occurrence comment on it, --comment-id): the report's frames symbolized with the matching
unstripped libhotdvr.so (the private release `crash-symbols`, uploaded by python scripts/quest_tools.py symbols), the report
itself, and the other crash issues to compare with. Writes into --out:
    context.md     what to read first: the frames as function + file:line, the build, the other crash issues
    report.md      the issue body (and the occurrence comment) as filed by the relay
    meta.json      issue number, mode (new | retriage), signatures, build id, symbols found or not

Offline use (tests): --issue-json FILE (an issue as `gh issue view --json number,title,body,labels`), --others-json
FILE (a list like it), --symbols FILE (a libhotdvr.so). Only the standard library; gh does the GitHub calls.
"""
import argparse, glob, gzip, json, os, re, shutil, subprocess, sys

FRAME = re.compile(r'^\s+(pc|lr|at)\s+(\S+?)\+0x([0-9a-f]+)\s+\((.*)\)\s*$')
SIG = re.compile(r'<!-- crash-sig: ([0-9a-f]{12}) -->')
APP_LIB = 'libhotdvr.so'
SYMBOLS_TAG = 'crash-symbols'


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def first_fence(text):
    """The first fenced code block in text (any fence length)."""
    m = re.search(r'^(`{3,})[a-z]*\n(.*?)\n\1\s*$', text, re.S | re.M)
    return m.group(2) if m else ''


def crash_text(body):
    """The crash block of an issue body (after '### Crash') or of an occurrence comment (its first block)."""
    i = body.find('### Crash')
    return first_fence(body[i:] if i >= 0 else body)


def parse(crash):
    frames = []
    for line in crash.splitlines():
        m = FRAME.match(line)
        if m: frames.append(dict(kind=m.group(1), lib=m.group(2), offset=int(m.group(3), 16), symbol=m.group(4)))
    bid = re.search(r'^\s+build-id ([0-9a-f]{40})', crash, re.M)
    head = re.search(r'^crash: signal (\d+) code (-?\d+) addr (\S+)', crash, re.M)
    return frames, bid.group(1) if bid else '', head.groups() if head else ('?', '?', '?')


def find_symbolizer():
    names = ['llvm-symbolizer'] + ['llvm-symbolizer-%d' % v for v in range(22, 13, -1)]
    for n in names:
        p = shutil.which(n)
        if p: return p
    cands = sorted(glob.glob('/usr/lib/llvm-*/bin/llvm-symbolizer'), reverse=True)
    for env in ('ANDROID_NDK_LATEST_HOME', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'ANDROID_NDK'):
        if os.environ.get(env):
            cands += glob.glob(os.path.join(os.environ[env], 'toolchains/llvm/prebuilt/*/bin/llvm-symbolizer*'))
    return cands[0] if cands else None


def repo_path(p):
    """A build machine's path as a path in the repository (src/..., gen/... stay; the rest goes)."""
    p = p.replace('\\', '/')
    m = re.search(r'(?:^|/)((?:src|gen|build_quest|thirdparty)/.*)$', p)
    return m.group(1) if m else os.path.basename(p)


def symbolize(so, frames):
    """Each app frame -> list of (function, file:line) innermost first; return addresses step back into the call."""
    tool = find_symbolizer()
    if not tool or not so: return {}, tool
    addrs = [f['offset'] - (0 if f['kind'] == 'pc' else 1) for f in frames if f['lib'] == APP_LIB]
    if not addrs: return {}, tool
    out = subprocess.run([tool, '--obj=' + so, '--inlining', '--functions=linkage', '--demangle'],
                         input='\n'.join('0x%x' % a for a in addrs) + '\n', capture_output=True, text=True).stdout
    res = {}
    for a, block in zip(addrs, out.strip().split('\n\n')):
        lines = block.strip().splitlines()
        res[a] = [(lines[i], repo_path(lines[i + 1]) if i + 1 < len(lines) else '?') for i in range(0, len(lines) - 1, 2)]
    return res, tool


def fetch_symbols(repo, bid, out):
    name = 'libhotdvr-%s.so.gz' % bid
    try:
        gh('release', 'download', SYMBOLS_TAG, '--repo', repo, '--pattern', name, '--dir', out, '--clobber')
    except subprocess.CalledProcessError as e:
        sys.stderr.write('no symbols for %s: %s\n' % (bid, e.stderr.strip()))
        return None
    so = os.path.join(out, 'libhotdvr-%s.so' % bid)
    with gzip.open(os.path.join(out, name), 'rb') as f, open(so, 'wb') as g: shutil.copyfileobj(f, g)
    os.remove(os.path.join(out, name))
    return so


def frames_md(frames, syms):
    rows = ['| # | frame | function | source |', '|---|---|---|---|']
    for i, f in enumerate(frames):
        if f['lib'] == APP_LIB:
            a = f['offset'] - (0 if f['kind'] == 'pc' else 1)
            got = syms.get(a) or [(f['symbol'] + ' (dynamic symbol only)', '?')]
            fn = ' <- '.join('`%s`' % s for s, _ in got)
            src = ' <- '.join('`%s`' % l for _, l in got)
        else:
            fn, src = '`%s` (system library)' % f['symbol'], ''
        rows.append('| %d | %s %s+0x%x | %s | %s |' % (i, f['kind'], f['lib'], f['offset'], fn, src))
    return '\n'.join(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    ap.add_argument('--issue', type=int)
    ap.add_argument('--comment-id', default='')
    ap.add_argument('--out', required=True)
    ap.add_argument('--issue-json'); ap.add_argument('--others-json'); ap.add_argument('--symbols')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.issue_json:
        issue = json.load(open(a.issue_json, encoding='utf-8'))
    else:
        issue = json.loads(gh('issue', 'view', str(a.issue), '--repo', a.repo, '--json', 'number,title,body,labels,state'))
    number = issue['number']
    comment = ''
    if a.comment_id:
        comment = json.loads(gh('api', 'repos/%s/issues/comments/%s' % (a.repo, a.comment_id)))['body']
    report = comment or issue['body']
    crash = crash_text(report)
    frames, bid, (signal, code, addr) = parse(crash)
    if not bid:
        m = re.search(r'<!-- crash-builds: ([0-9a-f]{40})', issue['body'])
        bid = m.group(1) if m else ''
    so = a.symbols or (fetch_symbols(a.repo, bid, a.out) if bid else None)
    syms, tool = symbolize(so, frames)

    if a.others_json:
        others = json.load(open(a.others_json, encoding='utf-8'))
    else:
        others = json.loads(gh('issue', 'list', '--repo', a.repo, '--label', 'crash', '--state', 'all', '--limit', '100',
                               '--json', 'number,title,state,body'))
    other_rows = []
    for o in others:
        if o['number'] == number: continue
        ofr, _, _ = parse(crash_text(o.get('body') or ''))
        top = ' > '.join(f['symbol'] for f in ofr if f['lib'] == APP_LIB)[:200] or '(no app frames)'
        other_rows.append('- #%d [%s] %s; signatures %s; app frames: %s' % (
            o['number'], o['state'].lower(), o['title'], ', '.join(SIG.findall(o.get('body') or '')) or '-', top))

    sigs = SIG.findall(issue['body'])
    mode = 'retriage' if comment else 'new'
    ctx = [
        '# Crash triage context for issue #%d (%s)' % (number, mode),
        '',
        'Issue title: %s' % issue['title'],
        'Signal %s, code %s, fault address %s. Build id `%s`. Signature(s): %s.' % (signal, code, addr, bid or '?', ', '.join(sigs) or '?'),
        ('Symbols: found (%s, symbolized with %s).' % (os.path.basename(so), os.path.basename(tool))) if so and syms else
        ('Symbols: NOT available for this build id (only the dynamic symbol names in the report; run '
         '`python scripts/quest_tools.py symbols` on the build machine).' if not so else 'Symbols: found, but no symbolizer ran (%s).' % tool),
        '',
        '## Frames (innermost first; return addresses looked up one byte back, at the call)',
        '',
        frames_md(frames, syms) if frames else '(no frames in the report)',
        '',
        '`gen/` files are the recompiled game (generated from the user\'s own game files, not in the repository); '
        '`src/` is the port in this repository.',
        '',
        '## Other crash issues (for duplicates)',
        '',
        '\n'.join(other_rows) or '(none)',
        '',
        'The full report (crash text, settings, the end of port.log) is in report.md next to this file.',
    ]
    open(os.path.join(a.out, 'context.md'), 'w', encoding='utf-8').write('\n'.join(ctx) + '\n')
    rep = '# Issue #%d: %s\n\n%s\n' % (number, issue['title'], issue['body'])
    if comment: rep += '\n\n# The occurrence to look at (comment)\n\n' + comment + '\n'
    open(os.path.join(a.out, 'report.md'), 'w', encoding='utf-8').write(rep)
    json.dump(dict(issue=number, mode=mode, signatures=sigs, build_id=bid, symbols=bool(so and syms)),
              open(os.path.join(a.out, 'meta.json'), 'w'), indent=1)
    print('\n'.join(ctx))


if __name__ == '__main__':
    main()
