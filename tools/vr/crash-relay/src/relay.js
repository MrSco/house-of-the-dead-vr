// The crash relay's logic, without the Worker around it (src/index.js), so tests can run it under Node with a mocked
// GitHub (test/relay.test.mjs).
//
// A report (src/quest/quest_report.c) is checked and size-limited, given a signature (the signal and the top frames'
// function names: the same crash in another build has the same one), and then either counted on the open `crash`
// issue that carries that signature (a comment for the first few occurrences and for every new build; after that only
// the counter in the issue body) or filed as a new issue labelled `crash`. The issue body carries hidden markers:
//   <!-- crash-sig: 0123456789ab -->     (several when triage merged duplicates into this issue)
//   <!-- crash-occurrences: N -->
//   <!-- crash-builds: id1 id2 -->
// and a comment for a new build carries <!-- crash-retriage --> so the triage workflow looks at it again.

export const LIMITS = {
  body: 128 * 1024, crash: 20 * 1024, log: 64 * 1024, field: 120, settings: 64,
  commentOccurrences: 10,     // after this many, occurrences of a known build only bump the counter
  issueLogLines: 200, commentLogLines: 60,
  newIssuesPerDay: 20,        // more new crash issues than this in 24 h: further new signatures are refused (429)
  listPages: 3,               // open crash issues searched: 3 x 100
};

const SIGNALS = { 4: 'SIGILL', 6: 'SIGABRT', 7: 'SIGBUS', 8: 'SIGFPE', 11: 'SIGSEGV' };
const APP_LIB = 'libhotdvr.so';

export class HttpError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

// ------------------------------------------------------------------ validation
const isStr = (v, max) => typeof v === 'string' && v.length <= max;
export function validateReport(r) {
  const bad = (m) => { throw new HttpError(400, m); };
  if (!r || typeof r !== 'object' || Array.isArray(r)) bad('not a JSON object');
  if (r.schema !== 1) bad('unknown schema');
  for (const k of ['build_id', 'running_build_id']) if (r[k] !== undefined && !/^[0-9a-f]{0,40}$/.test(r[k])) bad(k);
  if (!isStr(r.crash, LIMITS.crash) || !/^crash: signal \d+/m.test(r.crash)) bad('crash');
  if (r.log !== undefined && !isStr(r.log, LIMITS.log)) bad('log');
  if (r.crash_time !== undefined && !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(r.crash_time)) bad('crash_time');
  for (const k of ['device', 'build_info']) {
    const o = r[k] ?? {};
    if (typeof o !== 'object' || Array.isArray(o) || Object.keys(o).length > 16) bad(k);
    for (const [kk, v] of Object.entries(o)) if (!/^[a-z_]{1,32}$/.test(kk) || !isStr(v, LIMITS.field)) bad(k + '.' + kk);
  }
  const s = r.settings ?? {};
  if (typeof s !== 'object' || Array.isArray(s) || Object.keys(s).length > LIMITS.settings) bad('settings');
  for (const [k, v] of Object.entries(s)) if (!/^[a-z0-9_]{1,40}$/.test(k) || !Number.isInteger(v) || Math.abs(v) > 1e6) bad('settings.' + k);
  return {
    build_id: r.build_id || '', running_build_id: r.running_build_id || '', crash_time: r.crash_time || '',
    device: r.device || {}, build_info: r.build_info || {}, settings: s, crash: r.crash, log: r.log || '',
  };
}

// ------------------------------------------------------------------ the crash text
// "crash: signal 11 code 1 addr 0x8 thread 123", "  build-id ...", then frames "  pc libhotdvr.so+0x1234 (name)"
// or "  at 0x7f12" (outside any library)
export function parseCrash(text) {
  const lines = text.split('\n');
  const head = lines.find((l) => l.startsWith('crash: ')) || '';
  const m = /^crash: signal (\d+) code (-?\d+) addr (\S+)/.exec(head);
  const frames = [];
  for (const l of lines) {
    const f = /^\s+(pc|lr|at)\s+(\S+?)\+0x([0-9a-f]+)\s+\((.*)\)\s*$/.exec(l);
    if (f) frames.push({ kind: f[1], lib: f[2], offset: f[3], symbol: f[4] === '?' ? '' : f[4] });
    else if (/^\s+(pc|lr|at)\s+\S+\s*$/.test(l)) frames.push({ kind: l.trim().split(/\s+/)[0], lib: '', offset: '', symbol: '' });
  }
  const bid = /^\s+build-id ([0-9a-f]+)/m.exec(text);
  return {
    signal: m ? Number(m[1]) : 0, code: m ? Number(m[2]) : 0, addr: m ? m[3] : '', frames, build_id: bid ? bid[1] : '',
  };
}
export const signalName = (n) => SIGNALS[n] || 'signal ' + n;
const frameName = (f) => (f.lib ? f.lib.replace(/\.so$/, '') + ':' + (f.symbol || '?') : '?');

