#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the regular-window traffic capture: the allow-list generated
from docs/NETWORK_TRAFFIC.md (hostlist.py), the resolver and sinkhole
(sinkhole.py, also as the processes `run` starts), the firewall's sinkhole
rule, the comparison and the report (bganalysis.py), against a recorded
capture built here. No root and no browser.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import ipaddress
import json
import os
import re
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from leakkit import background
from leakkit import bganalysis
from leakkit import bgcapture
from leakkit import dissect
from leakkit import hostlist
from leakkit import netns
from leakkit import sinkhole
from leakkit import support
from tests import pcapgen as g

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
ENTRY = os.path.join(KIT, 'leaktest.py')

GOOD_DOC = '''# x

<!-- hosts:begin -->
| Host | Ports | Expect | Why | § |
|---|---|---|---|---|
| `a.example.com` | 443, 80 | allowed | A thing | 1 |
| `*.cdn.example.com`, `b.example.com` | 443 | step: download | B thing | 2 |
| `*.example.com` | * | user | Wide | 3 |
| `9.9.9.9` | 53/udp | forbidden | A probe | 2 |
<!-- hosts:end -->
'''

# Component IDs for the tests: 32 letters a-p, made up.
ID_OFF = 'abcdefghijklmnopabcdefghijklmnop'
ID_OFF2 = 'ponmlkjihgfedcbaponmlkjihgfedcba'
ID_OK = 'aaaabbbbccccddddeeeeffffgggghhhh'
ID_OK2 = 'iiiijjjjkkkkllllmmmmnnnnoooopppp'

COMPONENTS_DOC = GOOD_DOC + '''
<!-- components:begin -->
| Component | ID | Why | § |
|---|---|---|---|
| A thing | `%s` | Off | 3 |
| Another | `%s` | Not registered | 3 |
<!-- components:end -->
''' % (ID_OFF, ID_OFF2)


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def real_hosts():
    return hostlist.HostList.load(hostlist.JSON_PATH)


class HostListParsing(unittest.TestCase):

    def test_good_table(self):
        doc = hostlist.build(GOOD_DOC)
        self.assertEqual(doc['schema'], hostlist.SCHEMA)
        hosts = {h['host']: h for h in doc['hosts']}
        self.assertEqual(sorted(hosts), ['*.cdn.example.com', '*.example.com',
                                         '9.9.9.9', 'a.example.com',
                                         'b.example.com'])
        self.assertEqual(hosts['a.example.com']['ports'],
                         ['443/tcp', '80/tcp'])
        self.assertEqual(hosts['b.example.com']['expect'], hostlist.STEP)
        self.assertEqual(hosts['b.example.com']['steps'], ['download'])
        self.assertEqual(hosts['*.example.com']['ports'], ['*'])
        self.assertEqual(hosts['9.9.9.9']['ports'], ['53/udp'])
        self.assertNotIn('line', hosts['9.9.9.9'])

    def test_errors(self):
        def bad(row):
            text = GOOD_DOC.replace('| `9.9.9.9` | 53/udp | forbidden | A '
                                    'probe | 2 |', row)
            with self.assertRaises(hostlist.HostListError):
                hostlist.parse_doc(text)
        bad('| `not a host` | 443 | allowed | x | 1 |')
        bad('| a.example.org | 443 | allowed | x | 1 |')       # no backticks
        bad('| `c.example.org` | 70000 | allowed | x | 1 |')
        bad('| `c.example.org` | 443/sctp | allowed | x | 1 |')
        bad('| `c.example.org` | 443 | sometimes | x | 1 |')
        bad('| `c.example.org` | 443 | step: lunch | x | 1 |')
        bad('| `c.example.org` | 443 | allowed |  | 1 |')
        bad('| `a.example.com` | 443 | allowed | dup | 1 |')
        bad('| `c.example.org` | 443 | allowed | x |')
        bad('not a row')
        with self.assertRaises(hostlist.HostListError):
            hostlist.parse_doc('no markers here')
        with self.assertRaises(hostlist.HostListError):
            hostlist.parse_doc(GOOD_DOC.replace('| Host | Ports |',
                                                '| Name | Ports |'))
        with self.assertRaises(hostlist.HostListError):
            hostlist.parse_doc('%s\n%s\n' % (hostlist.BEGIN, hostlist.END))

    def test_components_table(self):
        self.assertEqual(hostlist.build(GOOD_DOC)['components'], [])
        doc = hostlist.build(COMPONENTS_DOC)
        self.assertEqual([c['id'] for c in doc['components']],
                         [ID_OFF, ID_OFF2])
        self.assertEqual(doc['components'][0], {
            'id': ID_OFF, 'name': 'A thing', 'why': 'Off', 'section': '3'})
        hl = hostlist.HostList(doc)
        self.assertEqual(sorted(hl.components), sorted([ID_OFF, ID_OFF2]))
        self.assertEqual(hl.components[ID_OFF2]['name'], 'Another')
        # A JSON from before the table: no components, no failure.
        old = hostlist.build(GOOD_DOC)
        del old['components']
        self.assertEqual(hostlist.HostList(old).components, {})

    def test_components_table_errors(self):
        row = '| A thing | `%s` | Off | 3 |' % ID_OFF

        def bad(new):
            with self.assertRaises(hostlist.HostListError):
                hostlist.parse_components(COMPONENTS_DOC.replace(row, new))
        bad('| A thing | `%s` | Off | 3 |' % ID_OFF2)        # duplicate
        bad('| A thing | `%sq` | Off | 3 |' % ID_OFF[:31])    # q is not a-p
        bad('| A thing | `%s` | Off | 3 |' % ID_OFF[:30])     # too short
        bad('| A thing | %s | Off | 3 |' % ID_OFF)            # no backticks
        bad('| A thing | `%s` |  | 3 |' % ID_OFF)              # no Why
        bad('| A thing | `%s` | Off |' % ID_OFF)                # 3 cells
        bad('not a row')
        with self.assertRaises(hostlist.HostListError):
            hostlist.parse_components(COMPONENTS_DOC.replace(
                '| Component | ID |', '| Name | ID |'))
        with self.assertRaises(hostlist.HostListError):
            hostlist.parse_components(COMPONENTS_DOC.replace(
                hostlist.COMPONENTS_END, ''))

    def test_lookup(self):
        hl = hostlist.HostList(hostlist.build(GOOD_DOC))
        self.assertEqual(hl.lookup('A.Example.COM.')['host'], 'a.example.com')
        # The most specific wildcard wins; an exact row beats both.
        self.assertEqual(hl.lookup('x.cdn.example.com')['host'],
                         '*.cdn.example.com')
        self.assertEqual(hl.lookup('b.example.com')['host'], 'b.example.com')
        self.assertEqual(hl.lookup('zz.example.com')['host'], '*.example.com')
        self.assertIsNone(hl.lookup('example.com'))
        self.assertIsNone(hl.lookup('cdn.example.com.evil.org'))
        self.assertEqual(hl.lookup('9.9.9.9')['expect'], hostlist.FORBIDDEN)
        self.assertIsNone(hl.lookup('9.9.9.8'))
        self.assertIsNone(hl.lookup(''))
        self.assertIsNone(hl.lookup(None))
        with self.assertRaises(hostlist.HostListError):
            hostlist.HostList({'schema': 'something else'})

    def test_ports(self):
        hl = hostlist.HostList(hostlist.build(GOOD_DOC))
        a = hl.lookup('a.example.com')
        self.assertTrue(hostlist.port_allowed(a, 443))
        self.assertTrue(hostlist.port_allowed(a, 80, 'tcp'))
        self.assertFalse(hostlist.port_allowed(a, 443, 'udp'))
        self.assertFalse(hostlist.port_allowed(a, 8443))
        self.assertTrue(hostlist.port_allowed(a, None))
        self.assertTrue(hostlist.port_allowed(hl.lookup('q.example.com'),
                                              5228))

    def test_likely_source(self):
        hl = real_hosts()
        e = hl.lookup('update.googleapis.com')
        text = hostlist.likely_source('update.googleapis.com', None, 'idle',
                                      300, e)
        self.assertIn('Component updater', text)
        self.assertIn('§1', text)
        text = hostlist.likely_source('newthing-pa.googleapis.com',
                                      'POST /v1/things', 'idle', 61.0)
        self.assertIn('a Google API', text)
        self.assertIn('startup timer', text)
        text = hostlist.likely_source('198.18.0.40', 'GET /time/1/current',
                                      'cert_error', 3.0)
        self.assertIn('network time', text)
        self.assertIn('cert_error step', text)
        self.assertEqual(hostlist.likely_source('x.example.net'), 'unknown')


