#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the capture reader and the protocol dissectors.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import os
import random
import struct
import unittest

from leakkit import dissect
from leakkit import pcapread
from tests import pcapgen as g

HERE = os.path.dirname(os.path.abspath(__file__))


def dns_packet(name='a.example', src='10.0.0.2', dst='127.0.0.1'):
    return g.ip(src, dst, 17, g.udp(40000, 53, g.dns_query(name)))


class PcapFormats(unittest.TestCase):

    def check_one(self, data, linktype=None):
        packets, stats = pcapread.read_bytes(data)
        self.assertEqual(stats.malformed, 0, stats.as_dict())
        self.assertEqual(len(packets), 1, stats.as_dict())
        p = packets[0]
        self.assertEqual(str(p.dst), '127.0.0.1')
        self.assertEqual(p.dport, 53)
        self.assertEqual(dissect.parse_dns(p.payload).questions[0].name,
                         'a.example')
        if linktype is not None:
            self.assertEqual(p.linktype, linktype)
        return p

    def test_pcap_both_byte_orders_and_resolutions(self):
        for endian in '<>':
            for nanos in (False, True):
                data = g.pcap([(1700000000.25, dns_packet())], 101, endian,
                              nanos)
                p = self.check_one(data, 101)
                self.assertAlmostEqual(p.ts, 1700000000.25, places=5)

    def test_pcapng_resolutions(self):
        for tsresol in (6, 9):
            data = g.pcapng([(1700000000.5, g.sll2(dns_packet()))], 276,
                            tsresol=tsresol)
            p = self.check_one(data, 276)
            self.assertAlmostEqual(p.ts, 1700000000.5, places=5)
            self.assertEqual(p.ifindex, 2)
            self.assertEqual(p.pkttype, pcapread.PACKET_OUTGOING)

    def test_link_types(self):
        pkt = dns_packet()
        self.check_one(g.pcap([(1, g.ethernet(pkt))], 1))
        self.check_one(g.pcap([(1, g.sll(pkt))], 113))
        self.check_one(g.pcap([(1, g.sll2(pkt))], 276))
        self.check_one(g.pcap([(1, struct.pack('<I', 2) + pkt)], 0))
        self.check_one(g.pcap([(1, pkt)], 228))

    def test_nflog_prefix(self):
        for endian in '<>':
            data = g.pcap([(1, g.nflog(dns_packet(), 'dns-kit', endian))],
                          239, endian)
            p = self.check_one(data, 239)
            self.assertEqual(p.prefix, 'dns-kit')
            self.assertEqual(p.ifindex, 2)

    def test_ipv6_extension_headers(self):
        payload = g.udp(5353, 5353, g.dns_query('x.local'))
        # hop-by-hop header whose next header is UDP
        ext = bytes([17, 0]) + b'\0' * 6
        pkt = g.ip6('fe80::1', 'ff02::fb', 17, payload, ext=ext)
        packets, stats = pcapread.read_bytes(g.pcap([(1, pkt)], 101))
        self.assertEqual(stats.malformed, 0)
        self.assertEqual(packets[0].proto, 17)
        self.assertEqual(packets[0].dport, 5353)

    def test_tcp_fields(self):
        pkt = g.ip('10.0.0.2', '1.2.3.4', 6,
                   g.tcp(5555, 443, b'', seq=7, flags=0x02))
        packets, _ = pcapread.read_bytes(g.pcap([(1, pkt)], 101))
        self.assertTrue(packets[0].is_syn)
        self.assertEqual(packets[0].seq, 7)

    def test_non_ip_is_counted_not_decoded(self):
        arp = b'\xff' * 6 + b'\x02' * 6 + b'\x08\x06' + b'\0' * 28
        packets, stats = pcapread.read_bytes(g.pcap([(1, arp)], 1))
        self.assertEqual(packets, [])
        self.assertEqual(stats.non_ip, 1)

    def test_truncated_and_garbage(self):
        good = g.pcap([(1, dns_packet()), (2, dns_packet())], 101)
        packets, stats = pcapread.read_bytes(good[:-5])
        self.assertEqual(len(packets), 1)
        self.assertTrue(stats.truncated_file)
        _, stats = pcapread.read_bytes(b'not a capture at all')
        self.assertTrue(stats.truncated_file)
        packets, stats = pcapread.read_bytes(b'')
        self.assertEqual(packets, [])

    def test_fuzz_never_raises(self):
        rng = random.Random(1234)
        base = g.pcap([(1, g.sll2(dns_packet())), (2, g.nflog(
            g.ip('10.0.0.2', '8.8.8.8', 6, g.tcp(1, 2, b'x' * 40)), 'drop'))],
            276)
        ng = g.pcapng([(1, g.sll2(dns_packet()))], 276)
        for seed_data in (base, ng):
            for _ in range(400):
                data = bytearray(seed_data)
                for _ in range(rng.randint(1, 12)):
                    data[rng.randrange(len(data))] = rng.randrange(256)
                pcapread.read_bytes(bytes(data))  # must not raise
        for _ in range(200):
            n = rng.randint(0, 200)
            junk = bytes(rng.randrange(256) for _ in range(n))
            for lt in (0, 1, 101, 113, 239, 276):
                try:
                    pcapread.decode_frame(0, lt, junk)
                except pcapread._Malformed:
                    pass

    def test_missing_file(self):
        packets, stats = pcapread.read_file(os.path.join(HERE, 'nope.pcap'))
        self.assertEqual(packets, [])
        self.assertTrue(stats.truncated_file)


