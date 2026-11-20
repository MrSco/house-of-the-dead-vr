// Writes an issue as the relay would file it, for trying the triage digest offline (README.md, "Testing"):
//   node test/make-issue.mjs <crash.txt> <out issue.json> [number] [platform] [version] [commit]
import { readFileSync, writeFileSync } from 'node:fs';
import { parseCrash, crashSignature, issueTitle, issueBody, validateReport } from '../src/relay.js';

const [crashFile, out, number = '1', platform = 'quest', version = '0.1', commit = 'test'] = process.argv.slice(2);
const crash = readFileSync(crashFile, 'utf8');
const parsed = parseCrash(crash, platform);
const r = validateReport({
  schema: 1, platform, build_id: parsed.build_id, crash, crash_time: '2026-11-12T10:00:00Z',
  build_info: { version, commit }, device: { manufacturer: 'Oculus', model: 'Quest 3' },
  settings: { crash_reports: 1 }, log: 'perf: frame 16.6 ms\nshim: DirectDrawCreate\n',
});
const sig = await crashSignature(parsed);
writeFileSync(out, JSON.stringify({
  number: Number(number), title: issueTitle(parsed), body: issueBody(r, parsed, sig),
  labels: [{ name: 'crash' }, { name: 'platform:' + platform }], state: 'OPEN',
}, null, 1));
console.log(out, sig);
