// The crash relay's logic, without the Worker around it (src/index.js), so tests can run it under Node with a mocked
// GitHub (test/relay.test.mjs).
//
// A report comes from any platform: src/quest/quest_report.c (Meta Quest, Steam Frame, Android phones and tablets),
// src/platform/windows/crash_win.c (Windows, flat and PC VR), src/platform/apple/crash_apple.m (iPhone, iPad, Mac,
// Apple TV, Vision Pro). It is checked, size-limited and redacted (IP and e-mail addresses, as GoldenEye VR's relay
// does), given a signature, and then either counted on the open `crash` issue that carries that signature (a comment
// for the first few occurrences and for every new build; after that only the counter in the issue body) or filed as a
// new issue labelled `crash` and `platform:<platform>`.
//
// The signature is per binary. The Android builds' handler knows the function names (libhotdvr.so's dynamic symbols):
// the signal and the top app frames' names, so the same crash in another build has the same signature. The shipped
// Windows and Apple binaries have no names: the fault, the top app frames' offsets and the build id; the triage
// digest (triage/digest.py) symbolizes them and closes a later build's issue of the same crash into the first one,
// adding its signature there. The issue body carries hidden markers:
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

// Linux's numbers (Android); Apple's are the same but SIGBUS (10; 7 is SIGEMT there)
const SIGNALS = { 4: 'SIGILL', 5: 'SIGTRAP', 6: 'SIGABRT', 7: 'SIGBUS', 8: 'SIGFPE', 11: 'SIGSEGV' };
const APPLE_SIGNALS = { ...SIGNALS, 7: 'SIGEMT', 10: 'SIGBUS' };

// Where reports come from. family: the binary (its crash format and its symbols); names: its crash handler writes
// real function names (the signature uses them, else the offsets and the build id).
export const PLATFORMS = {
  quest: { name: 'Meta Quest', family: 'android', names: true },
  frame: { name: 'Steam Frame', family: 'android', names: true },
  android: { name: 'Android phone / tablet', family: 'android', names: true },
  windows: { name: 'Windows', family: 'windows', names: false },
  pcvr: { name: 'Windows PC VR', family: 'windows', names: false },
  ios: { name: 'iPhone / iPad', family: 'apple', names: false },
  mac: { name: 'Mac', family: 'apple', names: false },
  tvos: { name: 'Apple TV', family: 'apple', names: false },
  visionos: { name: 'Apple Vision Pro', family: 'apple', names: false },
};
// the app's own binary in a frame, per family (Apple: the executable's name, spaces as _)
export const APP_BINARIES = {
  android: ['libhotdvr.so'], windows: ['HOTDVR.exe'], apple: ['HouseOfTheDeadVR', 'House_of_the_Dead_VR'],
};
const SYMBOLS_HINT = {
  android: 'Frames are offsets into the unstripped libhotdvr.so of that build id (its GNU build id; release `crash-symbols`: `libhotdvr-<build id>.so.gz`).',
  windows: "Frames are offsets into HOTDVR.exe of that build id (its PDB's GUID and age; release `crash-symbols`: `HOTDVR-<build id>.pdb.gz` and `HOTDVR-<build id>.exe.gz`).",
  apple: "Frames are offsets from the app executable's Mach-O header; the build id is its LC_UUID (release `crash-symbols`: `<executable>-<uuid>.dwarf.gz`, the dSYM's DWARF file).",
};
const platformOf = (r) => PLATFORMS[r.platform] || PLATFORMS.quest;

export class HttpError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

// ------------------------------------------------------------------ validation
// As GoldenEye VR's relay: no IP addresses (but loopback) in what is filed, nor e-mail addresses. The rest of a report
// is the app's own (its messages, no paths outside its folder, which it writes <files>).
export const redact = (s) => String(s)
  .replace(/(?<![0-9.])(?!127\.)\d{1,3}(?:\.\d{1,3}){3}(?![0-9.])/g, '[redacted address]')
  .replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, '[redacted e-mail]');
const redactObj = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, redact(v)]));

const isStr = (v, max) => typeof v === 'string' && v.length <= max;
export function validateReport(r) {
  const bad = (m) => { throw new HttpError(400, m); };
  if (!r || typeof r !== 'object' || Array.isArray(r)) bad('not a JSON object');
  if (r.schema !== 1) bad('unknown schema');
  if (r.platform !== undefined && !Object.hasOwn(PLATFORMS, r.platform)) bad('platform');
  // (Android: the GNU build id, 40 digits; Windows: the PDB's GUID and age, 33 or more; Apple: the LC_UUID, 32)
  for (const k of ['build_id', 'running_build_id']) if (r[k] !== undefined && !/^[0-9a-fA-F]{0,48}$/.test(r[k])) bad(k);
  if (!isStr(r.crash, LIMITS.crash) || !/^crash: (signal \d+|[a-z][a-z ()]{2,60}\(0x[0-9A-Fa-f]{8}\))/m.test(r.crash)) bad('crash');
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
    platform: r.platform || 'quest',
    build_id: r.build_id || '', running_build_id: r.running_build_id || '', crash_time: r.crash_time || '',
    device: redactObj(r.device || {}), build_info: redactObj(r.build_info || {}), settings: s,
    crash: redact(r.crash), log: redact(r.log || ''),
  };
}

