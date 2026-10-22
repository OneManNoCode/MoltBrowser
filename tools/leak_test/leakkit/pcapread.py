# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A small, defensive pcap / pcapng reader.

Reads what the kit's captures contain (tcpdump on `any`, on `nflog:N`, or a
capture from another tool): pcap (micro- and nanosecond, either byte order)
and pcapng (any number of sections and interfaces), with the link types
Ethernet, Linux cooked v1/v2, raw IPv4/IPv6, BSD loopback and NFLOG. Each
frame becomes a Packet with its IP and TCP/UDP fields decoded.

Capture files are treated as untrusted input: a truncated or malformed
frame is counted in Stats.malformed and skipped, never raised.
"""

import ipaddress
import struct

LINKTYPE_NULL = 0
LINKTYPE_ETHERNET = 1
LINKTYPE_RAW_OLD = 12
LINKTYPE_RAW_OLD2 = 14
LINKTYPE_RAW = 101
LINKTYPE_LOOP = 108
LINKTYPE_LINUX_SLL = 113
LINKTYPE_IPV4 = 228
LINKTYPE_IPV6 = 229
LINKTYPE_NFLOG = 239
LINKTYPE_LINUX_SLL2 = 276

# Linux packet types (struct sockaddr_ll.sll_pkttype).
PACKET_HOST = 0
PACKET_BROADCAST = 1
PACKET_MULTICAST = 2
PACKET_OTHERHOST = 3
PACKET_OUTGOING = 4

PROTO_ICMP = 1
PROTO_IGMP = 2
PROTO_TCP = 6
PROTO_UDP = 17
PROTO_ICMPV6 = 58

TCP_FIN = 0x01
TCP_SYN = 0x02
TCP_RST = 0x04
TCP_PSH = 0x08
TCP_ACK = 0x10

# NFLOG TLV types (linux/netfilter/nfnetlink_log.h).
NFULA_PACKET_HDR = 1
NFULA_IFINDEX_INDEV = 4
NFULA_IFINDEX_OUTDEV = 5
NFULA_PAYLOAD = 9
NFULA_PREFIX = 10
NFULA_UID = 11

_IPV6_EXT_HEADERS = (0, 43, 60)  # hop-by-hop, routing, destination options
_IPV6_FRAGMENT = 44
_MAX_FRAME = 1 << 20  # larger records are malformed for our purposes


class Stats(object):
    """What the reader saw, for the report's evidence."""

    def __init__(self):
        self.frames = 0
        self.decoded = 0
        self.non_ip = 0
        self.malformed = 0
        self.linktypes = set()
        self.truncated_file = False

    def as_dict(self):
        return {
            'frames': self.frames,
            'decoded': self.decoded,
            'non_ip': self.non_ip,
            'malformed': self.malformed,
            'linktypes': sorted(self.linktypes),
            'truncated_file': self.truncated_file,
        }


class Packet(object):
    """One decoded IP packet."""

    __slots__ = ('ts', 'linktype', 'ifindex', 'pkttype', 'prefix', 'uid',
                 'version', 'src', 'dst', 'proto', 'sport', 'dport',
                 'tcp_flags', 'seq', 'payload', 'ip_len', 'icmp_type')

    def __init__(self):
        self.ts = 0.0
        self.linktype = None
        self.ifindex = None    # Linux cooked v2 / NFLOG out device
        self.pkttype = None    # PACKET_* when the link type says
        self.prefix = None     # NFLOG log prefix
        self.uid = None        # NFLOG socket uid, if logged
        self.version = 0
        self.src = None        # ipaddress object
        self.dst = None
        self.proto = None
        self.sport = None
        self.dport = None
        self.tcp_flags = 0
        self.seq = 0
        self.payload = b''
        self.ip_len = 0
        self.icmp_type = None

    @property
    def is_syn(self):
        return (self.proto == PROTO_TCP and bool(self.tcp_flags & TCP_SYN) and
                not self.tcp_flags & TCP_ACK)

    def flow_key(self):
        return (self.proto, str(self.src), self.sport, str(self.dst),
                self.dport)

    def __repr__(self):
        return 'Packet(%.6f %s %s:%s -> %s:%s len=%d prefix=%r)' % (
            self.ts, self.proto, self.src, self.sport, self.dst, self.dport,
            len(self.payload), self.prefix)


