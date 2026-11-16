// A stand-in for the GitHub issues API, for trying the Worker under `wrangler dev` (README.md, "Testing"):
//   node test/mock-github.mjs 18790
//   npx wrangler dev --var GITHUB_API:http://127.0.0.1:18790 --var GITHUB_TOKEN:test
// It prints every call and keeps the issues in memory (GET /_issues shows them).
import http from 'node:http';

const issues = [], comments = [];
const port = Number(process.argv[2] || 18790);
http.createServer(async (req, res) => {
  let body = '';
  for await (const c of req) body += c;
  const u = new URL(req.url, 'http://x');
  const send = (status, j) => { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(j)); };
  console.log(req.method, u.pathname + u.search, req.headers.authorization ? '(token)' : '(NO TOKEN)');
  if (u.pathname === '/_issues') return send(200, { issues, comments });
  const m = /^\/repos\/[^/]+\/[^/]+(\/issues(?:\/(\d+)(\/comments)?)?)$/.exec(u.pathname);
  if (!m) return send(404, { message: 'Not Found' });
  const [, , num, isComments] = m;
  const data = body ? JSON.parse(body) : null;
  if (req.method === 'GET' && !num) {
    const page = Number(u.searchParams.get('page') || 1);
    return send(200, issues.filter((i) => i.state === 'open').slice((page - 1) * 100, page * 100));
  }
  if (req.method === 'POST' && !num) {
    const i = { number: issues.length + 1, state: 'open', created_at: new Date().toISOString(), html_url: `https://github.test/issues/${issues.length + 1}`, ...data,
      labels: (data.labels || []).map((name) => ({ name })) };
    issues.push(i);
    return send(201, i);
  }
  if (req.method === 'PATCH' && num && !isComments) { Object.assign(issues[num - 1], data); return send(200, issues[num - 1]); }
  if (req.method === 'POST' && isComments) { comments.push({ issue: Number(num), ...data }); return send(201, { id: comments.length }); }
  send(404, { message: 'Not Found' });
}).listen(port, '127.0.0.1', () => console.log('mock GitHub on http://127.0.0.1:' + port));
