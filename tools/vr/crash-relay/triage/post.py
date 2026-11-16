#!/usr/bin/env python3
"""Crash triage, step 3 (.github/workflows/crash-triage.yml): post what the analysis wrote, and act on a duplicate.

The analysis (step 2, Claude) only writes files and has no GitHub access; this script, run from a copy made before
step 2, is what touches the issues:
    <dir>/analysis.md    posted as a comment on the issue
    <dir>/result.json    {"duplicate_of": N or null, "confidence": "high|medium|low"}
A new issue judged a duplicate of another crash issue (high confidence only) gets "Duplicate of #N", is closed as a
duplicate, and its signatures are added to #N's body so the relay counts later reports of it there.

--dry-run prints the GitHub calls instead of making them (tests).
"""
import argparse, json, os, re, subprocess, sys

SIG = re.compile(r'<!-- crash-sig: ([0-9a-f]{12}) -->')
DRY = {'on': False}


def gh(*args, input=None):
    if DRY['on']:
        print('gh ' + ' '.join(args) + ('  <<< ' + input[:300].replace('\n', '\\n') if input else ''))
        return '{}'
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True, input=input).stdout


def clean(md):
    """The analysis as a comment: no hidden markers of ours (only the relay and this script write those)."""
    md = re.sub(r'<!--.*?-->', '', md, flags=re.S)
    return md.strip()[:60000]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    ap.add_argument('--dir', required=True)
    ap.add_argument('--meta', help='meta.json from prep.py (default: in --dir)')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--target-json', help='dry run: the duplicate target issue as gh issue view --json number,labels,body')
    a = ap.parse_args()
    DRY['on'] = a.dry_run
    meta = json.load(open(a.meta or os.path.join(a.dir, 'meta.json')))
    n = meta['issue']
    try:
        analysis = open(os.path.join(a.dir, 'analysis.md'), encoding='utf-8').read()
    except OSError:
        analysis = ''
    try:
        result = json.load(open(os.path.join(a.dir, 'result.json')))
    except (OSError, ValueError):
        result = {}
    body = clean(analysis) or '_The automatic analysis produced nothing this time (see the workflow run)._'
    head = '<!-- crash-triage -->\n**Automatic triage** (%s%s)\n\n' % (
        'new report' if meta['mode'] == 'new' else 'occurrence on a new build',
        '' if meta.get('symbols') else ', without symbols for this build')
    gh('issue', 'comment', str(n), '--repo', a.repo, '--body-file', '-', input=head + body)

    dup = result.get('duplicate_of')
    if meta['mode'] != 'new' or not isinstance(dup, int) or dup == n or result.get('confidence') != 'high':
        return
    if a.target_json:
        target = json.load(open(a.target_json))
    else:
        target = json.loads(gh('issue', 'view', str(dup), '--repo', a.repo, '--json', 'number,labels,body'))
    if 'crash' not in [l['name'] for l in target.get('labels', [])]:
        sys.stderr.write('#%d is not a crash issue: not closing #%d\n' % (dup, n))
        return
    have = set(SIG.findall(target.get('body') or ''))
    add = [s for s in meta.get('signatures', []) if s not in have]
    if add:
        new_body = (target.get('body') or '') + '\n' + '\n'.join('<!-- crash-sig: %s -->' % s for s in add)
        gh('api', '-X', 'PATCH', 'repos/%s/issues/%d' % (a.repo, dup), '--input', '-', input=json.dumps({'body': new_body}))
    gh('issue', 'comment', str(n), '--repo', a.repo, '--body', 'Duplicate of #%d (same crash; its reports are counted there from now on).' % dup)
    try:
        gh('api', '-X', 'PATCH', 'repos/%s/issues/%d' % (a.repo, n), '-f', 'state=closed', '-f', 'state_reason=duplicate')
    except subprocess.CalledProcessError:
        gh('issue', 'close', str(n), '--repo', a.repo, '--reason', 'not planned')


if __name__ == '__main__':
    main()