class _Malformed(Exception):
    pass


def _need(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise _Malformed('short frame')


def decode_ip(data, packet):
    """Fills `packet` from an IPv4 or IPv6 packet at the start of `data`."""
    _need(data, 0, 1)
    version = data[0] >> 4
    if version == 4:
        _need(data, 0, 20)
        ihl = (data[0] & 0x0F) * 4
        if ihl < 20:
            raise _Malformed('bad IHL')
        _need(data, 0, ihl)
        total = struct.unpack_from('!H', data, 2)[0]
        frag = struct.unpack_from('!H', data, 6)[0]
        proto = data[9]
        packet.src = ipaddress.IPv4Address(data[12:16])
        packet.dst = ipaddress.IPv4Address(data[16:20])
        end = min(len(data), total) if total >= ihl else len(data)
        packet.ip_len = total
        body = data[ihl:end]
        if frag & 0x1FFF:
            # A later fragment: no transport header.
            packet.version = 4
            packet.proto = proto
            packet.payload = bytes(body)
            return
    elif version == 6:
        _need(data, 0, 40)
        plen = struct.unpack_from('!H', data, 4)[0]
        proto = data[6]
        packet.src = ipaddress.IPv6Address(data[8:24])
        packet.dst = ipaddress.IPv6Address(data[24:40])
        packet.ip_len = plen + 40
        end = min(len(data), 40 + plen) if plen else len(data)
        body = data[40:end]
        hops = 0
        while proto in _IPV6_EXT_HEADERS or proto == _IPV6_FRAGMENT:
            hops += 1
            if hops > 8:
                raise _Malformed('too many extension headers')
            _need(body, 0, 8)
            nxt = body[0]
            if proto == _IPV6_FRAGMENT:
                offset = struct.unpack_from('!H', body, 2)[0] >> 3
                body = body[8:]
                proto = nxt
                if offset:
                    packet.version = 6
                    packet.proto = proto
                    packet.payload = bytes(body)
                    return
                continue
            hlen = (body[1] + 1) * 8
            _need(body, 0, hlen)
            body = body[hlen:]
            proto = nxt
    else:
        raise _Malformed('not IP')
    packet.version = version
    packet.proto = proto
    if proto == PROTO_TCP:
        _need(body, 0, 20)
        packet.sport, packet.dport, packet.seq = struct.unpack_from(
            '!HHI', body, 0)
        offset = (body[12] >> 4) * 4
        if offset < 20:
            raise _Malformed('bad TCP offset')
        packet.tcp_flags = body[13]
        packet.payload = bytes(body[offset:]) if offset <= len(body) else b''
    elif proto == PROTO_UDP:
        _need(body, 0, 8)
        packet.sport, packet.dport, ulen = struct.unpack_from('!HHH', body, 0)
        end = ulen if 8 <= ulen <= len(body) else len(body)
        packet.payload = bytes(body[8:end])
    elif proto in (PROTO_ICMP, PROTO_ICMPV6, PROTO_IGMP):
        _need(body, 0, 1)
        packet.icmp_type = body[0]
        packet.payload = bytes(body)
    else:
        packet.payload = bytes(body)


def _decode_nflog(data, packet, endian):
    _need(data, 0, 4)
    family = data[0]
    offset = 4
    payload = None
    while offset + 4 <= len(data):
        tlv_len, tlv_type = struct.unpack_from(endian + 'HH', data, offset)
        if tlv_len < 4:
            raise _Malformed('bad NFLOG TLV')
        _need(data, offset, tlv_len)
        value = data[offset + 4:offset + tlv_len]
        tlv_type &= 0x7FFF  # NLA_F_NESTED / byte-order bits
        if tlv_type == NFULA_PAYLOAD:
            payload = value
        elif tlv_type == NFULA_PREFIX:
            packet.prefix = bytes(value).split(b'\0', 1)[0].decode(
                'ascii', 'replace')
        elif tlv_type == NFULA_IFINDEX_OUTDEV and len(value) >= 4:
            packet.ifindex = struct.unpack_from('!I', value, 0)[0]
        elif tlv_type == NFULA_UID and len(value) >= 4:
            packet.uid = struct.unpack_from('!I', value, 0)[0]
        offset += (tlv_len + 3) & ~3
    if payload is None:
        raise _Malformed('NFLOG without payload')
    if family not in (2, 10):
        raise _Malformed('NFLOG family %d' % family)
    packet.pkttype = PACKET_OUTGOING
    decode_ip(payload, packet)


def _decode_ethernet(data, packet):
    _need(data, 0, 14)
    ethertype = struct.unpack_from('!H', data, 12)[0]
    offset = 14
    while ethertype in (0x8100, 0x88A8):
        _need(data, offset, 4)
        ethertype = struct.unpack_from('!H', data, offset + 2)[0]
        offset += 4
    return ethertype, data[offset:]


def decode_frame(ts, linktype, data, endian='<'):
    """Decodes one frame. Returns a Packet, None for non-IP frames, or
    raises _Malformed."""
    packet = Packet()
    packet.ts = ts
    packet.linktype = linktype
    if linktype == LINKTYPE_NFLOG:
        _decode_nflog(data, packet, endian)
        return packet
    if linktype == LINKTYPE_ETHERNET:
        ethertype, body = _decode_ethernet(data, packet)
        if ethertype not in (0x0800, 0x86DD):
            return None
        decode_ip(body, packet)
        return packet
    if linktype == LINKTYPE_LINUX_SLL:
        _need(data, 0, 16)
        packet.pkttype = struct.unpack_from('!H', data, 0)[0]
        ethertype = struct.unpack_from('!H', data, 14)[0]
        if ethertype not in (0x0800, 0x86DD):
            return None
        decode_ip(data[16:], packet)
        return packet
    if linktype == LINKTYPE_LINUX_SLL2:
        _need(data, 0, 20)
        ethertype = struct.unpack_from('!H', data, 0)[0]
        packet.ifindex = struct.unpack_from('!I', data, 4)[0]
        packet.pkttype = data[10]
        if ethertype not in (0x0800, 0x86DD):
            return None
        decode_ip(data[20:], packet)
        return packet
    if linktype in (LINKTYPE_RAW, LINKTYPE_RAW_OLD, LINKTYPE_RAW_OLD2,
                    LINKTYPE_IPV4, LINKTYPE_IPV6):
        decode_ip(data, packet)
        return packet
    if linktype in (LINKTYPE_NULL, LINKTYPE_LOOP):
        _need(data, 0, 4)
        family_le = struct.unpack_from('<I', data, 0)[0]
        family_be = struct.unpack_from('>I', data, 0)[0]
        family = family_le if family_le < 256 else family_be
        if family not in (2, 10, 24, 28, 30):
            return None
        decode_ip(data[4:], packet)
        return packet
    raise _Malformed('unsupported link type %d' % linktype)


# ---- file formats ----------------------------------------------------------

_PCAP_MAGICS = {
    b'\xd4\xc3\xb2\xa1': ('<', 1e-6),
    b'\xa1\xb2\xc3\xd4': ('>', 1e-6),
    b'\x4d\x3c\xb2\xa1': ('<', 1e-9),
    b'\xa1\xb2\x3c\x4d': ('>', 1e-9),
}
_PCAPNG_SHB = 0x0A0D0D0A


def _iter_pcap(data, stats, endian, scale):
    if len(data) < 24:
        stats.truncated_file = True
        return
    linktype = struct.unpack_from(endian + 'I', data, 20)[0] & 0xFFFF
    offset = 24
    while offset < len(data):
        if offset + 16 > len(data):
            stats.truncated_file = True
            return
        sec, frac, caplen, _ = struct.unpack_from(endian + 'IIII', data,
                                                  offset)
        offset += 16
        if caplen > _MAX_FRAME or offset + caplen > len(data):
            stats.truncated_file = True
            return
        yield sec + frac * scale, linktype, data[offset:offset + caplen], endian
        offset += caplen


def _pcapng_tsresol(options, endian):
    """if_tsresol of an Interface Description Block; default microseconds."""
    offset = 0
    while offset + 4 <= len(options):
        code, length = struct.unpack_from(endian + 'HH', options, offset)
        if code == 0:
            break
        value = options[offset + 4:offset + 4 + length]
        if code == 9 and len(value) >= 1:
            raw = value[0]
            if raw & 0x80:
                return 2.0 ** -(raw & 0x7F)
            return 10.0 ** -raw
        offset += 4 + ((length + 3) & ~3)
    return 1e-6


def _iter_pcapng(data, stats):
    offset = 0
    endian = '<'
    interfaces = []
    while offset + 12 <= len(data):
        block_type_le = struct.unpack_from('<I', data, offset)[0]
        if block_type_le == _PCAPNG_SHB:
            bom = data[offset + 8:offset + 12]
            if bom == b'\x4d\x3c\x2b\x1a':
                endian = '<'
            elif bom == b'\x1a\x2b\x3c\x4d':
                endian = '>'
            else:
                stats.truncated_file = True
                return
            interfaces = []
        block_type, block_len = struct.unpack_from(endian + 'II', data, offset)
        if block_len < 12 or block_len % 4 or offset + block_len > len(data):
            stats.truncated_file = True
            return
        body = data[offset + 8:offset + block_len - 4]
        if block_type == 1 and len(body) >= 8:  # Interface Description
            linktype = struct.unpack_from(endian + 'H', body, 0)[0]
            interfaces.append((linktype, _pcapng_tsresol(body[8:], endian)))
        elif block_type == 6 and len(body) >= 20:  # Enhanced Packet
            ifid, ts_hi, ts_lo, caplen = struct.unpack_from(
                endian + 'IIII', body, 0)
            if ifid < len(interfaces) and 20 + caplen <= len(body):
                linktype, resol = interfaces[ifid]
                yield (((ts_hi << 32) | ts_lo) * resol, linktype,
                       body[20:20 + caplen], endian)
            else:
                stats.malformed += 1
        elif block_type == 3 and len(body) >= 4 and interfaces:  # Simple
            linktype, _ = interfaces[0]
            yield 0.0, linktype, body[4:], endian
        elif block_type == 2 and len(body) >= 20:  # obsolete Packet Block
            ifid = struct.unpack_from(endian + 'H', body, 0)[0]
            ts_hi, ts_lo, caplen = struct.unpack_from(endian + 'III', body, 4)
            if ifid < len(interfaces) and 20 + caplen <= len(body):
                linktype, resol = interfaces[ifid]
                yield (((ts_hi << 32) | ts_lo) * resol, linktype,
                       body[20:20 + caplen], endian)
        offset += block_len


def iter_frames(data, stats):
    """Yields (ts, linktype, frame bytes, byte order) from a capture file's
    bytes."""
    magic = data[:4]
    if magic in _PCAP_MAGICS:
        endian, scale = _PCAP_MAGICS[magic]
        for frame in _iter_pcap(data, stats, endian, scale):
            yield frame
    elif len(data) >= 4 and struct.unpack_from('<I', data, 0)[0] == _PCAPNG_SHB:
        for frame in _iter_pcapng(data, stats):
            yield frame
    elif data:
        stats.truncated_file = True


def read_bytes(data):
    """Decodes every frame of a capture held in memory. Returns
    (packets, stats)."""
    stats = Stats()
    packets = []
    for ts, linktype, frame, endian in iter_frames(memoryview(data).tobytes(),
                                                   stats):
        stats.frames += 1
        stats.linktypes.add(linktype)
        try:
            packet = decode_frame(ts, linktype, frame, endian)
        except (_Malformed, struct.error, ValueError, IndexError):
            stats.malformed += 1
            continue
        if packet is None:
            stats.non_ip += 1
            continue
        stats.decoded += 1
        packets.append(packet)
    return packets, stats


def read_file(path):
    """Reads a capture file; a missing or empty file reads as no packets."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except (IOError, OSError):
        stats = Stats()
        stats.truncated_file = True
        return [], stats
    return read_bytes(data)
