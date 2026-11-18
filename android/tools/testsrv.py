#!/usr/bin/env python3
# 测试用的本地 HTTP 服务：/ 302 跳转到 /final，/final 返回固定内容。
# 用来验证「入口 URL 跟随跳转 → 本地端口转发」这条路。
import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.send_response(302)
            self.send_header("Location", "/final")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = ("txtmux-ok path=%s\n" % self.path).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("testsrv %s\n" % (fmt % args))


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", PORT), H) as httpd:
    print("testsrv listening on 127.0.0.1:%d" % PORT, flush=True)
    httpd.serve_forever()
