#!/usr/bin/env python3
"""Serve only a fixed diagnostic page; exit after 120 seconds. No file serving."""
import argparse
import http.server
import time

PAGE = b'''<!doctype html><html><head><meta charset="utf-8">
<title>PS5 browser test</title></head>
<body style="background:#fff;color:#000;font:32px sans-serif;padding:48px">
<h1>PS5 browser test</h1><p>Local page loaded. No Discord login involved.</p>
<p id="js">JavaScript has not run.</p>
<script>document.getElementById('js').innerHTML='JavaScript ran.';
var marker=new Image();marker.src='/js-ok';</script></body></html>'''


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/probe':
            body, code, kind = PAGE, 200, 'text/html; charset=utf-8'
        elif self.path == '/js-ok':
            body, code, kind = b'', 204, 'text/plain'
        else:
            body, code, kind = b'Not found', 404, 'text/plain'
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def setup(self):
        super().setup()
        self.connection.settimeout(3)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', required=True)
    args = parser.parse_args()
    with http.server.HTTPServer((args.bind, 9062), Handler) as server:
        server.timeout = 1
        deadline = time.monotonic() + 120
        print(f'Serving http://{args.bind}:9062/probe for 120 seconds', flush=True)
        while time.monotonic() < deadline:
            server.handle_request()
