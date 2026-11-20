#!/usr/bin/env python3
"""Crash triage, the GitHub side (.github/workflows/crash-triage.yml): what digest.py wrote, onto the issue.

    post.py --dir OUT                 the digest: OUT/digest.md as a comment, OUT/labels.txt as labels (sig-xxxxxxxx,
                                      made when missing); for a new issue that digest.py found an older open crash
                                      issue with the same signature for (meta.json duplicate_of): "Duplicate of #N",
                                      closed as a duplicate, its relay signatures added to #N so the relay counts later
                                      reports of it there
    post.py --dir OUT --analysis      the optional AI analysis (OUT/analysis.md, when the workflow has an
                                      ANTHROPIC_API_KEY): a comment, nothing else

digest.py and the analysis only write files; this script, copied before the analysis runs, is what touches GitHub.
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
    """text from the analysis as a comment: no hidden markers (only the relay and the digest write those)"""
    return re.sub(r'<!--.*?-->', '', md, flags=re.S).strip()[:60000]


def post_digest(a, meta):
    n = meta['issue']
    digest = open(os.path.join(a.dir, 'digest.md'), encoding='utf-8').read()
    gh('issue', 'comment', str(n), '--repo', a.repo, '--body-file', '-', input=digest)
    for label in meta.get('labels', []):
        try:
            gh('label', 'create', label, '--repo', a.repo, '--color', 'ededed', '--description', 'Crash signature (triage digest)')
        except subprocess.CalledProcessError:
            pass   # (there already)
        gh('issue', 'edit', str(n), '--repo', a.repo, '--add-label', label)
    dup = meta.get('duplicate_of')
    if meta.get('mode') != 'new' or not isinstance(dup, int) or dup == n: return
    target = json.load(open(a.target_json)) if a.target_json else \
        json.loads(gh('issue', 'view', str(dup), '--repo', a.repo, '--json', 'number,labels,body,state'))
    if 'crash' not in [l['name'] for l in target.get('labels', [])] or target.get('state', 'OPEN').upper() != 'OPEN':
        sys.stderr.write('#%d is not an open crash issue: not closing #%d\n' % (dup, n))
        return
    have = set(SIG.findall(target.get('body') or ''))
    add = [s for s in meta.get('signatures', []) if s not in have]
    if add:
        body = (target.get('body') or '') + '\n' + '\n'.join('<!-- crash-sig: %s -->' % s for s in add)
        gh('api', '-X', 'PATCH', 'repos/%s/issues/%d' % (a.repo, dup), '--input', '-', input=json.dumps({'body': body}))
    gh('issue', 'comment', str(n), '--repo', a.repo, '--body',
       'Duplicate of #%d: the same crash signature (%s). Its later reports are counted there.' % (dup, ', '.join(meta.get('labels', []))))
    try:
        gh('label', 'create', 'duplicate', '--repo', a.repo, '--color', 'cfd3d7')
    except subprocess.CalledProcessError:
        pass
    gh('issue', 'edit', str(n), '--repo', a.repo, '--add-label', 'duplicate')
    try:
        gh('api', '-X', 'PATCH', 'repos/%s/issues/%d' % (a.repo, n), '-f', 'state=closed', '-f', 'state_reason=duplicate')
    except subprocess.CalledProcessError:
        gh('issue', 'close', str(n), '--repo', a.repo, '--reason', 'not planned')


def post_analysis(a, meta):
    try:
        text = clean(open(os.path.join(a.dir, 'analysis.md'), encoding='utf-8').read())
    except OSError:
        text = ''
    if not text:
        print('no analysis written: nothing posted')
        return
    gh('issue', 'comment', str(meta['issue']), '--repo', a.repo, '--body-file', '-',
       input='<!-- crash-analysis -->\n**AI analysis** (optional; the digest above is the deterministic part)\n\n' + text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    ap.add_argument('--dir', required=True)
    ap.add_argument('--meta', help='meta.json from digest.py (default: in --dir)')
    ap.add_argument('--analysis', action='store_true', help='post the AI analysis instead of the digest')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--target-json', help='dry run: the duplicate target as gh issue view --json number,labels,body,state')
    a = ap.parse_args()
    DRY['on'] = a.dry_run
    meta = json.load(open(a.meta or os.path.join(a.dir, 'meta.json')))
    (post_analysis if a.analysis else post_digest)(a, meta)


if __name__ == '__main__':
    main()
