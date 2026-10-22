# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Builds packets and capture files for the kit's tests (and the sample
run in testdata/). Only what the tests need; checksums are left zero."""

import ipaddress
import struct


def ip4(src, dst, proto, payload, ttl=64):
    total = 20 + len(payload)
    return struct.pack('!BBHHHBBH4s4s', 0x45, 0, total, 1, 0, ttl, proto, 0,
                       ipaddress.IPv4Address(src).packed,
                       ipaddress.IPv4Address(dst).packed) + payload


def ip6(src, dst, nxt, payload, ext=b''):
    return struct.pack('!IHBB16s16s', 6 << 28, len(ext) + len(payload),
                       nxt if not ext else 0, 64,
                       ipaddress.IPv6Address(src).packed,
                       ipaddress.IPv6Address(dst).packed) + ext + payload


def ip(src, dst, proto, payload):
    if ':' in src:
        return ip6(src, dst, proto, payload)
    return ip4(src, dst, proto, payload)


def udp(sport, dport, payload):
    return struct.pack('!HHHH', sport, dport, 8 + len(payload), 0) + payload


def tcp(sport, dport, payload=b'', seq=1000, flags=0x18):
    return struct.pack('!HHIIBBHHH', sport, dport, seq, 0, 5 << 4, flags,
                       65535, 0, 0) + payload


def dns_name(name):
    out = b''
    for label in name.rstrip('.').split('.'):
        out += bytes([len(label)]) + label.encode('ascii')
    return out + b'\0'


def dns_query(name, qtype=1, txid=0x1234):
    return struct.pack('!HHHHHH', txid, 0x0100, 1, 0, 0, 0) + \
        dns_name(name) + struct.pack('!HH', qtype, 1)


def client_hello(sni):
    name = sni.encode('ascii')
    sni_ext = struct.pack('!HHHBH', 0, len(name) + 5, len(name) + 3, 0,
                          len(name)) + name
    body = b'\x03\x03' + b'\x11' * 32 + b'\x00' + b'\x00\x02\x13\x01' + \
        b'\x01\x00' + struct.pack('!H', len(sni_ext)) + sni_ext
    hs = b'\x01' + struct.pack('!I', len(body))[1:] + body
    return b'\x16\x03\x01' + struct.pack('!H', len(hs)) + hs


def socks5_client(host, port, username=None, password=b'x'):
    if username is None:
        data = b'\x05\x01\x00'
    else:
        u = username.encode('ascii')
        data = b'\x05\x02\x00\x02' + b'\x01' + bytes([len(u)]) + u + \
            bytes([len(password)]) + password
    h = host.encode('ascii')
    return data + b'\x05\x01\x00\x03' + bytes([len(h)]) + h + \
        struct.pack('!H', port)


def sll2(packet, ifindex=2, pkttype=4, ethertype=None):
    if ethertype is None:
        ethertype = 0x86DD if packet[0] >> 4 == 6 else 0x0800
    return struct.pack('!HHIHBB8s', ethertype, 0, ifindex, 1, pkttype, 6,
                       b'\0' * 8) + packet


def sll(packet, pkttype=4):
    ethertype = 0x86DD if packet[0] >> 4 == 6 else 0x0800
    return struct.pack('!HHH8sH', pkttype, 1, 6, b'\0' * 8, ethertype) + packet


def ethernet(packet):
    ethertype = 0x86DD if packet[0] >> 4 == 6 else 0x0800
    return b'\x02' * 6 + b'\x04' * 6 + struct.pack('!H', ethertype) + packet


def nflog(packet, prefix, endian='<'):
    family = 10 if packet[0] >> 4 == 6 else 2
    out = bytes([family, 0]) + b'\x00\x01'

    def tlv(t, value):
        raw = struct.pack(endian + 'HH', 4 + len(value), t) + value
        return raw + b'\0' * ((4 - len(raw) % 4) % 4)
    out += tlv(1, struct.pack('!HBB', 0x0800, 3, 0))
    out += tlv(10, prefix.encode('ascii') + b'\0')
    out += tlv(5, struct.pack('!I', 2))
    out += tlv(9, packet)
    return out


def pcap(frames, linktype, endian='<', nanos=False):
    magic = 0xA1B23C4D if nanos else 0xA1B2C3D4
    out = struct.pack(endian + 'IHHiIII', magic, 2, 4, 0, 0, 262144, linktype)
    for ts, frame in frames:
        sec = int(ts)
        frac = int(round((ts - sec) * (1e9 if nanos else 1e6)))
        out += struct.pack(endian + 'IIII', sec, frac, len(frame),
                           len(frame)) + frame
    return out


def pcapng(frames, linktype, endian='<', tsresol=6):
    def block(btype, body):
        pad = b'\0' * ((4 - len(body) % 4) % 4)
        length = 12 + len(body) + len(pad)
        return struct.pack(endian + 'II', btype, length) + body + pad + \
            struct.pack(endian + 'I', length)
    shb = block(0x0A0D0D0A, struct.pack(endian + 'IHHq', 0x1A2B3C4D, 1, 0,
                                        -1))
    opts = struct.pack(endian + 'HH', 9, 1) + bytes([tsresol]) + b'\0' * 3 + \
        struct.pack(endian + 'HH', 0, 0)
    idb = block(1, struct.pack(endian + 'HHI', linktype, 0, 262144) + opts)
    out = shb + idb
    for ts, frame in frames:
        units = int(round(ts * (10 ** tsresol)))
        out += block(6, struct.pack(endian + 'IIIII', 0, units >> 32,
                                    units & 0xFFFFFFFF, len(frame),
                                    len(frame)) + frame)
    return out