// ------------------------------------------------------------------ the crash text
// Android and Apple: "crash: signal 11 code 1 addr 0x8 thread 123"; Windows: "crash: access violation (0xC0000005),
// writing 0x8 at HOTDVR.exe+0x1234, thread 123". Then "build-id ..." and the frames: "  pc libhotdvr.so+0x1234 (name)"
// (Windows "  #00 pc HOTDVR.exe+0x1234", no name), or "  at 0x7f12" (outside any library).
export function parseCrash(text, platform = 'quest') {
  const lines = text.split('\n');
  const head = lines.find((l) => l.startsWith('crash: ')) || '';
  const m = /^crash: signal (\d+) code (-?\d+) addr (\S+)/.exec(head);
  const w = m ? null : /^crash: ([a-z][a-z ()]*?) \((0x[0-9A-Fa-f]{8})\)(?:, (?:reading|writing|running) (0x[0-9a-f]+))?/.exec(head);
  const frames = [];
  for (const l of lines) {
    const f = /^\s+(?:#\d+\s+)?(pc|lr|at)\s+(\S+?)\+0x([0-9a-f]+)(?:\s+\((.*)\))?\s*$/.exec(l);
    if (f) frames.push({ kind: f[1], lib: f[2], offset: f[3], symbol: !f[4] || f[4] === '?' ? '' : f[4] });
    else if (/^\s+(?:#\d+\s+)?(pc|lr|at)\s+\S+\s*$/.test(l)) frames.push({ kind: l.trim().replace(/^#\d+\s+/, '').split(/\s+/)[0], lib: '', offset: '', symbol: '' });
  }
  const bid = /^\s*build-id ([0-9a-fA-F]+)/m.exec(text);
  const plat = Object.hasOwn(PLATFORMS, platform) ? platform : 'quest';
  return {
    platform: plat, family: PLATFORMS[plat].family,
    signal: m ? Number(m[1]) : 0, code: m ? Number(m[2]) : w ? '0x' + w[2].slice(2).toUpperCase() : 0,
    addr: m ? m[3] : w && w[3] ? w[3] : '', exception: w ? w[1] : '', frames, build_id: bid ? bid[1] : '',
  };
}
export const signalName = (n, family = 'android') => (family === 'apple' ? APPLE_SIGNALS : SIGNALS)[n] || 'signal ' + n;
// the fault: a signal's name, or a Windows exception ("access violation 0xC0000005")
export const faultName = (p) => (p.exception ? `${p.exception} ${p.code}` : signalName(p.signal, p.family));
const frameName = (f, byOffset) => (f.lib ? f.lib.replace(/\.so$/, '') + (byOffset ? '+0x' + f.offset : ':' + (f.symbol || '?')) : '?');

// The frames a signature is made of: up to 4 in the app's binary (by name where the handler knows names: offsets
// change with every build; else by offset), else (no app frame) the first 4 of any kind. Repeats in a row count once.
export function keyFrames(frames, platform = 'quest') {
  const p = PLATFORMS[platform] || PLATFORMS.quest;
  const app = frames.filter((f) => APP_BINARIES[p.family].includes(f.lib) && (f.symbol || !p.names));
  const pick = (app.length ? app : frames).map((f) => frameName(f, !p.names && app.length > 0));
  return pick.filter((n, i) => i === 0 || n !== pick[i - 1]).slice(0, 4);
}
export async function crashSignature(parsed) {
  const p = PLATFORMS[parsed.platform] || PLATFORMS.quest;
  // (the Android text as it always was; the others: their family, and the build their offsets belong to)
  const text = p.names ? signalName(parsed.signal) + '|' + keyFrames(parsed.frames, parsed.platform).join('|')
    : [p.family, faultName(parsed), ...keyFrames(parsed.frames, parsed.platform), String(parsed.build_id).toLowerCase()].join('|');
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
function deviceLine(r) {
  const d = r.device;
  if (platformOf(r).family === 'android') {
    const name = [d.manufacturer, d.model].filter(Boolean).join(' ') || 'unknown';
    return `${inline(name)}${d.device ? ' (' + inline(d.device) + ')' : ''}, Android ${inline(d.android || '?')} (SDK ${inline(d.sdk || '?')})` +
      (d.os_build ? `, OS build ${inline(d.os_build)}` : '');
  }
  return Object.entries(d).map(([k, v]) => `${k.replace(/_/g, ' ')} ${inline(v)}`).join(', ') || 'unknown';
}
function crashBlock(r) {
  return r.crash.split('\n').slice(0, 60).join('\n');
}

export function issueTitle(parsed) {
  const top = keyFrames(parsed.frames, parsed.platform)[0] || '?';
  const p = PLATFORMS[parsed.platform] || PLATFORMS.quest;
  return `Crash: ${faultName(parsed)} in ${defang(top).replace(/[\r\n`]/g, ' ')} (${p.name})`.slice(0, 120);
}
export function issueBody(r, parsed, sig) {
  const bi = r.build_info;
  const p = platformOf(r);
  const frames = keyFrames(parsed.frames, parsed.platform).map((n) => inline(n)).join(' > ');
  const settings = Object.entries(r.settings).map(([k, v]) => `${k}=${v}`).join('\n');
  return [
    `<!-- crash-sig: ${sig} -->`,
    `<!-- crash-occurrences: 1 -->`,
    `<!-- crash-builds: ${r.build_id} -->`,
    `**Automatic crash report** from the ${p.name} build (crash relay). Signature \`${sig}\`: ${faultName(parsed)}, ${frames || 'no frames'}.`,
    '',
    '| | |',
    '|---|---|',
    `| Occurrences | 1 |`,
    `| Platform | ${p.name} (\`platform:${r.platform}\`) |`,
    `| Build id | ${inline(r.build_id || 'unknown')}${r.running_build_id && r.running_build_id !== r.build_id ? ' (reported by ' + inline(r.running_build_id) + ')' : ''} |`,
    `| Version / commit | ${inline(bi.version || '?')} / ${inline(bi.commit || '?')}${bi.built ? ', built ' + inline(bi.built) : ''} |`,
    `| Crash time | ${inline(r.crash_time || '?')} |`,
    `| Device | ${deviceLine(r)} |`,
    `| Fault | ${faultName(parsed)}${parsed.exception ? '' : ' code ' + parsed.code} at ${inline(parsed.addr || '?')} |`,
    '',
    '### Crash (from crash.txt)',
    fence(crashBlock(r)),
    '',
    "<details><summary>Settings (the menu's)</summary>",
    '',
    fence(settings),
    '</details>',
    '',
    `<details><summary>port.log, last ${LIMITS.issueLogLines} lines</summary>`,
    '',
    fence(tail(r.log, LIMITS.issueLogLines) || '(empty)'),
    '</details>',
    '',
    `_${SYMBOLS_HINT[p.family]} The triage workflow's digest comment symbolizes them. In source paths, \`src/\` is the port (the public repository, at the commit above) and \`gen/\` the recompiled game (generated from the player's own game files, in no repository)._`,
  ].join('\n').slice(0, 65000);
}
export function commentBody(r, parsed, n, retriage) {
  return [
    '<!-- crash-occurrence -->',
    retriage ? '<!-- crash-retriage -->' : '',
    `**Occurrence ${n}**${retriage ? ' on a build this issue has not seen yet' : ''}: build ${inline(r.build_id || 'unknown')}` +
      ` (${inline(r.build_info.version || '?')} / ${inline(r.build_info.commit || '?')}), ${inline(r.crash_time || '?')}, ${platformOf(r).name}, ${deviceLine(r)}.`,
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
    // (a 5xx to the app: it keeps the report and sends it again at its next start, so an outage or an expired
    // token loses nothing)
    if (!res.ok) throw new HttpError(502, `GitHub ${method} ${path.replace(/\?.*/, '')}: ${res.status}`);
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
  const parsed = parseCrash(r.crash, r.platform);
  if (!r.build_id && parsed.build_id) r.build_id = parsed.build_id;
  if (!parsed.build_id) parsed.build_id = r.build_id;
  const sig = await crashSignature(parsed);
  const label = 'platform:' + r.platform;
  const gh = github(env, fetchImpl);

  const issues = await openCrashIssues(gh);
  const same = issues.find((i) => sigsOf(i.body).includes(sig));
  if (same) {
    if (!(same.labels || []).some((l) => (l.name ?? l) === label)) await gh('POST', `/issues/${same.number}/labels`, { labels: [label] });
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
  const made = await gh('POST', '/issues', { title: issueTitle(parsed), body: issueBody(r, parsed, sig), labels: ['crash', label] });
  return { status: 201, json: { ok: true, action: 'created', issue: made.number, signature: sig } };
}