// The frames a signature is made of: up to 4 in the app's library (its function names; offsets change with every
// build), else (no app frame) the first 4 of any kind. Repeats in a row count once.
export function keyFrames(frames) {
  const app = frames.filter((f) => f.lib === APP_LIB && f.symbol);
  const pick = (app.length ? app : frames).map(frameName);
  return pick.filter((n, i) => i === 0 || n !== pick[i - 1]).slice(0, 4);
}
export async function crashSignature(parsed) {
  const text = signalName(parsed.signal) + '|' + keyFrames(parsed.frames).join('|');
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('').slice(0, 12);
}

// ------------------------------------------------------------------ markdown
// Report text is untrusted: no hidden markers of its own, no breaking out of code blocks, no @mentions outside them.
export const defang = (s) => String(s).replace(/<!--/g, '<!- -').replace(/-->/g, '- ->').replace(/\r/g, '');
const inline = (s) => '`' + defang(s).replace(/[`\n|]/g, ' ').slice(0, LIMITS.field) + '`';
function fence(s, lang = 'text') {
  const t = defang(s);
  const longest = Math.max(2, ...[...t.matchAll(/`+/g)].map((m) => m[0].length));
  const f = '`'.repeat(longest + 1);
  return f + lang + '\n' + t.replace(/\n+$/, '') + '\n' + f;
}
const tail = (s, n) => s.replace(/\n+$/, '').split('\n').slice(-n).join('\n');
function deviceLine(d) {
  const name = [d.manufacturer, d.model].filter(Boolean).join(' ') || 'unknown';
  return `${inline(name)}${d.device ? ' (' + inline(d.device) + ')' : ''}, Android ${inline(d.android || '?')} (SDK ${inline(d.sdk || '?')})` +
    (d.os_build ? `, OS build ${inline(d.os_build)}` : '');
}
function crashBlock(r) {
  // the crash minus the frames outside any library that add nothing
  return r.crash.split('\n').slice(0, 60).join('\n');
}

export function issueTitle(parsed) {
  const top = keyFrames(parsed.frames)[0] || '?';
  return `Crash: ${signalName(parsed.signal)} in ${defang(top).replace(/[\r\n`]/g, ' ')}`.slice(0, 120);
}
export function issueBody(r, parsed, sig) {
  const bi = r.build_info;
  const frames = keyFrames(parsed.frames).map((n) => inline(n)).join(' > ');
  const settings = Object.entries(r.settings).map(([k, v]) => `${k}=${v}`).join('\n');
  return [
    `<!-- crash-sig: ${sig} -->`,
    `<!-- crash-occurrences: 1 -->`,
    `<!-- crash-builds: ${r.build_id} -->`,
    `**Automatic crash report** from the Quest build (crash relay). Signature \`${sig}\`: ${signalName(parsed.signal)}, ${frames || 'no frames'}.`,
    '',
    '| | |',
    '|---|---|',
    `| Occurrences | 1 |`,
    `| Build id | ${inline(r.build_id || 'unknown')}${r.running_build_id && r.running_build_id !== r.build_id ? ' (reported by ' + inline(r.running_build_id) + ')' : ''} |`,
    `| Version / commit | ${inline(bi.version || '?')} / ${inline(bi.commit || '?')}${bi.built ? ', built ' + inline(bi.built) : ''} |`,
    `| Crash time | ${inline(r.crash_time || '?')} |`,
    `| Device | ${deviceLine(r.device)} |`,
    `| Fault | ${signalName(parsed.signal)} code ${parsed.code} at ${inline(parsed.addr || '?')} |`,
    '',
    '### Crash (from crash.txt)',
    fence(crashBlock(r)),
    '',
    '<details><summary>VR settings (vr-options.cfg)</summary>',
    '',
    fence(settings),
    '</details>',
    '',
    `<details><summary>port.log, last ${LIMITS.issueLogLines} lines</summary>`,
    '',
    fence(tail(r.log, LIMITS.issueLogLines) || '(empty)'),
    '</details>',
    '',
    '_Frames are offsets into the unstripped libhotdvr.so of that build id (release `crash-symbols`, `python scripts/quest_tools.py symbols`). The triage workflow symbolizes them._',
  ].join('\n').slice(0, 65000);
}
export function commentBody(r, parsed, n, retriage) {
  return [
    '<!-- crash-occurrence -->',
    retriage ? '<!-- crash-retriage -->' : '',
    `**Occurrence ${n}**${retriage ? ' on a build this issue has not seen yet' : ''}: build ${inline(r.build_id || 'unknown')}` +
      ` (${inline(r.build_info.commit || '?')}), ${inline(r.crash_time || '?')}, ${deviceLine(r.device)}.`,
    '',
    fence(crashBlock(r)),
    '',
    `<details><summary>port.log, last ${LIMITS.commentLogLines} lines</summary>`,
    '',
    fence(tail(r.log, LIMITS.commentLogLines) || '(empty)'),
    '</details>',
  ].filter((l, i) => i !== 1 || l).join('\n').slice(0, 65000);
}

// ------------------------------------------------------------------ GitHub
export function github(env, fetchImpl = fetch) {
  const base = (env.GITHUB_API || 'https://api.github.com') + '/repos/' + env.GITHUB_REPO;
  return async (method, path, body) => {
    const res = await fetchImpl(base + path, {
      method,
      headers: {
        Authorization: 'Bearer ' + env.GITHUB_TOKEN, Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'hotd-crash-relay',
        ...(body ? { 'Content-Type': 'application/json' } : {}),
      },
      body: body ? JSON.stringify(body) : undefined,
    });
    if (!res.ok) throw new HttpError(502, `GitHub ${method} ${path}: ${res.status}`);
    return res.status === 204 ? null : res.json();
  };
}
const sigsOf = (body) => [...(body || '').matchAll(/<!-- crash-sig: ([0-9a-f]{12}) -->/g)].map((m) => m[1]);
const markerNum = (body, name) => Number((new RegExp(`<!-- ${name}: (\\d+) -->`).exec(body || '') || [])[1] || 1);
const markerList = (body, name) => ((new RegExp(`<!-- ${name}: ([^>]*?) -->`).exec(body || '') || [])[1] || '').split(/\s+/).filter(Boolean);

async function openCrashIssues(gh) {
  const all = [];
  for (let page = 1; page <= LIMITS.listPages; page++) {
    const got = await gh('GET', `/issues?labels=crash&state=open&per_page=100&page=${page}`);
    all.push(...got.filter((i) => !i.pull_request));
    if (got.length < 100) break;
  }
  return all;
}

// The whole request: returns { status, json }.
export async function handle(request, env, fetchImpl = fetch, now = Date.now()) {
  if (request.method === 'GET') return { status: 200, json: { ok: true, service: 'hotd crash relay' } };
  if (request.method !== 'POST') throw new HttpError(405, 'POST a report');
  if (env.REPORT_KEY && request.headers.get('X-Report-Key') !== env.REPORT_KEY) throw new HttpError(403, 'bad key');
  const ip = request.headers.get('CF-Connecting-IP') || 'local';
  if (env.LIMITER && !(await env.LIMITER.limit({ key: 'ip:' + ip })).success) throw new HttpError(429, 'slow down');
  if (env.GLOBAL_LIMITER && !(await env.GLOBAL_LIMITER.limit({ key: 'all' })).success) throw new HttpError(429, 'busy');
  const len = Number(request.headers.get('Content-Length') || 0);
  if (len > LIMITS.body) throw new HttpError(413, 'too large');
  const raw = await request.text();
  if (raw.length > LIMITS.body) throw new HttpError(413, 'too large');
  let json;
  try { json = JSON.parse(raw); } catch { throw new HttpError(400, 'not JSON'); }
  const r = validateReport(json);
  const parsed = parseCrash(r.crash);
  if (!r.build_id && parsed.build_id) r.build_id = parsed.build_id;
  const sig = await crashSignature(parsed);
  const gh = github(env, fetchImpl);

  const issues = await openCrashIssues(gh);
  const same = issues.find((i) => sigsOf(i.body).includes(sig));
  if (same) {
    const n = markerNum(same.body, 'crash-occurrences') + 1;
    const builds = markerList(same.body, 'crash-builds');
    const newBuild = !!r.build_id && !builds.includes(r.build_id);
    if (newBuild) builds.push(r.build_id);
    let body = same.body.replace(/<!-- crash-occurrences: \d+ -->/, `<!-- crash-occurrences: ${n} -->`)
      .replace(/^\| Occurrences \| \d+ \|$/m, `| Occurrences | ${n} |`);
    body = /<!-- crash-builds: [^>]*?-->/.test(body)
      ? body.replace(/<!-- crash-builds: [^>]*?-->/, `<!-- crash-builds: ${builds.slice(-40).join(' ')} -->`)
      : `<!-- crash-builds: ${builds.join(' ')} -->\n` + body;
    if (!/<!-- crash-occurrences: \d+ -->/.test(same.body)) body = `<!-- crash-occurrences: ${n} -->\n` + body;
    await gh('PATCH', `/issues/${same.number}`, { body });
    let action = 'counted';
    if (newBuild || n <= LIMITS.commentOccurrences) {
      await gh('POST', `/issues/${same.number}/comments`, { body: commentBody(r, parsed, n, newBuild) });
      action = 'commented';
    }
    return { status: 200, json: { ok: true, action, issue: same.number, signature: sig, occurrences: n } };
  }
  const recent = issues.filter((i) => now - Date.parse(i.created_at) < 86400e3).length;
  if (recent >= Number(env.NEW_ISSUES_PER_DAY || LIMITS.newIssuesPerDay)) throw new HttpError(429, 'too many new crashes today');
  const made = await gh('POST', '/issues', { title: issueTitle(parsed), body: issueBody(r, parsed, sig), labels: ['crash'] });
  return { status: 201, json: { ok: true, action: 'created', issue: made.number, signature: sig } };
}
