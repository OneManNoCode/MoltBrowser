# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The kit's small servers. Each runs as its own process, as the invoking
(non-root) user, started by the harness through `leaktest.py service ...`:

  pages     the test-page server: HTTP, HTTPS and the AIA-test HTTPS port,
            behind the kit's onion services (or on loopback for regular
            windows); templated pages, request log, WebSocket handshake
  dns       the namespace's only resolver: logs every query and answers
            NXDOMAIN, so every lookup the browser makes is on record
  loopback  the loopback canary: a TCP port on this machine no MoltNet page
            may reach
  proxy     a logging HTTP proxy that forwards nothing (step 13)

All logs are JSON lines with a wall-clock `ts`, written and flushed per
line. Request data is attacker-controlled in principle (it comes from
pages), so it is length-capped before it is logged.
"""

import base64
import hashlib
import html
import json
import os
import re
import socket
import socketserver
import ssl
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

from . import dissect

_NAME_RE = re.compile(r'^[a-z0-9][a-z0-9_.-]{0,63}$')
_NONCE_RE = re.compile(r'^[a-z0-9]{6,24}$')
_TEMPLATE_RE = re.compile(r'\{\{(H:)?([A-Za-z0-9_-]{1,40})\}\}')
_LOG_HEADERS = ('accept-language', 'dnt', 'sec-gpc', 'user-agent', 'dpr',
                'viewport-width', 'device-memory', 'rtt', 'downlink', 'ect',
                'save-data', 'width', 'purpose', 'sec-purpose', 'upgrade',
                'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site',
                'content-type', 'ping-from', 'ping-to')


class JsonLog(object):
    def __init__(self, path):
        self.lock = threading.Lock()
        self.f = open(path, 'a') if path else None

    def write(self, **item):
        item.setdefault('ts', time.time())
        line = json.dumps(item, sort_keys=True)
        with self.lock:
            if self.f:
                self.f.write(line + '\n')
                self.f.flush()


def _cap(text, n=300):
    text = str(text)
    return text if len(text) <= n else text[:n] + '...'


# ---- test pages -------------------------------------------------------------

class PageConfig(object):
    def __init__(self, data):
        self.pages_dir = data['pages_dir']
        self.canary = data['canary']
        self.values = dict(data.get('values') or {})
        self.tests = data.get('tests') or {}   # nonce -> {ipv4, ipv6, id}

    def template_values(self, nonce):
        values = dict(self.values)
        values['NONCE'] = nonce
        values['CANARY'] = self.canary
        test = self.tests.get(nonce) or {}
        values['CIP4'] = test.get('ipv4', '198.51.100.250')
        values['CIP6'] = test.get('ipv6', '2001:db8::250')
        values['TEST'] = test.get('id', '')
        return values

    def render(self, text, nonce):
        values = self.template_values(nonce)

        def sub(m):
            if m.group(1):
                kind = m.group(2).lower()
                return html.escape('%s-%s.%s' % (kind, nonce, self.canary))
            return html.escape(str(values.get(m.group(2), '')), quote=True)
        return _TEMPLATE_RE.sub(sub, text)


def _page_headers(name, nonce, cfg, scheme):
    """Extra response headers some test pages need."""
    h = []

    def host(kind):
        return '%s-%s.%s' % (kind, nonce, cfg.canary)
    if name == 'csp.html':
        h.append(('Content-Security-Policy',
                  "default-src 'self'; img-src 'self'; script-src 'self'; "
                  "style-src 'self' 'unsafe-inline'; report-uri "
                  "https://%s/r /canary/csp-%s; report-to mlk" %
                  (host('cspreport'), nonce)))
        h.append(('Content-Security-Policy-Report-Only',
                  "connect-src 'none'; report-uri https://%s/r" %
                  host('cspro')))
        h.append(('Reporting-Endpoints',
                  'mlk="https://%s/r"' % host('reporting')))
        h.append(('Report-To', json.dumps({
            'group': 'nel', 'max_age': 86400,
            'endpoints': [{'url': 'https://%s/r' % host('nelreport')}]})))
        h.append(('NEL', '{"report_to":"nel","max_age":86400,'
                         '"success_fraction":1.0,"failure_fraction":1.0}'))
        h.append(('Cross-Origin-Opener-Policy-Report-Only',
                  'same-origin; report-to="mlk"'))
    if name == 'fp.html':
        h.append(('Accept-CH', 'Sec-CH-UA-Platform-Version, Sec-CH-UA-Arch, '
                  'Sec-CH-UA-Bitness, Sec-CH-UA-Model, Sec-CH-UA-WoW64, '
                  'Sec-CH-UA-Full-Version-List, Sec-CH-UA-Full-Version, '
                  'Sec-CH-UA-Form-Factors, Sec-CH-DPR, DPR, '
                  'Sec-CH-Viewport-Width, Viewport-Width, '
                  'Sec-CH-Viewport-Height, Sec-CH-Device-Memory, '
                  'Device-Memory, RTT, Downlink, ECT, Sec-CH-Width, '
                  'Sec-CH-Prefers-Color-Scheme, Sec-CH-Prefers-Reduced-Motion'))
        h.append(('Critical-CH', 'Sec-CH-UA-Platform-Version, Sec-CH-DPR'))
    if scheme == 'https':
        h.append(('Alt-Svc', 'h3=":443"; ma=86400, h3-29=":443"; ma=86400'))
    return h


class PageHandler(BaseHTTPRequestHandler):
    server_version = 'moltleak'
    sys_version = ''
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):  # quiet; we keep our own log
        pass

    def _log(self, status, extra=None):
        headers = {}
        hints = {}
        for name, value in self.headers.items():
            lname = name.lower()
            if lname.startswith('sec-ch-') or lname in (
                    'dpr', 'viewport-width', 'device-memory', 'rtt',
                    'downlink', 'ect', 'width'):
                hints[lname] = _cap(value, 200)
            if lname in _LOG_HEADERS:
                headers[lname] = _cap(value, 300)
        item = {'port': self.server.server_address[1],
                'scheme': self.server.scheme, 'method': self.command,
                'host': _cap(self.headers.get('Host', ''), 120),
                'path': _cap(self.path, 300), 'status': status,
                'headers': headers, 'client_hints': hints,
                'cookie': bool(self.headers.get('Cookie'))}
        if extra:
            item.update(extra)
        self.server.log.write(**item)

    def _send(self, status, body=b'', ctype='text/plain; charset=utf-8',
              extra_headers=()):
        self.send_response(status)
        for k, v in extra_headers:
            self.send_header(k, v)
        if ctype:
            self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        length = int(self.headers.get('Content-Length') or 0)
        if 0 < length <= 65536:
            self.rfile.read(length)
        self.do_GET()

    def do_OPTIONS(self):
        self._log(204)
        self._send(204, extra_headers=[('Access-Control-Allow-Origin', '*'),
                                       ('Access-Control-Allow-Headers', '*')])

    def do_GET(self):
        cfg = self.server.cfg
        path = self.path.split('?', 1)[0]
        parts = [p for p in path.split('/') if p]
        try:
            if path == '/ping':
                self._log(200)
                return self._send(200, b'ok')
            if path == '/favicon.ico':
                self._log(204)
                return self._send(204, ctype=None)
            if path == '/headers':
                self._log(200)
                body = json.dumps({k.lower(): _cap(v, 300) for k, v in
                                   self.headers.items()}).encode()
                return self._send(200, body, 'application/json',
                                  [('Access-Control-Allow-Origin', '*')])
            if len(parts) >= 1 and parts[0] == 'canary':
                self._log(204)
                return self._send(204, ctype=None, extra_headers=[
                    ('Access-Control-Allow-Origin', '*')])
            if len(parts) == 2 and parts[0] == 'to-onion' and \
                    _NONCE_RE.match(parts[1]):
                # A redirect to an onion host (MOLTNET_COMPLIANCE.md §4
                # step 13): a regular window must not follow it anywhere.
                self._log(302)
                return self._send(302, b'', None, [(
                    'Location', 'http://%s/redirected-%s.png' % (
                        cfg.values.get('ONION_A', 'invalid.onion'),
                        parts[1]))])
            if len(parts) == 2 and parts[0] == 'ws':
                return self._websocket(parts[1])
            if len(parts) == 3 and parts[0] == 'dl' and \
                    _NONCE_RE.match(parts[1]) and _NAME_RE.match(parts[2]):
                self._log(200)
                body = b'MZ moltleak download test file\n' + b'\0' * 512
                return self._send(200, body, 'application/octet-stream', [
                    ('Content-Disposition',
                     'attachment; filename="%s"' % parts[2])])
            if len(parts) == 2 and parts[0] == 'static' and \
                    _NAME_RE.match(parts[1]):
                return self._file(os.path.join(cfg.pages_dir, 'static',
                                               parts[1]), None, parts[1])
            if len(parts) == 3 and parts[0] == 't' and \
                    _NONCE_RE.match(parts[1]) and _NAME_RE.match(parts[2]):
                return self._file(os.path.join(cfg.pages_dir, parts[2]),
                                  parts[1], parts[2])
            if path == '/' or path == '/index.html':
                self._log(200)
                return self._send(200, b'<!doctype html><title>moltleak'
                                  b'</title>ok', 'text/html; charset=utf-8')
        except (OSError, ValueError) as e:
            self._log(500, {'error': _cap(e, 200)})
            return self._send(500, b'error')
        self._log(404)
        return self._send(404, b'not found')

    def _file(self, full, nonce, name):
        real = os.path.realpath(full)
        root = os.path.realpath(self.server.cfg.pages_dir)
        if not real.startswith(root + os.sep) or not os.path.isfile(real):
            self._log(404)
            return self._send(404, b'not found')
        with open(real, 'rb') as f:
            data = f.read()
        ctype = 'application/octet-stream'
        if name.endswith('.html'):
            ctype = 'text/html; charset=utf-8'
            if name == 'charset.html':
                ctype = 'text/html'
        elif name.endswith('.js'):
            ctype = 'text/javascript; charset=utf-8'
        elif name.endswith('.json'):
            ctype = 'application/json'
        elif name.endswith('.css'):
            ctype = 'text/css'
        extra = []
        if nonce is not None and name.endswith(('.html', '.js')):
            data = self.server.cfg.render(data.decode('utf-8'),
                                          nonce).encode('utf-8')
            extra = _page_headers(name, nonce, self.server.cfg,
                                  self.server.scheme)
        if name.endswith('.js') and nonce is not None:
            extra.append(('Service-Worker-Allowed', '/t/%s/' % nonce))
        self._log(200)
        return self._send(200, data, ctype, extra)

    def _websocket(self, nonce):
        key = self.headers.get('Sec-WebSocket-Key', '')
        if self.headers.get('Upgrade', '').lower() != 'websocket' or not key:
            self._log(400)
            return self._send(400, b'not a websocket')
        accept = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        self._log(101, {'websocket': True})
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept.decode())
        self.end_headers()
        # A text frame "hello", then a close frame.
        self.wfile.write(b'\x81\x05hello\x88\x02\x03\xe8')
        self.wfile.flush()
        self.close_connection = True


class _Server(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # a client hanging up mid-request is not interesting


class _Server6(_Server):
    address_family = socket.AF_INET6


def run_pages(config_path, log_path, listens):
    """listens: [(scheme, host, port, certfile, keyfile)]"""
    with open(config_path, 'r') as f:
        cfg = PageConfig(json.load(f))
    log = JsonLog(log_path)
    servers = []
    for scheme, host, port, cert, key in listens:
        cls = _Server6 if ':' in host else _Server
        srv = cls((host, port), PageHandler)
        srv.cfg = cfg
        srv.log = log
        srv.scheme = scheme
        if scheme == 'https':
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            ctx.set_alpn_protocols(['http/1.1'])
            srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        servers.append(srv)
    for srv in servers:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    _ready()
    _wait_forever()


# ---- DNS canary -------------------------------------------------------------

def nxdomain_reply(query):
    """An NXDOMAIN answer to `query` (header + question echoed), or None."""
    if len(query) < 12:
        return None
    txid, flags, qdcount = struct.unpack_from('!HHH', query, 0)
    if flags & 0x8000:
        return None
    msg = dissect.parse_dns(query)
    end = 12
    if msg is not None and msg.questions and qdcount == 1:
        _, end = dissect.read_name(query, 12)
        end = (end or 12) + 4
        qd = 1
    else:
        qd = 0
        end = 12
    rflags = 0x8000 | (flags & 0x7900) | 0x0080 | 3  # QR, opcode, RD, RA
    return struct.pack('!HHHHHH', txid, rflags, qd, 0, 0, 0) + query[12:end]


def run_dns(host, port, log_path):
    log = JsonLog(log_path)

    def record(data, client, proto):
        msg = dissect.parse_dns(data)
        if msg is None:
            log.write(proto=proto, client=str(client[0]), malformed=True,
                      server=host)
            return
        for q in msg.questions or [None]:
            log.write(proto=proto, client=str(client[0]), server=host,
                      txid=msg.txid,
                      qname=q.name if q else None,
                      qtype=dissect.qtype_name(q.qtype) if q else None)

    udp = socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET,
                        socket.SOCK_DGRAM)
    udp.bind((host, port))
    tcp = socket.socket(udp.family, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind((host, port))
    tcp.listen(16)

    def serve_udp():
        while True:
            data, client = udp.recvfrom(4096)
            record(data, client, 'udp')
            reply = nxdomain_reply(data)
            if reply:
                udp.sendto(reply, client)

    def serve_tcp_conn(conn, client):
        conn.settimeout(3)
        try:
            while True:
                head = conn.recv(2)
                if len(head) < 2:
                    return
                length = struct.unpack('!H', head)[0]
                data = b''
                while len(data) < length:
                    chunk = conn.recv(length - len(data))
                    if not chunk:
                        return
                    data += chunk
                record(data, client, 'tcp')
                reply = nxdomain_reply(data)
                if reply:
                    conn.sendall(struct.pack('!H', len(reply)) + reply)
        except OSError:
            return
        finally:
            conn.close()

    def serve_tcp():
        while True:
            conn, client = tcp.accept()
            threading.Thread(target=serve_tcp_conn, args=(conn, client),
                             daemon=True).start()

    threading.Thread(target=serve_udp, daemon=True).start()
    threading.Thread(target=serve_tcp, daemon=True).start()
    _ready()
    _wait_forever()


# ---- loopback canary and logging proxy --------------------------------------

def _listen_all(port):
    socks = []
    try:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        s6.bind(('::', port))
        s6.listen(32)
        socks.append(s6)
    except OSError:
        s4 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s4.bind(('0.0.0.0', port))
        s4.listen(32)
        socks.append(s4)
    return socks


def _first_line(conn):
    conn.settimeout(1.5)
    data = b''
    try:
        while b'\n' not in data and len(data) < 2048:
            chunk = conn.recv(1024)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    return data


def run_loopback(port, log_path):
    log = JsonLog(log_path)

    def handle(conn, peer):
        local = conn.getsockname()
        data = _first_line(conn)
        log.write(peer=str(peer[0]), local=str(local[0]), port=port,
                  first_line=_cap(data.split(b'\r\n', 1)[0].decode(
                      'latin-1'), 200))
        try:
            conn.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n'
                         b'Connection: close\r\n\r\n')
        except OSError:
            pass
        conn.close()

    for srv in _listen_all(port):
        threading.Thread(target=_accept_loop, args=(srv, handle),
                         daemon=True).start()
    _ready()
    _wait_forever()


def run_proxy(host, port, log_path):
    log = JsonLog(log_path)

    def handle(conn, peer):
        data = _first_line(conn)
        line = data.split(b'\r\n', 1)[0].decode('latin-1')
        parts = line.split(' ')
        hostname = ''
        for header in data.split(b'\r\n')[1:]:
            if header[:5].lower() == b'host:':
                hostname = header[5:].strip().decode('latin-1')
        log.write(peer=str(peer[0]), method=_cap(parts[0], 20),
                  target=_cap(parts[1] if len(parts) > 1 else '', 300),
                  host=_cap(hostname, 200))
        try:
            conn.sendall(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n'
                         b'Connection: close\r\n\r\n')
        except OSError:
            pass
        conn.close()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(64)
    threading.Thread(target=_accept_loop, args=(srv, handle),
                     daemon=True).start()
    _ready()
    _wait_forever()


def _accept_loop(srv, handle):
    while True:
        try:
            conn, peer = srv.accept()
        except OSError:
            return
        threading.Thread(target=handle, args=(conn, peer), daemon=True).start()


def _ready():
    sys.stdout.write('ready\n')
    sys.stdout.flush()


def _wait_forever():
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
