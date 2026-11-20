// npm test (Node 18+): the relay against an in-memory GitHub.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { handle, parseCrash, crashSignature, keyFrames, validateReport, redact, HttpError } from '../src/relay.js';
import worker from '../src/index.js';

const fixture = (name) => readFileSync(new URL('./fixtures/' + name, import.meta.url), 'utf8');

function report(over = {}) {
  return {
    schema: 1, build_id: 'aa'.repeat(20), running_build_id: 'aa'.repeat(20), crash_time: '2026-11-12T10:00:00Z',
    build_info: { version: '0.1', commit: '8349ba5e61ee', built: '2026-11-12T09:00:00Z' },
    device: { manufacturer: 'Oculus', model: 'Quest 3', device: 'eureka', android: '14', sdk: '34', os_build: 'UP1A.231005.007.A1' },
    settings: { aim_guide: 0, crash_reports: 1 },
    crash: fixture('crash.txt'), log: 'line 1\nline 2 ```` @someone <!-- crash-sig: 000000000000 -->\nperf: 16.6 ms\n',
    ...over,
  };
}

// A fake GitHub: issues in memory, every call recorded.
function fakeGitHub() {
  const gh = { issues: [], comments: [], calls: [] };
  gh.fetch = async (url, init) => {
    const u = new URL(url);
    const method = init.method || 'GET';
    gh.calls.push(method + ' ' + u.pathname + u.search);
    assert.equal(init.headers.Authorization, 'Bearer tok');
    assert.ok(u.pathname.startsWith('/repos/o/r/'));
    const path = u.pathname.slice('/repos/o/r'.length);
    const body = init.body ? JSON.parse(init.body) : null;
    const json = (status, j) => new Response(JSON.stringify(j), { status });
    if (method === 'GET' && path === '/issues') {
      const page = Number(u.searchParams.get('page'));
      const open = gh.issues.filter((i) => i.state === 'open' && i.labels.includes('crash'));
      return json(200, open.slice((page - 1) * 100, page * 100).map((i) => ({ ...i, labels: i.labels.map((name) => ({ name })) })));
    }
    if (method === 'POST' && path === '/issues') {
      const i = { number: gh.issues.length + 1, state: 'open', created_at: new Date().toISOString(), html_url: 'x', ...body };
      gh.issues.push(i);
      return json(201, i);
    }
    let m;
    if (method === 'PATCH' && (m = /^\/issues\/(\d+)$/.exec(path))) {
      const i = gh.issues[m[1] - 1]; Object.assign(i, body); return json(200, i);
    }
    if (method === 'POST' && (m = /^\/issues\/(\d+)\/labels$/.exec(path))) {
      const i = gh.issues[m[1] - 1]; i.labels = [...new Set([...i.labels, ...body.labels])]; return json(200, i.labels.map((name) => ({ name })));
    }
    if (method === 'POST' && (m = /^\/issues\/(\d+)\/comments$/.exec(path))) {
      gh.comments.push({ issue: Number(m[1]), ...body }); return json(201, { id: gh.comments.length });
    }
    return json(404, { message: 'not found' });
  };
  return gh;
}
const env = (extra = {}) => ({ GITHUB_TOKEN: 'tok', GITHUB_REPO: 'o/r', GITHUB_API: 'https://gh.test', ...extra });
const post = (body, headers = {}, path = '/v1/reports') => new Request('https://relay.test' + path, {
  method: 'POST', body: typeof body === 'string' ? body : JSON.stringify(body), headers: { 'Content-Type': 'application/json', ...headers },
});

test('parses the crash handler output', () => {
  const p = parseCrash(fixture('crash.txt'));
  assert.equal(p.signal, 11);
  assert.equal(p.build_id, 'aa'.repeat(20));
  assert.equal(p.frames[0].kind, 'pc');
  assert.equal(p.frames[0].symbol, 'qmenu_update');
  assert.deepEqual(keyFrames(p.frames), ['libhotdvr:qmenu_update', 'libhotdvr:frame', 'libhotdvr:android_main']);
});

