# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The traffic capture's resolver and sinkhole (docs/LEAK_TESTING.md,
"Regular-window traffic capture"). Each runs as its own process, as the
invoking user, inside the test namespace (`leaktest.py service fakedns` /
`service sinkhole`).

  fakedns   the namespace's only resolver. Every A query gets an address
            of its own from SINKHOLE_NET (198.18.0.0/15, the benchmarking
            range, routed to the namespace's loopback), so a connection's
            destination address names the host it was for. AAAA, HTTPS
            and every other type get an empty answer; names under
            NX_SUFFIXES (.invalid) get NXDOMAIN, so a page can still meet
            a name that doesn't resolve. Each query is logged with its
            answer.
  sinkhole  listens on the sinkhole's TCP ports (any address of the range
            reaches it). TLS is accepted with the kit's test key (the
            browser trusts it through --ignore-certificate-errors-spki-
            list), so the log has the server name, the HTTP method, path
            and a few headers; plain HTTP likewise. Every request gets a
            503 and the connection is closed: nothing goes further, and
            nothing reaches the real host.

Logs are JSON lines with a wall-clock `ts`. Everything a client sends is
length-capped before it is logged; bodies are read and dropped, never
logged.
"""

import ipaddress
import socket
import ssl
import struct
import threading

from . import dissect
from .services import JsonLog, _cap, _ready, _wait_forever

SINKHOLE_NET = '198.18.0.0/15'
NX_SUFFIXES = ('.invalid',)
TTL = 30
_LOG_HEADERS = ('host', 'user-agent', 'content-type', 'content-length',
                'x-goog-api-key', 'x-goog-update-appid',
                'x-goog-update-interactivity', 'x-goog-update-updater',
                'x-client-data', 'cookie', 'authorization')
_REDACT = ('x-goog-api-key', 'cookie', 'authorization')
_MAX_HEAD = 16384
_MAX_BODY = 1 << 20


class AddressPool(object):
    """One address per name, in order, from the sinkhole range (thread-
    safe). The network and broadcast addresses and .0/.255 hosts are
    skipped so that no address looks special."""

    def __init__(self, net=SINKHOLE_NET, first=10):
        self.net = ipaddress.ip_network(net)
        self.next = int(self.net.network_address) + first
        self.by_name = {}
        self.lock = threading.Lock()

    def address(self, name):
        with self.lock:
            ip = self.by_name.get(name)
            if ip is not None:
                return ip
            while True:
                candidate = ipaddress.ip_address(self.next)
                self.next += 1
                if candidate not in self.net or \
                        candidate == self.net.broadcast_address:
                    raise RuntimeError('sinkhole address pool exhausted')
                if candidate.packed[-1] not in (0, 255):
                    break
            self.by_name[name] = candidate
            return candidate


def _question_end(query):
    """End offset of the (single) question, or None."""
    name, end = dissect.read_name(query, 12)
    if name is None or end is None or end + 4 > len(query):
        return None
    return end + 4


def dns_reply(query, pool, nx_suffixes=NX_SUFFIXES):
    """(reply bytes or None, log fields) for one query."""
    if len(query) < 12:
        return None, {'malformed': True}
    txid, flags, qdcount = struct.unpack_from('!HHH', query, 0)
    if flags & 0x8000:
        return None, {'malformed': True, 'note': 'a response, not a query'}
    msg = dissect.parse_dns(query)
    end = _question_end(query) if qdcount == 1 else None
    if msg is None or not msg.questions or end is None:
        # FORMERR, header only.
        rflags = 0x8000 | (flags & 0x7900) | 0x0080 | 1
        return struct.pack('!HHHHHH', txid, rflags, 0, 0, 0, 0), {
            'malformed': True}
    q = msg.questions[0]
    name = (q.name or '').lower().rstrip('.')
    fields = {'qname': q.name, 'qtype': dissect.qtype_name(q.qtype)}
    question = query[12:end]
    base = 0x8000 | (flags & 0x7900) | 0x0400 | 0x0080   # QR, AA, RA
    if name == 'invalid' or any(name.endswith(s) for s in nx_suffixes):
        fields['answer'] = 'NXDOMAIN'
        return struct.pack('!HHHHHH', txid, base | 3, 1, 0, 0, 0) + \
            question, fields
    if q.qtype == 1 and q.qclass in (1, 255):
        ip = pool.address(name)
        fields['answer'] = str(ip)
        answer = struct.pack('!HHHIH', 0xC00C, 1, 1, TTL, 4) + ip.packed
        return struct.pack('!HHHHHH', txid, base, 1, 1, 0, 0) + question + \
            answer, fields
    fields['answer'] = 'NODATA'
    return struct.pack('!HHHHHH', txid, base, 1, 0, 0, 0) + question, fields


def run_fakedns(host, port, log_path, net=SINKHOLE_NET):
    log = JsonLog(log_path)
    pool = AddressPool(net)

    def answer(data, client, proto):
        reply, fields = dns_reply(data, pool)
        log.write(proto=proto, client=str(client[0]), server=host, **fields)
        return reply

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
            reply = answer(data, client, 'udp')
            if reply:
                udp.sendto(reply, client)

    def serve_tcp_conn(conn, client):
        conn.settimeout(3)
        try:
            while True:
                head = _recv_exact(conn, 2)
                if head is None:
                    return
                data = _recv_exact(conn, struct.unpack('!H', head)[0])
                if data is None:
                    return
                reply = answer(data, client, 'tcp')
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


def _recv_exact(conn, n):
    data = b''
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def parse_head(data):
    """(method, target, {lower header: value}) of an HTTP request head, or
    None. `data` ends at (or contains) the blank line."""
    head = data.split(b'\r\n\r\n', 1)[0]
    lines = head.split(b'\r\n')
    parts = lines[0].split(b' ')
    if len(parts) < 3 or not parts[2].startswith(b'HTTP/'):
        return None
    headers = {}
    for line in lines[1:]:
        k, sep, v = line.partition(b':')
        if not sep:
            continue
        headers[k.strip().lower().decode('latin-1')] = v.strip().decode(
            'latin-1')
    return (parts[0].decode('latin-1'), parts[1].decode('latin-1'),
            headers)


def logged_headers(headers):
    out = {}
    for name in _LOG_HEADERS:
        if name in headers:
            value = headers[name]
            if name in _REDACT:
                value = '(%d bytes, not logged)' % len(value)
            out[name] = _cap(value, 200)
    return out


def _read_head(stream):
    data = b''
    while b'\r\n\r\n' not in data and len(data) < _MAX_HEAD:
        chunk = stream.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def _drain_body(stream, data, headers):
    try:
        length = int(headers.get('content-length') or 0)
    except ValueError:
        length = 0
    have = len(data.split(b'\r\n\r\n', 1)[1]) if b'\r\n\r\n' in data else 0
    left = min(max(length - have, 0), _MAX_BODY)
    while left > 0:
        chunk = stream.recv(min(left, 65536))
        if not chunk:
            break
        left -= len(chunk)
    return length


_RESPONSE = (b'HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n'
             b'Cache-Control: no-store\r\nConnection: close\r\n\r\n')


def handle_connection(conn, ctx, log, port):
    """Serves one sinkhole connection: logs who it was for and what it
    asked, answers 503, closes."""
    item = {'port': port}
    try:
        local = conn.getsockname()
        peer = conn.getpeername()
        item.update(dst=str(local[0]), peer_port=peer[1])
    except OSError:
        pass
    conn.settimeout(5)
    stream = conn
    try:
        first = conn.recv(4096, socket.MSG_PEEK)
        if first[:1] == b'\x16':
            item['tls'] = True
            sni = dissect.tls_client_hello_sni(first)
            if sni:
                item['sni'] = _cap(sni, 255)
            if ctx is None:
                item['error'] = 'TLS not terminated'
                return
            state = {}

            def on_sni(sock, name, _ctx):
                state['sni'] = name
            ctx.sni_callback = on_sni
            stream = ctx.wrap_socket(conn, server_side=True)
            if state.get('sni') and not item.get('sni'):
                item['sni'] = _cap(state['sni'], 255)
            item['alpn'] = stream.selected_alpn_protocol()
        else:
            item['tls'] = False
        data = _read_head(stream)
        req = parse_head(data)
        if req is None:
            item['error'] = 'not an HTTP request'
            item['first_bytes'] = len(data)
            return
        method, target, headers = req
        item.update(method=_cap(method, 16), path=_cap(target, 300),
                    headers=logged_headers(headers))
        item['body_length'] = _drain_body(stream, data, headers)
        stream.sendall(_RESPONSE)
    except (ssl.SSLError, OSError, ValueError) as e:
        item['error'] = _cap(str(e), 200)
    finally:
        log.write(**item)
        try:
            stream.close()
        except OSError:
            pass


def server_context(cert, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    ctx.set_alpn_protocols(['http/1.1'])
    return ctx


def run_sinkhole(log_path, ports, cert=None, key=None, host='0.0.0.0'):
    log = JsonLog(log_path)
    for port in ports:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(128)

        def loop(srv=srv, port=port):
            while True:
                try:
                    conn, _ = srv.accept()
                except OSError:
                    return
                # A context per connection: the SNI callback writes to it.
                ctx = server_context(cert, key) if cert and key else None
                threading.Thread(target=handle_connection,
                                 args=(conn, ctx, log, port),
                                 daemon=True).start()
        threading.Thread(target=loop, daemon=True).start()
    _ready()
    _wait_forever()
