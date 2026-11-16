"""send.sh's server: 201 for a report with "accept": true, 400 otherwise; checks the headers the app sends."""
import http.server, json, sys


class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        ok = (self.path == '/report' and self.headers['Content-Type'] == 'application/json'
              and self.headers['X-Report-Key'] == 'test-key' and json.loads(body).get('accept') is True)
        out = json.dumps({'ok': ok, 'action': 'created', 'issue': 7} if ok else {'ok': False, 'error': 'refused'}).encode()
        self.send_response(201 if ok else 400)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a): sys.stderr.write('server: ' + (a[0] % a[1:]) + '\n')


http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