test('the signature ignores offsets and build ids', async () => {
  const a = parseCrash(fixture('crash.txt'));
  const b = parseCrash(fixture('crash.txt').replace(/\+0x[0-9a-f]+/g, '+0x42').replace(/build-id a+/, 'build-id ' + 'bb'.repeat(20)));
  assert.equal(await crashSignature(a), await crashSignature(b));
  const c = parseCrash(fixture('crash.txt').replace('signal 11', 'signal 6'));
  assert.notEqual(await crashSignature(a), await crashSignature(c));
  assert.match(await crashSignature(a), /^[0-9a-f]{12}$/);
});

test('an abort is keyed by the app frames under libc', async () => {
  const p = parseCrash(fixture('abort.txt'));
  assert.deepEqual(keyFrames(p.frames), ['libhotdvr:host_fatal', 'libhotdvr:sub_4a2b10']);
});

test('rejects bad reports', () => {
  assert.throws(() => validateReport({ schema: 2 }), HttpError);
  assert.throws(() => validateReport(report({ crash: 'hello' })), /crash/);
  assert.throws(() => validateReport(report({ build_id: '../../etc' })), /build_id/);
  assert.throws(() => validateReport(report({ settings: { a: 'x' } })), /settings/);
  assert.throws(() => validateReport(report({ device: { model: 'x'.repeat(500) } })), /device/);
  assert.throws(() => validateReport(report({ log: 'x'.repeat(70000) })), /log/);
});

