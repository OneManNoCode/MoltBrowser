# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Application-layer dissectors for the leak analysis.

DNS (also mDNS, LLMNR and DNS over TCP), the TLS ClientHello's server
name, plain HTTP requests, QUIC and STUN detection, IGMP/MLD group
reports and the client side of a SOCKS5 exchange. Every function takes
untrusted bytes and returns None (or an empty result) instead of raising.
Names are returned lower-case, without the trailing dot, with anything
outside printable ASCII escaped as \\xHH so a report can't be garbled.
"""

import hashlib
import ipaddress
import struct

_MAX_POINTER_JUMPS = 32
_MAX_NAME = 255


class DnsQuestion(object):
    __slots__ = ('name', 'qtype', 'qclass')

    def __init__(self, name, qtype, qclass):
        self.name = name
        self.qtype = qtype
        self.qclass = qclass

    def __repr__(self):
        return 'DnsQuestion(%r, %d)' % (self.name, self.qtype)


class DnsMessage(object):
    __slots__ = ('txid', 'is_response', 'opcode', 'rcode', 'questions')

    def __init__(self):
        self.txid = 0
        self.is_response = False
        self.opcode = 0
        self.rcode = 0
        self.questions = []


QTYPES = {1: 'A', 2: 'NS', 5: 'CNAME', 6: 'SOA', 12: 'PTR', 15: 'MX',
          16: 'TXT', 28: 'AAAA', 33: 'SRV', 64: 'SVCB', 65: 'HTTPS',
          255: 'ANY'}


def qtype_name(qtype):
    return QTYPES.get(qtype, str(qtype))


def _escape_label(raw):
    out = []
    for b in raw:
        c = chr(b)
        if 0x21 <= b <= 0x7E and c not in '\\':
            out.append(c.lower())
        else:
            out.append('\\x%02x' % b)
    return ''.join(out)


def read_name(data, offset):
    """Reads a (possibly compressed) domain name. Returns (name, offset
    after the name in the original position) or (None, None)."""
    labels = []
    total = 0
    jumps = 0
    end = None
    pos = offset
    while True:
        if pos >= len(data):
            return None, None
        length = data[pos]
        if length & 0xC0 == 0xC0:
            if pos + 1 >= len(data):
                return None, None
            target = ((length & 0x3F) << 8) | data[pos + 1]
            if end is None:
                end = pos + 2
            jumps += 1
            if jumps > _MAX_POINTER_JUMPS or target >= len(data):
                return None, None
            pos = target
            continue
        if length & 0xC0:
            return None, None  # reserved label types
        pos += 1
        if length == 0:
            break
        if pos + length > len(data):
            return None, None
        total += length + 1
        if total > _MAX_NAME:
            return None, None
        labels.append(_escape_label(data[pos:pos + length]))
        pos += length
    if end is None:
        end = pos
    return '.'.join(labels), end


def parse_dns(data):
    """Parses a DNS (or mDNS / LLMNR) message's header and questions."""
    if len(data) < 12:
        return None
    try:
        txid, flags, qdcount = struct.unpack_from('!HHH', data, 0)
    except struct.error:
        return None
    msg = DnsMessage()
    msg.txid = txid
    msg.is_response = bool(flags & 0x8000)
    msg.opcode = (flags >> 11) & 0xF
    msg.rcode = flags & 0xF
    offset = 12
    for _ in range(min(qdcount, 16)):
        name, offset = read_name(data, offset)
        if name is None or offset + 4 > len(data):
            break
        qtype, qclass = struct.unpack_from('!HH', data, offset)
        offset += 4
        msg.questions.append(DnsQuestion(name, qtype, qclass & 0x7FFF))
    return msg


def parse_dns_tcp(data):
    """Splits a DNS-over-TCP byte stream (2-byte length prefixes)."""
    out = []
    offset = 0
    while offset + 2 <= len(data) and len(out) < 64:
        length = struct.unpack_from('!H', data, offset)[0]
        offset += 2
        if length == 0 or offset + length > len(data):
            break
        msg = parse_dns(data[offset:offset + length])
        if msg is not None:
            out.append(msg)
        offset += length
    return out


def tls_client_hello_sni(data):
    """Server name from a TLS ClientHello at the start of `data`, if any."""
    try:
        if len(data) < 9 or data[0] != 22 or data[5] != 1:
            return None
        pos = 9 + 2 + 32  # record + handshake headers, version, random
        sid_len = data[pos]
        pos += 1 + sid_len
        cs_len = struct.unpack_from('!H', data, pos)[0]
        pos += 2 + cs_len
        cm_len = data[pos]
        pos += 1 + cm_len
        ext_total = struct.unpack_from('!H', data, pos)[0]
        pos += 2
        end = min(len(data), pos + ext_total)
        while pos + 4 <= end:
            ext_type, ext_len = struct.unpack_from('!HH', data, pos)
            pos += 4
            if ext_type == 0 and pos + 5 <= end:
                name_len = struct.unpack_from('!H', data, pos + 3)[0]
                if data[pos + 2] == 0 and pos + 5 + name_len <= end:
                    return _escape_label(data[pos + 5:pos + 5 + name_len])
                return None
            pos += ext_len
    except (IndexError, struct.error):
        return None
    return None