class RealCaptures(unittest.TestCase):
    """Heads of the captures of a dry run (stock Chromium with
    --proxy-server pointed at a real tor in the kit's namespace; see
    docs/LEAK_TESTING.md), written by real tcpdump, as the kit records
    them."""

    REAL = os.path.join(HERE, 'testdata', 'real')

    def read(self, name):
        packets, stats = pcapread.read_file(os.path.join(self.REAL, name))
        self.assertEqual(stats.malformed, 0)
        self.assertFalse(stats.truncated_file)
        return packets, stats

    def test_firewall_log(self):
        packets, stats = self.read('blocked_nflog.pcap')
        self.assertEqual(stats.linktypes, {pcapread.LINKTYPE_NFLOG})
        self.assertEqual(set(p.prefix for p in packets),
                         {'drop', 'dns', 'dns-kit', 'igmp', 'lo-canary'})
        names = set()
        for p in packets:
            if p.dport == 53:
                names.update(q.name for q in dissect.parse_dns(
                    p.payload).questions)
        self.assertIn('stun-ak9473zgza.moltleak.test', names)
        self.assertIn('turn-ak9473zgza.moltleak.test', names)

    def test_tor_log(self):
        packets, _ = self.read('tor_nflog.pcap')
        self.assertTrue(packets)
        self.assertTrue(all(p.prefix == 'tor' and p.is_syn for p in packets))

    def test_any_capture_and_socks(self):
        from leakkit import traffic
        packets, stats = self.read('all_sll2.pcap')
        self.assertEqual(stats.linktypes, {pcapread.LINKTYPE_LINUX_SLL2})
        loopback = [p for p in packets if p.ifindex == 1]
        self.assertTrue(loopback)
        self.assertTrue(all(p.pkttype == pcapread.PACKET_HOST
                            for p in loopback))
        socks = traffic._socks_requests(traffic.Context({}), packets)
        self.assertEqual([(s['host'], s['port']) for s in socks[:2]],
                         [('clients2.google.com', 80), ('www.google.com',
                                                        443)])


