// The crash relay: a Cloudflare Worker between the Quest app's crash reports and GitHub issues (see README.md and
// relay.js). The app holds no GitHub credential; this Worker holds a fine-grained token (secret GITHUB_TOKEN) that
// can only read and write the issues of one repository (GITHUB_REPO).
import { handle, HttpError } from './relay.js';

const reply = (status, json) => new Response(JSON.stringify(json), { status, headers: { 'Content-Type': 'application/json' } });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname !== '/report' && url.pathname !== '/') return reply(404, { ok: false, error: 'not found' });
    try {
      if (!env.GITHUB_TOKEN || !env.GITHUB_REPO) throw new HttpError(503, 'relay not configured');
      const { status, json } = await handle(request, env);
      return reply(status, json);
    } catch (e) {
      const status = e instanceof HttpError ? e.status : 500;
      if (status >= 500) console.error(e && e.stack ? e.stack : String(e));
      return reply(status, { ok: false, error: e instanceof HttpError ? e.message : 'internal error' });
    }
  },
};