test('first report opens an issue, the next ones are counted on it', async () => {
  const gh = fakeGitHub();
  let r = await handle(post(report()), env(), gh.fetch);
  assert.equal(r.status, 201);
  assert.equal(r.json.action, 'created');
  const issue = gh.issues[0];
  assert.deepEqual(issue.labels, ['crash', 'platform:quest']);
  assert.match(issue.title, /^Crash: SIGSEGV in libhotdvr:qmenu_update \(Meta Quest\)$/);
  assert.match(issue.body, new RegExp(`<!-- crash-sig: ${r.json.signature} -->`));
  // the log's fake marker is defanged, its backticks can't close the fence
  assert.equal([...issue.body.matchAll(/<!-- crash-sig: /g)].length, 1);
  assert.match(issue.body, /`````text\nline 1/);

  r = await handle(post(report()), env(), gh.fetch);
  assert.equal(r.json.action, 'commented');
  assert.equal(r.json.issue, 1);
  assert.equal(r.json.occurrences, 2);
  assert.match(gh.issues[0].body, /<!-- crash-occurrences: 2 -->/);
  assert.match(gh.issues[0].body, /^\| Occurrences \| 2 \|$/m);
  assert.equal(gh.comments.length, 1);
  assert.doesNotMatch(gh.comments[0].body, /crash-retriage/);

  // the same crash in a new build: a comment the triage workflow looks at again
  r = await handle(post(report({ build_id: 'cc'.repeat(20), crash: fixture('crash.txt').replace(/\+0x[0-9a-f]+/g, '+0x99') })), env(), gh.fetch);
  assert.equal(r.json.action, 'commented');
  assert.match(gh.comments[1].body, /<!-- crash-retriage -->/);
  assert.match(gh.issues[0].body, new RegExp(`<!-- crash-builds: ${'aa'.repeat(20)} ${'cc'.repeat(20)} -->`));
  assert.equal(gh.issues.length, 1);
});

test('after the first occurrences only the counter moves', async () => {
  const gh = fakeGitHub();
  for (let i = 0; i < 14; i++) await handle(post(report()), env(), gh.fetch);
  assert.equal(gh.issues.length, 1);
  assert.equal(gh.comments.length, 9);          // occurrences 2..10
  assert.match(gh.issues[0].body, /<!-- crash-occurrences: 14 -->/);
});

test('a signature merged into another issue (triage) lands there', async () => {
  const gh = fakeGitHub();
  await handle(post(report()), env(), gh.fetch);
  const other = report({ crash: fixture('abort.txt') });
  const r1 = await handle(post(other), env(), gh.fetch);
  assert.equal(r1.json.issue, 2);
  // triage closed #2 as a duplicate of #1 and added its signature to #1
  gh.issues[1].state = 'closed';
  gh.issues[0].body += `\n<!-- crash-sig: ${r1.json.signature} -->`;
  const r2 = await handle(post(other), env(), gh.fetch);
  assert.equal(r2.json.issue, 1);
});

test('limits: key, size, rate, new issues per day', async () => {
  const gh = fakeGitHub();
  await assert.rejects(handle(post(report()), env({ REPORT_KEY: 'k' }), gh.fetch), (e) => e.status === 403);
  const ok = await handle(post(report(), { 'X-Report-Key': 'k' }), env({ REPORT_KEY: 'k' }), gh.fetch);
  assert.equal(ok.status, 201);
  await assert.rejects(handle(post('{' + ' '.repeat(140000) + '}'), env(), gh.fetch), (e) => e.status === 413);
  await assert.rejects(handle(post('nope'), env(), gh.fetch), (e) => e.status === 400);
  const limiter = { n: 0, limit: async () => ({ success: ++limiter.n <= 1 }) };
  await handle(post(report()), env({ LIMITER: limiter }), gh.fetch);
  await assert.rejects(handle(post(report()), env({ LIMITER: limiter }), gh.fetch), (e) => e.status === 429);
  // NEW_ISSUES_PER_DAY=1: one issue exists from today, a new signature is refused, a known one still counts
  await assert.rejects(handle(post(report({ crash: fixture('abort.txt') })), env({ NEW_ISSUES_PER_DAY: '1' }), gh.fetch),
    (e) => e.status === 429);
  assert.equal((await handle(post(report()), env({ NEW_ISSUES_PER_DAY: '1' }), gh.fetch)).json.action, 'commented');
});

test('the Worker entry point answers JSON', async () => {
  let res = await worker.fetch(post(report()), {});
  assert.equal(res.status, 503);                       // no token yet: the app keeps the report for later
  assert.deepEqual(await res.json(), { ok: false, error: 'relay not configured' });
  res = await worker.fetch(new Request('https://relay.test/'), {});
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { ok: true, service: 'hotdvr', reports: '/v1/reports', configured: false });
  res = await worker.fetch(new Request('https://relay.test/other'), env());
  assert.equal(res.status, 404);
  res = await worker.fetch(new Request('https://relay.test/v1/lobbies'), env());
  assert.equal(res.status, 404);
  res = await worker.fetch(post('{"schema":1}'), env());
  assert.equal(res.status, 400);
  assert.equal((await res.json()).ok, false);
  res = await worker.fetch(post('{"schema":1}', {}, '/report'), env());   // (the first address)
  assert.equal(res.status, 400);
});

test('Windows: the exception, frames by offset, the PDB build id', async () => {
  const text = fixture('windows.txt');
  const p = parseCrash(text, 'windows');
  assert.equal(p.exception, 'access violation');
  assert.equal(p.code, '0xC0000005');
  assert.equal(p.addr, '0x8');
  assert.equal(p.build_id, 'D64CB03B731E4F65B0DB15CF071F0205B');
  assert.equal(p.frames.length, 5);
  assert.deepEqual(keyFrames(p.frames, 'windows'), ['HOTDVR.exe+0x1a2b3c', 'HOTDVR.exe+0x1a2a00', 'HOTDVR.exe+0x10f00']);
  // offsets belong to one build: another build is another signature (the digest merges them)
  const other = parseCrash(text.replace(/build-id \w+/, 'build-id D64CB03B731E4F65B0DB15CF071F0205C'), 'windows');
  assert.notEqual(await crashSignature(p), await crashSignature(other));
  // flat and PC VR are the same exe: the same signature
  assert.equal(await crashSignature(p), await crashSignature(parseCrash(text, 'pcvr')));
  const gh = fakeGitHub();
  const r = await handle(post(report({ platform: 'windows', build_id: p.build_id, running_build_id: p.build_id, crash: text,
    device: { os: 'windows', windows: '10.0.26100', cpus: '16' }, build_info: { app: 'HOTDVR.exe', version: '0.6.0', commit: '386c1980c47f' } })), env(), gh.fetch);
  assert.equal(r.status, 201);
  assert.deepEqual(gh.issues[0].labels, ['crash', 'platform:windows']);
  assert.equal(gh.issues[0].title, 'Crash: access violation 0xC0000005 in HOTDVR.exe+0x1a2b3c (Windows)');
  assert.match(gh.issues[0].body, /\| Device \| os `windows`, windows `10\.0\.26100`, cpus `16` \|/);
  assert.match(gh.issues[0].body, /HOTDVR-<build id>\.pdb\.gz/);
  // the same crash from PC VR: counted there, and labelled
  const r2 = await handle(post(report({ platform: 'pcvr', build_id: p.build_id, crash: text })), env(), gh.fetch);
  assert.equal(r2.json.issue, 1);
  assert.deepEqual(gh.issues[0].labels, ['crash', 'platform:windows', 'platform:pcvr']);
});

test('Apple: signals by Apple numbers, frames by offset, the UUID', async () => {
  const text = fixture('apple.txt');
  const p = parseCrash(text, 'ios');
  assert.equal(p.signal, 11);
  assert.equal(p.build_id, '96eb6c86c22b3a1498d876874288e510');
  assert.deepEqual(keyFrames(p.frames, 'ios'), ['HouseOfTheDeadVR+0x1a2b3c', 'HouseOfTheDeadVR+0x1a2a00', 'HouseOfTheDeadVR+0x10f00']);
  assert.equal(parseCrash(text.replace('signal 11', 'signal 10'), 'mac').signal, 10);
  const gh = fakeGitHub();
  const r = await handle(post(report({ platform: 'visionos', build_id: p.build_id, crash: text.replace('signal 11', 'signal 10'),
    device: { os: 'visionOS 26.1', model: 'RealityDevice14,1' } })), env(), gh.fetch);
  assert.equal(r.status, 201);
  assert.equal(gh.issues[0].title, 'Crash: SIGBUS in HouseOfTheDeadVR+0x1a2b3c (Apple Vision Pro)');
  assert.deepEqual(gh.issues[0].labels, ['crash', 'platform:visionos']);
  await assert.rejects(handle(post(report({ platform: 'amiga' })), env(), gh.fetch), (e) => e.status === 400);
});

test('IP and e-mail addresses are redacted', async () => {
  assert.equal(redact('peer 192.168.1.20:5000 and 127.0.0.1, v1.2.3, me@example.com'),
    'peer [redacted address]:5000 and 127.0.0.1, v1.2.3, [redacted e-mail]');
  const gh = fakeGitHub();
  await handle(post(report({ log: 'online: host 192.0.2.26 joined\n' })), env(), gh.fetch);
  assert.doesNotMatch(gh.issues[0].body, /192\.0\.2\.26/);
  assert.match(gh.issues[0].body, /host \[redacted address\] joined/);
});

test('a report written by the Quest client is accepted', async (t) => {
  let text;
  try { text = fixture('client-report.json'); } catch { return t.skip('no client fixture (tools/vr/crash-relay/test/client/run.sh makes it)'); }
  const gh = fakeGitHub();
  const r = await handle(post(text), env(), gh.fetch);
  assert.equal(r.status, 201);
  assert.match(gh.issues[0].body, /Quest 3/);
  assert.doesNotMatch(gh.issues[0].body, /\/sdcard\/Android|\/storage\/emulated/);
});
