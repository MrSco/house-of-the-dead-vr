// House of the Dead VR's Worker, at https://hotdvr.roccojuliano.com (README.md). Today it is the crash relay: the apps'
// crash reports become GitHub issues (relay.js). The apps hold no GitHub credential; this Worker holds a fine-grained
// token (secret GITHUB_TOKEN) that can only read and write the issues of one repository (GITHUB_REPO).
//
//   GET  /             health: {"ok":true,...} (and whether the relay is configured)
//   POST /v1/reports   a crash report (POST /report: the same, the first address the Quest app was built with)
// Later services get paths of their own beside these (/v1/lobbies, ...).
import { handle, HttpError } from './relay.js';

const reply = (status, json) => new Response(JSON.stringify(json), {
  status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
});
const REPORTS = new Set(['/v1/reports', '/report']);

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/' && (request.method === 'GET' || request.method === 'HEAD')) {
      return reply(200, { ok: true, service: 'hotdvr', reports: '/v1/reports', configured: !!(env.GITHUB_TOKEN && env.GITHUB_REPO) });
    }
    if (!REPORTS.has(url.pathname)) return reply(404, { ok: false, error: 'not found' });
    try {
      // (503: the app keeps the report and sends it again at its next start)
      if (!env.GITHUB_TOKEN || !env.GITHUB_REPO) throw new HttpError(503, 'relay not configured');
      const { status, json } = await handle(request, env);
      return reply(status, json);
    } catch (e) {
      const status = e instanceof HttpError ? e.status : 500;
      if (status >= 500) console.error(e instanceof HttpError ? 'relay: ' + e.message : e && e.stack ? e.stack : String(e));
      return reply(status, { ok: false, error: e instanceof HttpError ? e.message : 'internal error' });
    }
  },
};
