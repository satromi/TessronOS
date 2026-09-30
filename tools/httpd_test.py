#!/usr/bin/env python3
"""A small HTTP server for the tests of マイクロスクリプト's network statements.

  httpd_test.py [--host 127.0.0.1] [--port 2180] [--exit-on-eof]

Two ports: --port answers in plain JSON, --port + 1 as an event stream
(text/event-stream), as the MCP servers the samples of 仮身サンプル talk to
answer (protocol version 2026-07-28, no sessions: the MCP-Protocol-Version,
Mcp-Method and Mcp-Name headers must say what the body says). Besides the
MCP endpoint /mcp, on either port:

  GET /hello      a text in chunks (Transfer-Encoding: chunked)
  GET /todos/1    a JSON object, as jsonplaceholder has it
  GET /headers    the request's headers, as JSON

What each request was is written to standard error, the Authorization
header by its first letters only.
"""

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL_VERSION = '2026-07-28'
META_VERSION_KEY = 'io.modelcontextprotocol/protocolVersion'
META_SERVER_INFO_KEY = 'io.modelcontextprotocol/serverInfo'


def rpc_error(mid, code, message, data=None):
    e = {'code': code, 'message': message}
    if data is not None:
        e['data'] = data
    return {'jsonrpc': '2.0', 'id': mid, 'error': e}


def check_headers(h, msg):
    version = h.get('MCP-Protocol-Version')
    if not version:
        return 400, rpc_error(msg.get('id'), -32020, 'Header mismatch: MCP-Protocol-Version header is missing')
    if version != PROTOCOL_VERSION:
        return 400, rpc_error(msg.get('id'), -32022, 'Unsupported protocol version',
                              {'supported': [PROTOCOL_VERSION], 'requested': version})
    meta = (msg.get('params') or {}).get('_meta') or {}
    if meta.get(META_VERSION_KEY) != version:
        return 400, rpc_error(msg.get('id'), -32020, 'Header mismatch: the version differs from _meta')
    if h.get('Mcp-Method') != msg.get('method'):
        return 400, rpc_error(msg.get('id'), -32020, 'Header mismatch: Mcp-Method')
    if msg.get('method') == 'tools/call' and h.get('Mcp-Name') != (msg.get('params') or {}).get('name'):
        return 400, rpc_error(msg.get('id'), -32020, 'Header mismatch: Mcp-Name')
    return None


def route(msg, server_name):
    info = {META_SERVER_INFO_KEY: {'name': server_name, 'version': '0.2'}}
    method = msg.get('method')
    params = msg.get('params') or {}
    if method == 'server/discover':
        result = {'resultType': 'complete', 'supportedVersions': [PROTOCOL_VERSION],
                  'capabilities': {'tools': {}}, '_meta': info}
    elif method == 'tools/list':
        result = {'resultType': 'complete',
                  'tools': [{'name': 'echo', 'description': 'echoes text'},
                            {'name': 'add', 'description': 'adds two numbers'}],
                  '_meta': info}
    elif method == 'tools/call':
        args = params.get('arguments') or {}
        name = params.get('name')
        if name == 'echo':
            text = str(args.get('text', ''))
        elif name == 'add':
            text = str(int(args.get('a', 0)) + int(args.get('b', 0)))
        else:
            return 200, rpc_error(msg.get('id'), -32602, 'unknown tool: %s' % name)
        result = {'resultType': 'complete', 'content': [{'type': 'text', 'text': text}], '_meta': info}
    else:
        return 404, rpc_error(msg.get('id'), -32601, 'method not found: %s' % method)
    return 200, {'jsonrpc': '2.0', 'id': msg.get('id'), 'result': result}


def make_handler(sse, quiet):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, fmt, *args):
            pass

        def note(self, what):
            if quiet:
                return
            auth = self.headers.get('Authorization')
            tail = ' [Authorization: %s...]' % auth[:12] if auth else ''
            sys.stderr.write('httpd_test: %s %s %s%s\n' % (self.command, self.path, what, tail))

        def send(self, status, ctype, body, chunked=False):
            self.send_response(status)
            self.send_header('Content-Type', ctype)
            self.send_header('Connection', 'close')
            if chunked:
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for i in range(0, len(body), 7):
                    piece = body[i:i + 7]
                    self.wfile.write(b'%x\r\n' % len(piece) + piece + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            self.close_connection = True

        def do_GET(self):
            if self.path == '/hello':
                self.note('hello')
                self.send(200, 'text/plain; charset=utf-8', 'こんにちは TessronOS\n'.encode('utf-8'), chunked=True)
            elif self.path == '/todos/1':
                self.note('todo')
                self.send(200, 'application/json; charset=utf-8',
                          json.dumps({'userId': 1, 'id': 1, 'title': 'delectus aut autem',
                                      'completed': False}).encode('utf-8'))
            elif self.path == '/headers':
                self.note('headers')
                self.send(200, 'application/json', json.dumps(dict(self.headers.items())).encode('utf-8'))
            else:
                self.note('404')
                self.send(404, 'text/plain', b'not here\n')

        def do_POST(self):
            n = int(self.headers.get('Content-Length', '0'))
            raw = self.rfile.read(n) if n > 0 else b''
            if self.path != '/mcp':
                self.note('404')
                self.send(404, 'text/plain', b'not here\n')
                return
            try:
                msg = json.loads(raw.decode('utf-8'))
            except ValueError:
                self.note('parse error')
                self.send(400, 'application/json', json.dumps(rpc_error(None, -32700, 'Parse error')).encode())
                return
            bad = check_headers(self.headers, msg)
            status, body = bad if bad else route(msg, 'sse-demo-mcp' if sse else 'demo-mcp')
            self.note('%s -> %s' % (msg.get('method'), 'error %d' % body['error']['code'] if 'error' in body else 'ok'))
            if sse and status == 200:
                note = {'jsonrpc': '2.0', 'method': 'notifications/progress',
                        'params': {'progress': 1, 'total': 1}}
                text = (': keep-alive\n\n'
                        'event: message\ndata: %s\n\n'
                        'event: message\ndata: %s\n\n' % (json.dumps(note), json.dumps(body)))
                self.send(200, 'text/event-stream', text.encode('utf-8'))
            else:
                self.send(status, 'application/json', json.dumps(body).encode('utf-8'))

    return Handler


def serve(host, port, quiet=False):
    """Both servers, each in a thread of its own; returns at once."""
    for p, sse in ((port, False), (port + 1, True)):
        srv = ThreadingHTTPServer((host, p), make_handler(sse, quiet))
        srv.daemon_threads = True
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    if not quiet:
        sys.stderr.write('httpd_test: plain JSON on %s:%d, event stream on %s:%d\n' % (host, port, host, port + 1))


def main():
    ap = argparse.ArgumentParser(description='A small HTTP server for the MicroScript network tests.')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=2180)
    ap.add_argument('--exit-on-eof', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()
    serve(args.host, args.port, args.quiet)
    try:
        if args.exit_on_eof:
            sys.stdin.read()
        else:
            threading.Event().wait()
    except (KeyboardInterrupt, OSError, ValueError):
        pass
    os._exit(0)


if __name__ == '__main__':
    main()