class HostListMatchesTheDoc(unittest.TestCase):
    """docs/NETWORK_TRAFFIC.md, its generated JSON and the kit's own lists
    can't drift apart."""

    @classmethod
    def setUpClass(cls):
        with open(hostlist.DOC, 'r', encoding='utf-8') as f:
            cls.doc = f.read()
        cls.hosts = real_hosts()

    def test_json_is_generated_from_the_doc(self):
        self.assertIsNone(hostlist.check_files())

    def test_check_files_reports_drift(self):
        tmp = tempfile.mkdtemp()
        try:
            stale = os.path.join(tmp, 'hosts.json')
            with open(stale, 'w') as f:
                f.write('{}\n')
            self.assertIn('out of date', hostlist.check_files(
                json_path=stale))
            self.assertIn('cannot read', hostlist.check_files(
                json_path=os.path.join(tmp, 'missing.json')))
        finally:
            shutil.rmtree(tmp)

    def test_key_rows(self):
        h = self.hosts
        self.assertEqual(h.lookup('update.googleapis.com')['expect'],
                         hostlist.ALLOWED)
        self.assertEqual(h.lookup('edgedl.me.gvt1.com')['expect'],
                         hostlist.ALLOWED)
        sb = h.lookup('sb-ssl.google.com')
        self.assertEqual((sb['expect'], sb['steps']),
                         (hostlist.STEP, ['download']))
        for host in ('content-autofill.googleapis.com', '8.8.8.8',
                     'clientservices.googleapis.com', 'accounts.google.com',
                     'translate.googleapis.com',
                     'connectivitycheck.gstatic.com', 'dns.google'):
            self.assertEqual(h.lookup(host)['expect'], hostlist.FORBIDDEN,
                             host)
        self.assertIn('53/udp', h.lookup('8.8.8.8')['ports'])
        for host in ('duckduckgo.com', 'huggingface.co', 'api.openai.com'):
            self.assertEqual(h.lookup(host)['expect'], hostlist.USER, host)
        self.assertEqual(h.lookup('api.github.com')['expect'],
                         hostlist.ALLOWED)

    def test_component_table(self):
        names = {c['name']: c['id'] for c in self.hosts.components.values()}
        for name in ('On-device model manifest', 'Actor safety lists',
                     'CAPTCHA providers',
                     'Amount extraction heuristic regexes',
                     'Commerce heuristics', 'Indigo', 'Dictation connector',
                     'History search strings', 'On-device head suggest'):
            self.assertIn(name, names)
        # The well-known IDs of two of them, as a check on the table's
        # transcription.
        self.assertEqual(names['On-device head suggest'],
                         'obedbbhbpmojnkanicioggnmelmoomoc')
        self.assertEqual(names['On-device base model'],
                         'fklghjjljmnfjoepjmlobpekiapffcja')

    def test_steps_are_the_captures_steps(self):
        for e in self.hosts.entries:
            for s in e['steps']:
                self.assertIn(s, hostlist.STEP_NAMES)
        browse = [name for name, _, _ in bgcapture.TrafficRun.BROWSE]
        self.assertEqual(tuple(browse), hostlist.BROWSE_STEPS)
        order = ['first_run', 'idle'] + browse + ['shutdown', 'restart',
                                                  'update_check', 'final']
        self.assertEqual(tuple(order), hostlist.STEP_NAMES)

    _TLDS = ('com', 'net', 'org', 'io', 'to', 'nz', 'ai', 'google', 'co')

    def _hosts_named(self, text):
        out = set()
        for token in re.findall(r'`([^`\n]+)`', text):
            token = token.strip()
            if re.match(r'^[a-z-]+://', token) and not token.startswith(
                    ('http://', 'https://')):
                continue        # molt://, chrome://, devtools://
            token = re.sub(r'^https?://', '', token)
            host = re.split(r'[/?\s]', token, maxsplit=1)[0]
            if re.match(r'^\d+\.\d+\.\d+\.\d+$', host):
                out.add(host)
                continue
            host = host.split(':')[0]
            if re.match(r'^(\*\.)?[a-z0-9-]+(\.[a-z0-9-]+)+$', host) and \
                    host.rsplit('.', 1)[1] in self._TLDS:
                out.add(host)
        return out

    def test_every_host_in_sections_1_to_5_is_in_the_table(self):
        # The summary and the detailed sections, not the procedures.
        parts = re.split(r'\n(?=## )', self.doc)
        scanned = [p for p in parts if re.match(
            r'## (Summary|[1-5]\. )', p)]
        self.assertGreaterEqual(len(scanned), 6)
        missing = []
        for part in scanned:
            for host in sorted(self._hosts_named(part)):
                if host.startswith('*.'):
                    ok = any(e['host'] == host for e in self.hosts.entries)
                else:
                    ok = self.hosts.lookup(host) is not None
                if not ok:
                    missing.append('%s (in "%s")' % (host, part.split(
                        '\n', 1)[0]))
        self.assertEqual(missing, [], 'named in NETWORK_TRAFFIC.md but not '
                         'in its section 6 table')

    def test_background_lists_agree_with_the_doc(self):
        rows = {e['host']: e for e in self.hosts.entries}
        for pattern, why in background.EXPECTED:
            self.assertIn(pattern, rows, pattern)
            self.assertNotEqual(rows[pattern]['expect'], hostlist.FORBIDDEN,
                                pattern)
        for pattern, why in background.MUST_NOT_APPEAR:
            self.assertIn(pattern, rows, pattern)
            self.assertIn(rows[pattern]['expect'],
                          (hostlist.FORBIDDEN, hostlist.USER), pattern)

    def test_hosts_command(self):
        out = subprocess.run([sys.executable, '-I', ENTRY, 'hosts'],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn('matches', out.stdout)


def _query(name, qtype=1, txid=0x4242):
    return g.dns_query(name, qtype, txid)


def _answer_ip(reply):
    """The A record of a fakedns reply."""
    self_end = 12
    _, end = dissect.read_name(reply, self_end)
    end += 4
    rtype, rclass, ttl, rdlen = struct.unpack_from('!HHIH', reply, end + 2)
    assert rtype == 1 and rdlen == 4
    return ipaddress.ip_address(reply[end + 12:end + 16])


class FakeDns(unittest.TestCase):

    def test_answers(self):
        pool = sinkhole.AddressPool()
        net = ipaddress.ip_network(sinkhole.SINKHOLE_NET)
        reply, fields = sinkhole.dns_reply(_query('update.googleapis.com'),
                                           pool)
        txid, flags, qd, an, ns, ar = struct.unpack_from('!HHHHHH', reply, 0)
        self.assertEqual((txid, flags & 0x800F, qd, an), (0x4242, 0x8000, 1,
                                                          1))
        ip = _answer_ip(reply)
        self.assertIn(ip, net)
        self.assertEqual(fields['answer'], str(ip))
        self.assertEqual(fields['qname'], 'update.googleapis.com')
        # The same name keeps its address; another gets its own.
        again, _ = sinkhole.dns_reply(_query('Update.GoogleAPIs.com.'), pool)
        self.assertEqual(_answer_ip(again), ip)
        other, _ = sinkhole.dns_reply(_query('api.github.com'), pool)
        self.assertNotEqual(_answer_ip(other), ip)
        msg = dissect.parse_dns(reply)
        self.assertTrue(msg.is_response)

    def test_empty_and_nx(self):
        pool = sinkhole.AddressPool()
        for qtype in (28, 65, 16):
            reply, fields = sinkhole.dns_reply(_query('example.org', qtype),
                                               pool)
            self.assertEqual(fields['answer'], 'NODATA')
            _, flags, _, an, _, _ = struct.unpack_from('!HHHHHH', reply, 0)
            self.assertEqual((flags & 0xF, an), (0, 0))
        reply, fields = sinkhole.dns_reply(_query('nx-abc.invalid'), pool)
        self.assertEqual(fields['answer'], 'NXDOMAIN')
        self.assertEqual(struct.unpack_from('!H', reply, 2)[0] & 0xF, 3)
        self.assertEqual(pool.by_name, {})

    def test_malformed(self):
        pool = sinkhole.AddressPool()
        self.assertEqual(sinkhole.dns_reply(b'\x00\x01', pool)[0], None)
        response = bytearray(_query('a.example'))
        response[2] |= 0x80
        self.assertIsNone(sinkhole.dns_reply(bytes(response), pool)[0])
        two = struct.pack('!HHHHHH', 1, 0x100, 2, 0, 0, 0) + \
            g.dns_name('a.example') + b'\0\x01\0\x01'
        reply, fields = sinkhole.dns_reply(two, pool)
        self.assertTrue(fields.get('malformed'))
        self.assertEqual(struct.unpack_from('!H', reply, 2)[0] & 0xF, 1)
        reply, fields = sinkhole.dns_reply(_query('a.example')[:-3], pool)
        self.assertTrue(fields.get('malformed'))

    def test_pool(self):
        pool = sinkhole.AddressPool('198.18.0.0/23', first=250)
        seen = [pool.address('h%d' % i) for i in range(20)]
        self.assertEqual(len(set(seen)), 20)
        for ip in seen:
            self.assertNotIn(ip.packed[-1], (0, 255))
        small = sinkhole.AddressPool('198.18.0.0/30', first=1)
        small.address('a')
        small.address('b')
        with self.assertRaises(RuntimeError):
            small.address('c')


class SinkholeParsing(unittest.TestCase):

    def test_parse_head(self):
        method, target, headers = sinkhole.parse_head(
            b'POST /service/update2/json?cup2key=1 HTTP/1.1\r\nHost: '
            b'update.googleapis.com\r\nX-Goog-Api-Key: dummytoken\r\n'
            b'Content-Length: 5\r\n\r\nhello')
        self.assertEqual((method, target), ('POST',
                                            '/service/update2/json?cup2key=1'))
        logged = sinkhole.logged_headers(headers)
        self.assertEqual(logged['host'], 'update.googleapis.com')
        self.assertEqual(logged['x-goog-api-key'], '(10 bytes, not logged)')
        self.assertIsNone(sinkhole.parse_head(b'\x16\x03\x01garbage'))
        self.assertIsNone(sinkhole.parse_head(b'GET /\r\n\r\n'))

    def test_update_check_component_ids(self):
        self.assertTrue(sinkhole.is_update_check(
            'POST', '/service/update2/json?cup2key=1:2&cup2hreq=ab'))
        self.assertFalse(sinkhole.is_update_check('GET',
                                                  '/service/update2/json'))
        self.assertFalse(sinkhole.is_update_check('POST', '/v1/things'))
        body = json.dumps({'request': {'protocol': '4.0', 'apps': [
            {'appid': ID_OK, 'version': '1.2'},
            {'appid': ID_OFF, 'version': '0.0.0.0'},
            {'appid': ID_OK},                               # duplicate
            {'appid': 'not-an-id'}, {'appid': 5}, 'junk']}}).encode()
        self.assertEqual(sinkhole.update_app_ids(body), [ID_OK, ID_OFF])
        # The older protocol's "app", and the header's copy.
        old = json.dumps({'request': {'app': [{'appid': ID_OK2}]}}).encode()
        self.assertEqual(sinkhole.update_app_ids(old, '%s, %s' % (ID_OK2,
                                                                  ID_OFF2)),
                         [ID_OK2, ID_OFF2])
        self.assertEqual(sinkhole.update_app_ids(b'', ID_OK), [ID_OK])
        for junk in (b'', b'{', b'[1]', b'\xff\xfe', b'{"request": 3}',
                     b'{"request": {"apps": {}}}', b'[' * 100000):
            self.assertEqual(sinkhole.update_app_ids(junk), [], junk[:20])
        many = json.dumps({'request': {'apps': [
            {'appid': ''.join('abcdefghijklmnop'[(i >> s) & 15]
                              for s in range(0, 128, 4))}
            for i in range(1000)]}}).encode()
        self.assertEqual(len(sinkhole.update_app_ids(many)), 300)


@unittest.skipUnless(shutil.which('openssl'), 'needs openssl')
class SinkholeServers(unittest.TestCase):
    """The resolver and the sinkhole as `run` starts them."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.spki = support.make_certs(cls.tmp, ['localhost'], ['127.0.0.1'], {
            'ca_issuers': ['http://aia.moltleak.test/i.crt'],
            'ocsp': 'http://ocsp.moltleak.test/',
            'crl': 'http://crl.moltleak.test/c.crl'})
        cls.procs = []

    @classmethod
    def tearDownClass(cls):
        for p in cls.procs:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
            p.stdout.close()
        shutil.rmtree(cls.tmp)

    def start(self, *args):
        proc = subprocess.Popen([sys.executable, '-I', ENTRY, 'service'] +
                                list(args), stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
        self.procs.append(proc)
        self.assertEqual(proc.stdout.readline().strip(), b'ready')
        return proc

    def read_log(self, path, n, timeout=10):
        deadline = time.time() + timeout
        items = []
        while time.time() < deadline:
            try:
                with open(path) as f:
                    items = [json.loads(line) for line in f if line.strip()]
            except (IOError, OSError, ValueError):
                items = []
            if len(items) >= n:
                break
            time.sleep(0.1)
        return items

    def test_fakedns(self):
        port = free_port(socket.SOCK_DGRAM)
        log = os.path.join(self.tmp, 'fakedns.jsonl')
        self.start('fakedns', '--listen', '127.0.0.1', '--port', str(port),
                   '--log', log)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(5)
        s.sendto(_query('safebrowsing.googleapis.com'), ('127.0.0.1', port))
        reply, _ = s.recvfrom(4096)
        s.close()
        ip = _answer_ip(reply)
        self.assertIn(ip, ipaddress.ip_network(sinkhole.SINKHOLE_NET))
        t = socket.create_connection(('127.0.0.1', port), timeout=5)
        q = _query('safebrowsing.googleapis.com', txid=7)
        t.sendall(struct.pack('!H', len(q)) + q)
        n = struct.unpack('!H', t.recv(2))[0]
        data = b''
        while len(data) < n:
            data += t.recv(n - len(data))
        t.close()
        self.assertEqual(_answer_ip(data), ip)
        items = self.read_log(log, 2)
        self.assertEqual([i['proto'] for i in items], ['udp', 'tcp'])
        self.assertEqual(items[0]['answer'], str(ip))
        self.assertEqual(items[0]['qname'], 'safebrowsing.googleapis.com')

    def test_sinkhole_tls_and_http(self):
        tls_port, http_port = free_port(), free_port()
        log = os.path.join(self.tmp, 'sinkhole.jsonl')
        self.start('sinkhole', '--ports', '%d,%d' % (tls_port, http_port),
                   '--cert', os.path.join(self.tmp, 'server.crt'), '--key',
                   os.path.join(self.tmp, 'server.key'), '--log', log)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_alpn_protocols(['h2', 'http/1.1'])
        raw = socket.create_connection(('127.0.0.1', tls_port), timeout=5)
        s = ctx.wrap_socket(raw, server_hostname='update.googleapis.com')
        body = json.dumps({'request': {'@os': 'linux', 'apps': [
            {'appid': ID_OK, 'version': '31.4.15.9'},
            {'appid': ID_OFF, 'version': '0.0.0.0'}]}}).encode()
        s.sendall(b'POST /service/update2/json HTTP/1.1\r\nHost: '
                  b'update.googleapis.com\r\nX-Goog-Api-Key: dummytoken\r\n'
                  b'Content-Length: %d\r\n\r\n' % len(body) + body)
        resp = s.recv(4096)
        s.close()
        self.assertTrue(resp.startswith(b'HTTP/1.1 503'), resp)
        h = socket.create_connection(('127.0.0.1', http_port), timeout=5)
        h.sendall(b'GET /time/1/current?cup2key=7 HTTP/1.1\r\nHost: '
                  b'clients2.google.com\r\n\r\n')
        self.assertTrue(h.recv(4096).startswith(b'HTTP/1.1 503'))
        h.close()
        items = sorted(self.read_log(log, 2), key=lambda i: i['ts'])
        self.assertEqual(len(items), 2, items)
        tls, plain = items
        self.assertEqual((tls['tls'], tls['sni'], tls['method'], tls['path']),
                         (True, 'update.googleapis.com', 'POST',
                          '/service/update2/json'))
        self.assertEqual(tls['alpn'], 'http/1.1')
        self.assertEqual(tls['headers']['x-goog-api-key'],
                         '(10 bytes, not logged)')
        self.assertEqual(tls['body_length'], len(body))
        self.assertEqual(tls['port'], tls_port)
        self.assertEqual(tls['dst'], '127.0.0.1')
        # Only the component IDs come out of the body.
        self.assertEqual(tls['update_apps'], [ID_OK, ID_OFF])
        self.assertNotIn('linux', json.dumps(tls))
        self.assertNotIn('31.4.15.9', json.dumps(tls))
        self.assertEqual((plain['tls'], plain['path'],
                          plain['headers']['host']),
                         (False, '/time/1/current?cup2key=7',
                          'clients2.google.com'))
        self.assertNotIn('update_apps', plain)

    def test_expired_certificate(self):
        d = os.path.join(self.tmp, 'expired')
        os.makedirs(d)
        cert, key = bgcapture.make_expired_cert(d)
        dates = subprocess.run(['openssl', 'x509', '-in', cert, '-noout',
                                '-enddate', '-ext', 'subjectAltName'],
                               stdout=subprocess.PIPE,
                               universal_newlines=True).stdout
        self.assertIn('2020', dates)
        self.assertIn('127.0.0.1', dates)
        self.assertNotEqual(support.spki_sha256_b64(cert), self.spki)


_CLIENT = r'''
import socket, ssl, struct, sys
def query(name):
    q = struct.pack('!HHHHHH', 7, 0x100, 1, 0, 0, 0)
    for label in name.split('.'):
        q += bytes([len(label)]) + label.encode()
    q += b'\0\0\x01\0\x01'
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(q, ('127.0.0.1', 53))
    r = s.recv(512)
    return socket.inet_ntoa(r[-4:])
ip = query('update.googleapis.com')
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
t = ctx.wrap_socket(socket.create_connection((ip, 443), timeout=5),
                    server_hostname='update.googleapis.com')
t.sendall(b'POST /service/update2/json HTTP/1.1\r\nHost: '
          b'update.googleapis.com\r\nContent-Length: 2\r\n\r\n{}')
print(t.recv(100).split(b'\r\n')[0].decode())
t.close()
try:
    socket.create_connection((query('mtalk.google.com'), 5228), timeout=3)
except OSError as e:
    print('5228:', type(e).__name__)
'''


@unittest.skipUnless(os.geteuid() == 0 and all(shutil.which(t) for t in (
    'ip', 'nft', 'tcpdump', 'openssl')), 'needs root, ip, nft, tcpdump')
class SinkholeInANamespace(unittest.TestCase):
    """The capture's network pieces with the real kernel, in a scratch
    namespace: the local route for the sinkhole range, the kit's firewall
    rules (the Tor cgroup rules left out when cgroup v2 isn't there), the
    resolver and the sinkhole, and the analysis of what tcpdump logged.
    Root only, so it runs on the build machine (sudo), not in CI."""

    def setUp(self):
        self.ns = 'mlkt' + os.urandom(3).hex()
        self.tmp = tempfile.mkdtemp()
        self.procs = []
        if subprocess.run(['ip', 'netns', 'add', self.ns],
                          stderr=subprocess.DEVNULL).returncode != 0:
            shutil.rmtree(self.tmp)
            self.skipTest('cannot create a network namespace here')

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
            p.stdout.close()
            p.stderr.close()
        subprocess.run(['ip', 'netns', 'del', self.ns])
        shutil.rmtree(self.tmp)

    def in_ns(self, *argv, **kw):
        return subprocess.run(['ip', 'netns', 'exec', self.ns] + list(argv),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, **kw)

    def spawn(self, *argv, ready=True):
        p = subprocess.Popen(['ip', 'netns', 'exec', self.ns] + list(argv),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        if ready:
            self.assertEqual(p.stdout.readline().strip(), b'ready')
        return p

    def test_capture_pipeline(self):
        self.in_ns('ip', 'link', 'set', 'lo', 'up')
        r = self.in_ns('ip', 'route', 'add', 'local', sinkhole.SINKHOLE_NET,
                       'dev', 'lo')
        self.assertEqual(r.returncode, 0, r.stderr)
        # The kit's rules without the Tor cgroup's two (nft resolves a
        # cgroup path at load time; no Tor runs in a capture).
        rules = netns.netns_ruleset('x/tor', 1, 47321,
                                    sinkhole=sinkhole.SINKHOLE_NET)
        rules = '\n'.join(line for line in rules.split('\n')
                          if 'socket cgroupv2' not in line)
        r = self.in_ns('nft', '-f', '-', input=rules)
        self.assertEqual(r.returncode, 0, r.stderr)
        support.make_certs(self.tmp, ['localhost'], ['127.0.0.1'], {
            'ca_issuers': ['http://a.moltleak.test/i'],
            'ocsp': 'http://o.moltleak.test/',
            'crl': 'http://c.moltleak.test/c'})
        os.makedirs(os.path.join(self.tmp, 'logs'))
        os.makedirs(os.path.join(self.tmp, 'capture'))
        pcap = os.path.join(self.tmp, 'capture', 'blocked.pcap')
        dump = self.spawn('tcpdump', '-i', 'nflog:1', '-n', '-U', '-s', '0',
                          '-w', pcap, ready=False)
        deadline = time.time() + 10
        while time.time() < deadline:
            line = dump.stderr.readline()
            if b'listening on' in line or not line:
                break
        self.spawn(sys.executable, '-I', ENTRY, 'service', 'fakedns',
                   '--listen', '127.0.0.1', '--port', '53', '--log',
                   os.path.join(self.tmp, 'logs', 'fakedns.jsonl'))
        self.spawn(sys.executable, '-I', ENTRY, 'service', 'sinkhole',
                   '--ports', '80,443', '--cert',
                   os.path.join(self.tmp, 'server.crt'), '--key',
                   os.path.join(self.tmp, 'server.key'), '--log',
                   os.path.join(self.tmp, 'logs', 'sinkhole.jsonl'))
        t0 = time.time()
        out = self.in_ns(sys.executable, '-I', '-c', _CLIENT, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn('HTTP/1.1 503', out.stdout)
        self.assertIn('5228: ConnectionRefusedError', out.stdout)
        time.sleep(1)
        dump.terminate()
        dump.wait(timeout=5)
        manifest = {'schema': bgcapture.SCHEMA, 't0': t0 - 1,
                    'phases': [{'name': 'idle', 'start': t0 - 1,
                                'end': time.time()}],
                    'captures': {'blocked': 'capture/blocked.pcap'},
                    'logs': {'dns': 'logs/fakedns.jsonl',
                             'sinkhole': 'logs/sinkhole.jsonl'},
                    'sinkhole': {'net': sinkhole.SINKHOLE_NET}}
        events, facts, _ = bganalysis.analyze(manifest, self.tmp,
                                              real_hosts())
        got = {(e.kind, e.host, e.port): e for e in events}
        tls = got[('tcp', 'update.googleapis.com', 443)]
        self.assertEqual(tls.category, bganalysis.EXPECTED)
        self.assertEqual(tls.sni, {'update.googleapis.com'})
        self.assertIn('POST /service/update2/json', tls.requests)
        # The firewall's first-packet log and the sinkhole's own log were
        # matched into one connection.
        self.assertEqual(tls.count, 1)
        push = got[('tcp', 'mtalk.google.com', 5228)]
        self.assertEqual(push.category, bganalysis.USER_ONLY)
        self.assertEqual(facts['escaped'], [])


class Firewall(unittest.TestCase):

    def test_sinkhole_rule(self):
        plain = netns.netns_ruleset('moltleak-abc/tor', 1, 47321)
        rules = netns.netns_ruleset('moltleak-abc/tor', 1, 47321,
                                    sinkhole='198.18.0.0/15')
        rule = ('ip daddr 198.18.0.0/15 ct state new log group 1 prefix '
                '"sinkhole" counter accept')
        self.assertIn(rule, rules)
        self.assertNotIn('sinkhole', plain)
        self.assertEqual(rules.replace('    ' + rule + '\n', ''), plain)
        # After the DNS rules (a query to a sinkhole address is DNS that
        # goes elsewhere), before the loopback accept.
        order = [rules.index(s) for s in (
            'dport { 53, 853, 5353, 5355 }', rule, 'oif "lo" counter accept',
            'prefix "drop" counter drop')]
        self.assertEqual(order, sorted(order))
        with self.assertRaises(ValueError):
            netns.netns_ruleset('x/tor', 1, 1, sinkhole='fd00::/64')
        with self.assertRaises(ValueError):
            netns.netns_ruleset('x/tor', 1, 1, sinkhole='not a net')


T0 = 1000.0
NS4, GW4 = '10.231.7.2', '10.231.7.1'


def _phases():
    plan = [('first_run', 0, 45), ('idle', 45, 645), ('home', 645, 655),
            ('form', 655, 675), ('translate', 675, 690),
            ('spellcheck', 690, 705), ('download', 705, 722),
            ('dns_error', 722, 735), ('cert_error', 735, 752),
            ('shutdown', 752, 758), ('restart', 758, 880),
            ('update_check', 880, 926), ('final', 926, 948)]
    return [{'name': n, 'target': 'regular', 'start': T0 + a,
             'end': T0 + b, 'data': {}} for n, a, b in plan]


def make_capture(run_dir, events, escaped=False, apps=None):
    """A recorded capture. events: (rel ts, name, port, sni, request) -
    a lookup of `name` followed, when port is set, by a connection to its
    sinkhole address on that port, which the sinkhole logs. apps: {rel ts:
    component IDs} for the update checks among them."""
    apps = apps or {}
    os.makedirs(os.path.join(run_dir, 'logs'))
    os.makedirs(os.path.join(run_dir, 'capture'))
    pool = sinkhole.AddressPool()
    dns, sink, blocked, allp = [], [], [], []
    sport = 40000
    for rel, name, port, sni, request in events:
        ts = T0 + rel
        if name is not None:
            reply, fields = sinkhole.dns_reply(_query(name), pool)
            dns.append(dict(fields, ts=ts, proto='udp', client='127.0.0.1',
                            server='127.0.0.1'))
            blocked.append((ts, g.nflog(g.ip4('127.0.0.1', '127.0.0.1', 17,
                                              g.udp(50000, 53, _query(name))),
                                        'dns-kit')))
        if port is None:
            continue
        ip = str(pool.address(name.lower()))
        sport += 1
        blocked.append((ts + 0.05, g.nflog(g.ip4(ip, ip, 6, g.tcp(
            sport, port, flags=0x02)), 'sinkhole')))
        allp.append((ts + 0.05, g.sll2(g.ip4(ip, ip, 6, g.tcp(
            sport, port, flags=0x02)), ifindex=1, pkttype=0)))
        if port in (80, 443):
            item = {'ts': ts + 0.1, 'port': port, 'dst': ip,
                    'peer_port': sport, 'tls': port == 443}
            if sni:
                item['sni'] = sni
            if request:
                method, path = request.split(' ', 1)
                item.update(method=method, path=path,
                            headers={'host': sni or name})
            if rel in apps:
                item['update_apps'] = list(apps[rel])
            sink.append(item)
    if escaped:
        allp.append((T0 + 500, g.sll2(g.ip4(NS4, '93.184.216.34', 6, g.tcp(
            41000, 443, flags=0x02)), ifindex=2, pkttype=4)))
    with open(os.path.join(run_dir, 'logs', 'fakedns.jsonl'), 'w') as f:
        for item in dns:
            f.write(json.dumps(item) + '\n')
    with open(os.path.join(run_dir, 'logs', 'sinkhole.jsonl'), 'w') as f:
        for item in sink:
            f.write(json.dumps(item) + '\n')
    return blocked, allp


def write_capture(run_dir, blocked, allp, extra_blocked=()):
    blocked = sorted(list(blocked) + list(extra_blocked), key=lambda x: x[0])
    with open(os.path.join(run_dir, 'capture', 'blocked.pcap'), 'wb') as f:
        f.write(g.pcap(blocked, 239))
    with open(os.path.join(run_dir, 'capture', 'all.pcap'), 'wb') as f:
        f.write(g.pcap(sorted(allp, key=lambda x: x[0]), 276))
    manifest = {
        'schema': bgcapture.SCHEMA, 'run_id': 'abc123', 'target': 'traffic',
        't0': T0, 'started': '2026-10-25T10:00:00+00:00',
        'finished': '2026-10-25T10:16:00+00:00',
        'browser': {'product': 'MoltBrowser', 'version': '0.3.0'},
        'phases': _phases(), 'errors': [],
        'launches': [{'label': 'first-run', 'ts': T0 + 0.5},
                     {'label': 'restart', 'ts': T0 + 758.5}],
        'captures': {'all': 'capture/all.pcap',
                     'blocked': 'capture/blocked.pcap'},
        'logs': {'dns': 'logs/fakedns.jsonl',
                 'sinkhole': 'logs/sinkhole.jsonl'},
        'sinkhole': {'net': sinkhole.SINKHOLE_NET, 'ports': [80, 443]},
        'net': {'ns_ipv4': NS4, 'gw_ipv4': GW4, 'ns_ipv6': 'fd6d:6c6b:7::2',
                'gw_ipv6': 'fd6d:6c6b:7::1'},
        'canary': {'domain': 'moltleak.test'},
        'hosts': {'path': hostlist.JSON_PATH},
    }
    with open(os.path.join(run_dir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f)


CLEAN = [
    (12, 'easylist.to', 443, 'easylist.to', 'GET /easylist/easylist.txt'),
    (31, 'api.github.com', 443, 'api.github.com',
     'GET /repos/OneManNoCode/MoltBrowser/releases/latest'),
    (62, 'update.googleapis.com', 443, 'update.googleapis.com',
     'POST /service/update2/json'),
    (90, 'safebrowsing.googleapis.com', 443, 'safebrowsing.googleapis.com',
     'GET /v5/hashLists:batchGet?key=dummytoken'),
    (95, 'clients2.google.com', 80, None, 'GET /time/1/current?cup2key=1'),
    (712, 'sb-ssl.google.com', 443, 'sb-ssl.google.com',
     'POST /safebrowsing/clientreport/download?key=dummytoken'),
    (723, 'nx-abcdefgh.invalid', None, None, None),
    (885, 'api.github.com', 443, 'api.github.com',
     'GET /repos/OneManNoCode/MoltBrowser/releases/latest'),
]
# The update check at +62 s names two components the doc doesn't list.
CLEAN_APPS = {62: [ID_OK, ID_OK2]}
# Real IDs from NETWORK_TRAFFIC.md's component table.
MANIFEST_ID = 'ceofaddefefcbblgcgnibnonglccbfja'
ACTOR_LISTS_ID = 'ninodabcejpeglfjbkhdplaoglpcbffj'


class Analysis(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.hosts = real_hosts()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def build(self, events, extra_blocked=(), escaped=False, name='run',
              apps=None):
        run_dir = os.path.join(self.tmp, name)
        blocked, allp = make_capture(run_dir, events, escaped=escaped,
                                     apps=CLEAN_APPS if apps is None else apps)
        write_capture(run_dir, blocked, allp, extra_blocked)
        return run_dir, bganalysis.build(run_dir, self.hosts)

    def by_id(self, rep):
        return {c['id']: c for c in rep['checks']}

    def test_clean_capture_passes(self):
        run_dir, rep = self.build(CLEAN)
        checks = self.by_id(rep)
        self.assertEqual(rep['verdict'], 'PASS', json.dumps(
            [c for c in rep['checks'] if c['status'] != 'PASS' and
             c['status'] != 'INFO'], indent=1))
        self.assertEqual(checks['traffic.harness.update_check']['status'],
                         'PASS')
        self.assertEqual(checks['traffic.harness.escaped']['status'], 'PASS')
        expected = [e for e in rep['events'] if e['category'] == 'expected']
        hosts = set(e['host'] for e in expected)
        self.assertIn('update.googleapis.com', hosts)
        self.assertIn('clients2.google.com', hosts)
        sb = [e for e in expected if e['host'] == 'sb-ssl.google.com' and
              e['kind'] == 'tcp'][0]
        self.assertEqual(sb['step'], 'download')
        self.assertEqual(sb['port'], 443)
        self.assertIn('POST /safebrowsing/clientreport/download?key='
                      'dummytoken', sb['requests'])
        nx = [e for e in rep['events'] if e['host'].endswith('.invalid')][0]
        self.assertEqual(nx['category'], 'page')
        upd = [e for e in rep['events'] if e['host'] ==
               'update.googleapis.com' and e['kind'] == 'tcp'][0]
        self.assertEqual(upd['step'], 'idle')
        self.assertTrue(61 < upd['since_launch'] < 62, upd['since_launch'])
        self.assertIn('traffic.not_seen', checks)
        self.assertNotIn('update.googleapis.com', ' '.join(
            checks['traffic.not_seen']['evidence']))
        comp = checks['traffic.components']
        self.assertEqual(comp['status'], 'PASS')
        self.assertIn('2 component(s): %s, %s' % (ID_OK, ID_OK2),
                      ' '.join(comp['evidence']))

    def test_components_the_doc_turns_off_fail(self):
        events = CLEAN + [(400, 'update.googleapis.com', 443,
                           'update.googleapis.com',
                           'POST /service/update2/json?cup2key=2')]
        run_dir, rep = self.build(events, apps={
            62: [ID_OK, MANIFEST_ID], 400: [ACTOR_LISTS_ID, ID_OK2]})
        self.assertEqual(rep['verdict'], 'FAIL')
        comp = self.by_id(rep)['traffic.components']
        self.assertEqual(comp['status'], 'FAIL')
        text = '\n'.join(comp['evidence'])
        self.assertIn('On-device model manifest (%s)' % MANIFEST_ID, text)
        self.assertIn('Actor safety lists (%s)' % ACTOR_LISTS_ID, text)
        self.assertIn('+62.', text)
        self.assertIn('idle', text)
        self.assertIn('update.googleapis.com', text)
        # The hosts themselves are still as expected.
        self.assertEqual(self.by_id(rep)['traffic.not_listed']['status'],
                         'PASS')
        self.assertIn('On-device model manifest',
                      bganalysis.to_markdown(rep))

    def test_no_component_ids_is_a_skip(self):
        _, rep = self.build(CLEAN, apps={})
        comp = self.by_id(rep)['traffic.components']
        self.assertEqual(comp['status'], 'SKIP')
        self.assertIn('0 update check(s) seen', comp['evidence'][0])
        self.assertEqual(rep['verdict'], 'PASS')
        # An update check whose body had no IDs counts as seen, still a skip.
        _, rep = self.build(CLEAN, apps={62: []}, name='empty-ids')
        comp = self.by_id(rep)['traffic.components']
        self.assertEqual(comp['status'], 'SKIP')
        self.assertIn('1 update check(s) seen', comp['evidence'][0])

    def test_failures_name_time_step_and_source(self):
        bad = CLEAN + [
            (300, 'newthing-pa.googleapis.com', 443,
             'newthing-pa.googleapis.com', 'POST /v1/things:fetch'),
            (660, 'content-autofill.googleapis.com', 443,
             'content-autofill.googleapis.com', 'GET /v1/pages/abc'),
            (800, 'sb-ssl.google.com', 443, 'sb-ssl.google.com',
             'POST /safebrowsing/clientreport/download'),
            (400, 'huggingface.co', 443, 'huggingface.co', 'GET /'),
            (70, 'update.googleapis.com', 8443, None, None),
        ]
        probe = g.nflog(g.ip4(NS4, '8.8.8.8', 17, g.udp(51000, 53, _query(
            'google.com'))), 'dns')
        doh = g.nflog(g.ip4(NS4, '1.1.1.1', 6, g.tcp(
            51001, 443, g.client_hello('cloudflare-dns.com'))), 'drop')
        mdns = g.nflog(g.ip4(NS4, '224.0.0.22', 2, bytes([0x22, 0, 0, 0, 0, 0,
                                                          0, 1, 4, 0, 0, 0]) +
                             ipaddress.IPv4Address('224.0.0.251').packed),
                       'igmp')
        run_dir, rep = self.build(bad, extra_blocked=[
            (T0 + 724, probe), (T0 + 200, doh), (T0 + 20, mdns)],
            escaped=True)
        self.assertEqual(rep['verdict'], 'FAIL')
        checks = self.by_id(rep)
        ev = {(e['host'], e['category']) for e in rep['events']}
        self.assertIn(('newthing-pa.googleapis.com', 'not listed'), ev)
        self.assertIn(('content-autofill.googleapis.com', 'forbidden'), ev)
        self.assertIn(('sb-ssl.google.com', 'outside its step'), ev)
        self.assertIn(('huggingface.co', 'user only'), ev)
        self.assertIn(('update.googleapis.com', 'unexpected port'), ev)
        self.assertIn(('8.8.8.8', 'forbidden'), ev)
        self.assertIn(('cloudflare-dns.com', 'user only'), ev)
        self.assertIn(('224.0.0.251', 'not listed'), ev)
        for cid in ('traffic.forbidden', 'traffic.not_listed',
                    'traffic.user_only', 'traffic.outside_its_step',
                    'traffic.unexpected_port', 'traffic.harness.escaped'):
            self.assertEqual(checks[cid]['status'], 'FAIL', cid)
        line = [e for e in checks['traffic.not_listed']['evidence']
                if 'newthing-pa' in e and '] tcp ' in e][0]
        self.assertIn('+300.', line)
        self.assertIn('idle', line)
        self.assertIn('POST /v1/things:fetch', line)
        self.assertIn('a Google API', line)
        probe_line = [e for e in checks['traffic.forbidden']['evidence']
                      if '8.8.8.8' in e][0]
        self.assertIn('dns_error', probe_line)
        self.assertIn('DNS query google.com', probe_line)
        self.assertIn('DNS probe', probe_line)
        md = bganalysis.to_markdown(rep)
        self.assertIn('# Regular-window traffic capture', md)
        self.assertIn('**Verdict: FAIL**', md)
        self.assertIn('| +300.', md)
        self.assertIn('newthing-pa.googleapis.com', md)

    def test_missing_update_check_is_an_error(self):
        events = [e for e in CLEAN if e[0] != 885]
        _, rep = self.build(events)
        self.assertEqual(self.by_id(rep)['traffic.harness.update_check'][
            'status'], 'ERROR')
        self.assertEqual(rep['verdict'], 'INCOMPLETE')

    def test_empty_capture_is_an_error(self):
        run_dir = os.path.join(self.tmp, 'empty')
        os.makedirs(run_dir)
        write_capture_dir = os.path.join(run_dir, 'capture')
        os.makedirs(write_capture_dir)
        os.makedirs(os.path.join(run_dir, 'logs'))
        write_capture(run_dir, [], [])
        rep = bganalysis.build(run_dir, self.hosts)
        self.assertEqual(self.by_id(rep)['traffic.harness.capture']['status'],
                         'ERROR')
        self.assertNotEqual(rep['verdict'], 'PASS')

    def test_hostile_logs_do_not_crash(self):
        run_dir = os.path.join(self.tmp, 'hostile')
        blocked, allp = make_capture(run_dir, CLEAN)
        write_capture(run_dir, blocked, allp, extra_blocked=[
            (T0 + 5, b'\x02\x00\x00\x01\xff\xff'),
            (T0 + 6, g.nflog(b'\x45' + b'\x00' * 10, 'sinkhole'))])
        with open(os.path.join(run_dir, 'logs', 'sinkhole.jsonl'), 'a') as f:
            f.write('not json\n[1,2]\n{"ts": "x", "dst": 5, "port": "p"}\n')
            f.write('{"update_apps": [1, "x", null, {"a": 1}, "%s"], '
                    '"headers": "h", "sni": 7}\n{"update_apps": "no"}\n' %
                    MANIFEST_ID)
        with open(os.path.join(run_dir, 'logs', 'fakedns.jsonl'), 'a') as f:
            f.write('{"qname": null}\n{"ts": 1, "qname": "%s"}\n' % (
                'a' * 400))
        rep = bganalysis.build(run_dir, self.hosts)
        self.assertIn(rep['verdict'], ('PASS', 'FAIL', 'INCOMPLETE'))
        json.dumps(rep)

    def test_report_command_dispatches(self):
        run_dir, _ = self.build(CLEAN, name='cli')
        out = subprocess.run([sys.executable, '-I', ENTRY, 'report', run_dir],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn('verdict: PASS', out.stdout)
        with open(os.path.join(run_dir, 'report.json')) as f:
            rep = json.load(f)
        self.assertEqual(rep['schema'], bganalysis.REPORT_SCHEMA)
        self.assertTrue(os.path.isfile(os.path.join(run_dir, 'report.md')))
        self.assertTrue(bganalysis.is_traffic_run(run_dir))
        self.assertFalse(bganalysis.is_traffic_run(self.tmp))


class CapturePlan(unittest.TestCase):

    def test_options(self):
        opts = bgcapture.TrafficOptions(browser='/bin/true', idle=5)
        self.assertEqual(opts.target, 'traffic')
        self.assertFalse(opts.extension)
        self.assertEqual(opts.idle, 5)
        self.assertEqual(opts.restart_idle, 120)
        self.assertEqual(opts.hosts, hostlist.JSON_PATH)

    def test_browse_pages_exist(self):
        for page in ('bg_form.html', 'bg_done.html', 'bg_fr.html',
                     'bg_spell.html', 'download.html', 'basic.html'):
            self.assertTrue(os.path.isfile(os.path.join(KIT, 'pages', page)),
                            page)
        with open(os.path.join(KIT, 'pages', 'bg_form.html')) as f:
            form = f.read()
        for field in ('id="name"', 'id="street"', 'id="city"', 'id="zip"',
                      'id="user"', 'id="pass"', 'id="signin"'):
            self.assertIn(field, form)
        for page in ('bg_form.html', 'bg_fr.html', 'bg_spell.html',
                     'bg_done.html'):
            with open(os.path.join(KIT, 'pages', page)) as f:
                self.assertNotRegex(f.read(), r'(src|href)="https?://')


if __name__ == '__main__':
    unittest.main()
