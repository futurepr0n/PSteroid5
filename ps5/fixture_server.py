"""Loopback stand-in for the CSSteroids high-score API, for host network tests.

Prints the bound port on stdout, then serves until killed. Responses mirror
routes/highscores.js: GET returns an array of rows, POST returns 201 (echoing
the request body here so the test can check what was sent).
"""
import http.server, json, sys

ROWS = [{"id": 1, "name": "Fixture Ace", "score": 9000, "shipType": "custom", "shipColor": "#00ffcc"},
        {"id": 2, "name": "Second", "score": 4500, "shipType": "default", "shipColor": "white"},
        {"id": 3, "name": "Third", "score": 100, "shipType": "diamond", "shipColor": "red"}]

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def reply(self, status, body, chunked=False):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Connection', 'close')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            for i in range(0, len(data), 7):
                part = data[i:i+7]
                self.wfile.write(b'%x\r\n' % len(part) + part + b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path == '/api/high-scores': self.reply(200, json.dumps(ROWS))
        elif self.path == '/chunked': self.reply(200, json.dumps(ROWS), chunked=True)
        else: self.reply(404, '{"message":"not found"}')
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        self.reply(201 if self.path == '/api/high-scores' else 404, body)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