_HTTP_METHODS = (b'GET ', b'POST ', b'HEAD ', b'PUT ', b'OPTIONS ',
                 b'CONNECT ', b'DELETE ', b'PATCH ')


def http_request(data):
    """(method, target, host) of a plain HTTP request at the start of
    `data`, or None."""
    if not data.startswith(_HTTP_METHODS):
        return None
    head = data[:4096].split(b'\r\n\r\n', 1)[0]
    lines = head.split(b'\r\n')
    parts = lines[0].split(b' ')
    if len(parts) < 2:
        return None
    host = None
    for line in lines[1:]:
        if line[:5].lower() == b'host:':
            host = _escape_label(line[5:].strip())
            break
    return (parts[0].decode('ascii', 'replace'),
            _escape_label(parts[1][:512]), host)


def is_quic_long_header(data):
    """A QUIC long-header packet (Initial, 0-RTT, Handshake, Retry)."""
    if len(data) < 7 or not data[0] & 0x80:
        return False
    version = struct.unpack_from('!I', data, 1)[0]
    return version != 0 and (version == 1 or version == 0x6B3343CF or
                             version & 0xFF000000 == 0xFF000000 or
                             version & 0x0F0F0F0F == 0x0A0A0A0A)


def is_stun(data):
    """A STUN message (RFC 5389 magic cookie)."""
    return (len(data) >= 20 and data[0] & 0xC0 == 0 and
            data[4:8] == b'\x21\x12\xa4\x42')


def multicast_groups(proto, icmp_type, payload):
    """Groups an IGMP (v1/v2/v3) or MLD (v1/v2) report joins."""
    groups = []
    try:
        if proto == 2:
            if icmp_type in (0x12, 0x16, 0x17) and len(payload) >= 8:
                groups.append(str(ipaddress.IPv4Address(payload[4:8])))
            elif icmp_type == 0x22 and len(payload) >= 8:
                count = struct.unpack_from('!H', payload, 6)[0]
                pos = 8
                for _ in range(min(count, 64)):
                    aux, nsrc = payload[pos + 1], struct.unpack_from(
                        '!H', payload, pos + 2)[0]
                    groups.append(str(ipaddress.IPv4Address(
                        payload[pos + 4:pos + 8])))
                    pos += 8 + 4 * nsrc + 4 * aux
        elif proto == 58:
            if icmp_type in (131, 132) and len(payload) >= 24:
                groups.append(str(ipaddress.IPv6Address(payload[8:24])))
            elif icmp_type == 143 and len(payload) >= 8:
                count = struct.unpack_from('!H', payload, 6)[0]
                pos = 8
                for _ in range(min(count, 64)):
                    aux, nsrc = payload[pos + 1], struct.unpack_from(
                        '!H', payload, pos + 2)[0]
                    groups.append(str(ipaddress.IPv6Address(
                        payload[pos + 4:pos + 20])))
                    pos += 20 + 16 * nsrc + 4 * aux
    except (IndexError, struct.error, ValueError):
        pass
    return groups


class SocksRequest(object):
    """The client side of a SOCKS5 exchange."""
    __slots__ = ('methods', 'username', 'password_len', 'password_tag',
                 'command', 'host', 'port', 'complete')

    def __init__(self):
        self.methods = []
        self.username = None
        self.password_len = None
        # A digest of the password (never the password itself): tells two
        # passwords apart without putting one in a report.
        self.password_tag = None
        self.command = None
        self.host = None
        self.port = None
        self.complete = False


def parse_socks5_client(data):
    """Parses what a SOCKS5 client sent: greeting, optional RFC 1929
    username/password, and the request. None if it isn't SOCKS5."""
    if len(data) < 3 or data[0] != 5:
        return None
    req = SocksRequest()
    try:
        nmethods = data[1]
        if nmethods == 0 or 2 + nmethods > len(data):
            return None
        req.methods = list(data[2:2 + nmethods])
        pos = 2 + nmethods
        if pos < len(data) and data[pos] == 1 and 2 in req.methods:
            ulen = data[pos + 1]
            req.username = _escape_label(data[pos + 2:pos + 2 + ulen])
            plen = data[pos + 2 + ulen]
            req.password_len = plen
            password = data[pos + 3 + ulen:pos + 3 + ulen + plen]
            req.password_tag = hashlib.sha256(password).hexdigest()[:16]
            pos += 3 + ulen + plen
        if pos + 4 > len(data) or data[pos] != 5:
            return req
        req.command = data[pos + 1]
        atyp = data[pos + 3]
        pos += 4
        if atyp == 1:
            req.host = str(ipaddress.IPv4Address(data[pos:pos + 4]))
            pos += 4
        elif atyp == 3:
            hlen = data[pos]
            req.host = _escape_label(data[pos + 1:pos + 1 + hlen])
            pos += 1 + hlen
        elif atyp == 4:
            req.host = str(ipaddress.IPv6Address(data[pos:pos + 16]))
            pos += 16
        else:
            return req
        if pos + 2 <= len(data):
            req.port = struct.unpack_from('!H', data, pos)[0]
            req.complete = True
    except (IndexError, struct.error, ValueError):
        return req
    return req
