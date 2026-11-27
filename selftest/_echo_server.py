import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self): self._handle()
    def do_PUT(self): self._handle()
    def do_PATCH(self): self._handle()
    def do_DELETE(self): self._handle()
    def _handle(self):
        n = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(n) if n else b''
        self.send_response(201)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"echo":' + str(n).encode() + b',"body":"' + body.replace(b'"', b'\\"') + b'"}')
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b'get-ok')
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', 8099), H).serve_forever()