class Dissectors(unittest.TestCase):

    def test_dns_compression_and_loops(self):
        q = g.dns_query('www.example.com')
        msg = dissect.parse_dns(q)
        self.assertEqual(msg.questions[0].name, 'www.example.com')
        self.assertEqual(dissect.qtype_name(msg.questions[0].qtype), 'A')
        # A second question that points at the first name.
        two = bytearray(q)
        two[5] = 2
        two += b'\x03foo\xc0\x0c' + struct.pack('!HH', 28, 1)
        msg = dissect.parse_dns(bytes(two))
        self.assertEqual([x.name for x in msg.questions],
                         ['www.example.com', 'foo.www.example.com'])
        loop = struct.pack('!HHHHHH', 1, 0x100, 1, 0, 0, 0) + b'\xc0\x0c' + \
            b'\0\x01\0\x01'
        self.assertEqual(dissect.parse_dns(loop).questions, [])
        self.assertIsNone(dissect.parse_dns(b'\x00' * 5))

    def test_dns_name_escaping_and_length(self):
        raw = struct.pack('!HHHHHH', 1, 0x100, 1, 0, 0, 0) + \
            b'\x05a\nB\x00z\x04TEST\x00' + b'\0\x01\0\x01'
        name = dissect.parse_dns(raw).questions[0].name
        self.assertEqual(name, 'a\\x0ab\\x00z.test')
        too_long = struct.pack('!HHHHHH', 1, 0x100, 1, 0, 0, 0) + \
            (b'\x3f' + b'a' * 63) * 5 + b'\0' + b'\0\x01\0\x01'
        self.assertEqual(dissect.parse_dns(too_long).questions, [])

    def test_dns_tcp(self):
        q = g.dns_query('t.example')
        stream = struct.pack('!H', len(q)) + q + struct.pack('!H', len(q)) + q
        self.assertEqual(len(dissect.parse_dns_tcp(stream)), 2)
        self.assertEqual(dissect.parse_dns_tcp(stream[:10]), [])

    def test_tls_sni(self):
        self.assertEqual(dissect.tls_client_hello_sni(
            g.client_hello('Update.GoogleAPIs.com')), 'update.googleapis.com')
        self.assertIsNone(dissect.tls_client_hello_sni(b'\x16\x03\x01'))
        self.assertIsNone(dissect.tls_client_hello_sni(b'GET / HTTP/1.1'))
        hello = g.client_hello('x.example')
        for cut in range(len(hello)):
            dissect.tls_client_hello_sni(hello[:cut])  # never raises

    def test_http(self):
        self.assertEqual(dissect.http_request(
            b'GET /x HTTP/1.1\r\nHost: Onion.Example\r\n\r\n'),
            ('GET', '/x', 'onion.example'))
        self.assertIsNone(dissect.http_request(b'\x16\x03'))

    def test_quic_and_stun(self):
        self.assertTrue(dissect.is_quic_long_header(
            b'\xc3\x00\x00\x00\x01' + b'\0' * 20))
        self.assertFalse(dissect.is_quic_long_header(b'\x40' + b'\0' * 20))
        stun = struct.pack('!HHI', 1, 0, 0x2112A442) + b'\0' * 12
        self.assertTrue(dissect.is_stun(stun))
        self.assertFalse(dissect.is_stun(b'\0' * 20))

    def test_multicast_groups(self):
        igmp2 = b'\x16\x00\x00\x00' + bytes([224, 0, 0, 251])
        self.assertEqual(dissect.multicast_groups(2, 0x16, igmp2),
                         ['224.0.0.251'])
        rec = b'\x04\x00\x00\x00' + bytes([224, 0, 0, 251])
        igmp3 = b'\x22\x00\x00\x00\x00\x00\x00\x01' + rec
        self.assertEqual(dissect.multicast_groups(2, 0x22, igmp3),
                         ['224.0.0.251'])
        import ipaddress
        mld2 = b'\x8f\x00\x00\x00\x00\x00\x00\x01' + b'\x04\x00\x00\x00' + \
            ipaddress.IPv6Address('ff02::fb').packed
        self.assertEqual(dissect.multicast_groups(58, 143, mld2),
                         ['ff02::fb'])
        self.assertEqual(dissect.multicast_groups(2, 0x22, igmp3[:10]), [])

    def test_socks5(self):
        req = dissect.parse_socks5_client(g.socks5_client(
            'abc.onion', 80, username='https://site.example'))
        self.assertTrue(req.complete)
        self.assertEqual((req.username, req.host, req.port),
                         ('https://site.example', 'abc.onion', 80))
        plain = dissect.parse_socks5_client(g.socks5_client('a.test', 443))
        self.assertEqual((plain.username, plain.host), (None, 'a.test'))
        self.assertIsNone(dissect.parse_socks5_client(b'GET /'))
        data = g.socks5_client('a.test', 443, username='u')
        for cut in range(len(data)):
            dissect.parse_socks5_client(data[:cut])  # never raises


if __name__ == '__main__':
    unittest.main()
