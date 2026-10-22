#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the runtime pieces that need no root and no browser: the
firewall rules, the Tor wrapper, the launcher's argument checks, the kit's
servers (each started as its own process, as `run` does) and the DevTools
pipe client (against a fake browser).

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import base64
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

from leakkit import cdp
from leakkit import dissect
from leakkit import netns
from leakkit import services
from tests import pcapgen as g

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
ENTRY = os.path.join(KIT, 'leaktest.py')


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Firewall(unittest.TestCase):

    def test_namespace_rules(self):
        rules = netns.netns_ruleset('unified/moltleak-abc123/tor', 2, 47321)
        self.assertIn('policy drop;', rules)
        self.assertIn('socket cgroupv2 level 2 "unified/moltleak-abc123/tor" '
                      'ct state new log group 2 prefix "tor"', rules)
        self.assertIn('oif "lo" tcp dport 47321 ct state new log group 1 '
                      'prefix "lo-canary"', rules)
        # Tor first, the final drop last; DNS before the loopback accept.
        order = [rules.index(s) for s in (
            'socket cgroupv2', 'dport { 53, 853, 5353, 5355 }', 'oif "lo" '
            'counter accept', 'prefix "drop" counter drop')]
        self.assertEqual(order, sorted(order))
        with self.assertRaises(ValueError):
            netns.netns_ruleset('a"b', 2, 1)

    def test_host_rules(self):
        rules = netns.host_ruleset('moltleak_abc123', 'mlkabc123h',
                                   '10.231.9.0/30')
        self.assertIn('ip saddr 10.231.9.0/30 oifname != "mlkabc123h" '
                      'counter masquerade', rules)
        self.assertIn('oifname "mlkabc123h" counter drop', rules)

    def test_pick_index(self):
        x = netns.pick_index('abcd12', '')
        self.assertTrue(1 <= x <= 250)
        used = ''.join('inet 10.231.%d.1/30\n' % i for i in range(1, 250))
        self.assertEqual(netns.pick_index('abcd12', used), 250)

    def test_cgroup2_mount(self):
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, 'mountinfo')
            with open(path, 'w') as f:
                f.write('30 23 0:26 / /sys/fs/cgroup rw,nosuid - tmpfs tmpfs '
                        'ro\n39 30 0:33 / /sys/fs/cgroup/unified rw - cgroup2 '
                        'cgroup2 rw\n')
            self.assertEqual(netns.cgroup2_mount(path),
                             '/sys/fs/cgroup/unified')
            with open(path, 'w') as f:
                f.write('30 23 0:26 / /sys/fs/cgroup rw - tmpfs tmpfs ro\n')
            self.assertIsNone(netns.cgroup2_mount(path))
        finally:
            shutil.rmtree(tmp)

    @unittest.skipUnless(os.geteuid() == 0 and shutil.which('nft') and
                         shutil.which('unshare'), 'needs root and nft')
    def test_rules_load_in_a_scratch_namespace(self):
        os.makedirs('/sys/fs/cgroup', exist_ok=True)
        mount = netns.cgroup2_mount()
        if not mount or not mount.startswith('/sys/fs/cgroup'):
            self.skipTest('no cgroup v2 under /sys/fs/cgroup')
        rel = os.path.relpath(mount, '/sys/fs/cgroup')
        rules = netns.netns_ruleset('.' if rel == '.' else rel, 0, 47321)
        proc = subprocess.run(['unshare', '--net', 'nft', '-c', '-f', '-'],
                              input=rules, universal_newlines=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stderr)


class TorWrapper(unittest.TestCase):

    def test_wrapper_joins_cgroup_records_argv_and_execs(self):
        tmp = tempfile.mkdtemp()
        try:
            procs = os.path.join(tmp, 'cgroup.procs')
            argv_log = os.path.join(tmp, 'argv.txt')
            fake_tor = os.path.join(tmp, 'tor')
            with open(fake_tor, 'w') as f:
                f.write('#!/bin/sh\necho "tor pid $$ args $*"\n')
            os.chmod(fake_tor, 0o755)
            wrapper = os.path.join(tmp, 'wrapper')
            with open(wrapper, 'w') as f:
                f.write(netns.tor_wrapper_script(procs, fake_tor, argv_log))
            os.chmod(wrapper, 0o755)
            out = subprocess.run([wrapper, '-f', '/x/torrc',
                                  '__OwningControllerProcess', '42'],
                                 stdout=subprocess.PIPE,
                                 universal_newlines=True).stdout
            with open(procs) as f:
                pid = f.read().strip()
            self.assertEqual(out.strip(), 'tor pid %s args -f /x/torrc '
                             '__OwningControllerProcess 42' % pid)
            with open(argv_log) as f:
                self.assertEqual(f.read().split('\n')[:2], ['-f',
                                                            '/x/torrc'])
            with self.assertRaises(ValueError):
                netns.tor_wrapper_script("/a'b", fake_tor, argv_log)
        finally:
            shutil.rmtree(tmp)

    def test_launcher_rejects_bad_arguments(self):
        launcher = netns.LAUNCHER
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        for args in ([], ['p', 'r', 'n', 'x', '0', '-', '-', '--', 'true'],
                     ['p', 'r', 'n', '0', '0', '3', '4', '--', 'true']):
            proc = subprocess.run(['bash', launcher] + args,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, cwd=self.tmp)
            self.assertEqual(proc.returncode, 64, args)

    @unittest.skipUnless(os.geteuid() == 0 and shutil.which('unshare') and
                         os.path.exists('/etc/resolv.conf'), 'needs root')
    def test_launcher_mounts_stay_private(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        resolv = os.path.join(tmp, 'resolv.conf')
        nss = os.path.join(tmp, 'nsswitch.conf')
        with open(resolv, 'w') as f:
            f.write('nameserver 127.0.0.1 # moltleak-test\n')
        with open(nss, 'w') as f:
            f.write('hosts: files dns\n')
        with open('/etc/resolv.conf') as f:
            before = f.read()
        proc = subprocess.run(
            ['bash', netns.LAUNCHER, os.path.join(tmp, 'procs'), resolv, nss,
             '0', '0', '-', '-', '--', 'cat', '/etc/resolv.conf'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('moltleak-test', proc.stdout)
        with open('/etc/resolv.conf') as f:
            self.assertEqual(f.read(), before)
        with open(os.path.join(tmp, 'procs')) as f:
            self.assertTrue(f.read().strip().isdigit())


class Servers(unittest.TestCase):

    def start(self, *args):
        proc = subprocess.Popen([sys.executable, '-I', ENTRY, 'service'] +
                                list(args), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.addCleanup(proc.stderr.close)
        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        line = proc.stdout.readline()
        self.assertEqual(line.strip(), b'ready', proc.stderr.read(2000)
                         if not line else line)
        return proc

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def read_log(self, name, wait_for=1):
        path = os.path.join(self.tmp, name)
        deadline = time.time() + 5
        lines = []
        while time.time() < deadline:
            if os.path.exists(path):
                with open(path) as f:
                    lines = [json.loads(x) for x in f if x.strip()]
                if len(lines) >= wait_for:
                    break
            time.sleep(0.05)
        return lines

    def test_dns_canary_logs_and_answers_nxdomain(self):
        port = free_port(socket.SOCK_DGRAM)
        self.start('dns', '--listen', '127.0.0.1', '--port', str(port),
                   '--log', os.path.join(self.tmp, 'dns.jsonl'))
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(5)
        s.sendto(g.dns_query('leak-abc123.moltleak.test', 28), ('127.0.0.1',
                                                                port))
        reply, _ = s.recvfrom(512)
        s.close()
        msg = dissect.parse_dns(reply)
        self.assertTrue(msg.is_response)
        self.assertEqual(msg.rcode, 3)
        self.assertEqual(msg.questions[0].name, 'leak-abc123.moltleak.test')
        t = socket.create_connection(('127.0.0.1', port), timeout=5)
        q = g.dns_query('tcp.example')
        t.sendall(struct.pack('!H', len(q)) + q)
        n = struct.unpack('!H', t.recv(2))[0]
        self.assertEqual(dissect.parse_dns(t.recv(n)).rcode, 3)
        t.close()
        log = self.read_log('dns.jsonl', 2)
        self.assertEqual([(e['qname'], e['qtype'], e['proto']) for e in log],
                         [('leak-abc123.moltleak.test', 'AAAA', 'udp'),
                          ('tcp.example', 'A', 'tcp')])

    def test_nxdomain_reply_edge_cases(self):
        self.assertIsNone(services.nxdomain_reply(b'\x00' * 4))
        reply = services.nxdomain_reply(g.dns_query('a.b'))
        self.assertEqual(reply[:2], b'\x12\x34')
        response = bytearray(g.dns_query('a.b'))
        response[2] |= 0x80
        self.assertIsNone(services.nxdomain_reply(bytes(response)))

    def test_loopback_canary_and_proxy(self):
        lport = free_port()
        self.start('loopback', '--port', str(lport), '--log',
                   os.path.join(self.tmp, 'lo.jsonl'))
        c = socket.create_connection(('127.0.0.1', lport), timeout=5)
        c.sendall(b'GET /probe HTTP/1.1\r\nHost: x\r\n\r\n')
        self.assertIn(b'404', c.recv(100))
        c.close()
        self.assertEqual(self.read_log('lo.jsonl')[0]['first_line'],
                         'GET /probe HTTP/1.1')
        pport = free_port()
        self.start('proxy', '--listen', '127.0.0.1', '--port', str(pport),
                   '--log', os.path.join(self.tmp, 'proxy.jsonl'))
        c = socket.create_connection(('127.0.0.1', pport), timeout=5)
        c.sendall(b'CONNECT abc.onion:443 HTTP/1.1\r\nHost: abc.onion:443'
                  b'\r\n\r\n')
        self.assertIn(b'502', c.recv(100))
        c.close()
        entry = self.read_log('proxy.jsonl')[0]
        self.assertEqual((entry['method'], entry['target']),
                         ('CONNECT', 'abc.onion:443'))

    def test_page_server(self):
        cfg = os.path.join(self.tmp, 'pages.json')
        with open(cfg, 'w') as f:
            json.dump({'pages_dir': os.path.join(KIT, 'pages'),
                       'canary': 'moltleak.test',
                       'values': {'BASE_HTTP': 'http://x.onion',
                                  'ONION_A': 'x"<.onion', 'LPORT': 47321},
                       'tests': {'abcdef1234': {'id': 'webrtc',
                                                'ipv4': '198.51.100.12',
                                                'ipv6': '2001:db8::12'}}}, f)
        port = free_port()
        self.start('pages', '--config', cfg, '--log',
                   os.path.join(self.tmp, 'server.jsonl'), '--http',
                   '127.0.0.1:%d' % port)
        base = 'http://127.0.0.1:%d' % port

        def get(path, headers=None):
            req = urllib.request.Request(base + path, headers=headers or {})
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status, dict(r.headers), r.read().decode()
            except urllib.error.HTTPError as e:
                return e.code, dict(e.headers), e.read().decode()

        status, headers, body = get('/t/abcdef1234/dnsprefetch.html')
        self.assertEqual(status, 200)
        self.assertIn('href="//dnsprefetch-abcdef1234.moltleak.test"', body)
        self.assertIn('data-cip4="198.51.100.12"', body)
        self.assertIn('data-onion-a="x&quot;&lt;.onion"', body)
        self.assertNotIn('{{', body.split('-->')[-1])
        status, headers, _ = get('/t/abcdef1234/csp.html')
        self.assertIn('report-uri https://cspreport-abcdef1234.moltleak.test',
                      headers['Content-Security-Policy'])
        status, headers, _ = get('/t/abcdef1234/charset.html')
        self.assertEqual(headers['Content-Type'], 'text/html')
        status, _, body = get('/t/abcdef1234/sw.js')
        self.assertIn('https://sw-abcdef1234.moltleak.test/', body)
        for bad in ('/t/ab/basic.html', '/t/abcdef1234/../leaktest.py',
                    '/static/..%2fleaktest.py', '/t/ABCDEF1234/basic.html'):
            self.assertEqual(get(bad)[0], 404, bad)
        status, _, body = get('/headers', {'DNT': '1', 'Sec-CH-UA': 'x'})
        self.assertEqual(json.loads(body)['dnt'], '1')
        self.assertEqual(get('/canary/beacon-abcdef1234')[0], 204)
        # WebSocket handshake
        key = base64.b64encode(b'0123456789abcdef').decode()
        s = socket.create_connection(('127.0.0.1', port), timeout=5)
        s.sendall(('GET /ws/abcdef1234 HTTP/1.1\r\nHost: x\r\nUpgrade: '
                   'websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '
                   '%s\r\nSec-WebSocket-Version: 13\r\n\r\n' % key).encode())
        data = b''
        while b'hello' not in data:
            chunk = s.recv(1024)
            if not chunk:
                break
            data += chunk
        s.close()
        accept = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        self.assertIn(b'101 Switching Protocols', data)
        self.assertIn(accept, data)
        log = self.read_log('server.jsonl', 9)
        canary = [e for e in log if e['path'] == '/canary/beacon-abcdef1234']
        self.assertEqual(canary[0]['status'], 204)
        hdr = [e for e in log if e['path'] == '/headers'][0]
        self.assertEqual(hdr['client_hints'], {'sec-ch-ua': 'x'})


class FakeBrowser(object):
    """Speaks the DevTools pipe protocol well enough for the client."""

    def __init__(self):
        self.cmd_r, self.cmd_w = os.pipe()
        self.resp_r, self.resp_w = os.pipe()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def send(self, obj):
        os.write(self.resp_w, json.dumps(obj).encode() + b'\0')

    def loop(self):
        buf = b''
        while True:
            chunk = os.read(self.cmd_r, 4096)
            if not chunk:
                break
            buf += chunk
            while b'\0' in buf:
                raw, buf = buf.split(b'\0', 1)
                msg = json.loads(raw)
                if msg['method'] == 'Runtime.evaluate':
                    expr = msg['params']['expression']
                    if expr == 'boom':
                        self.send({'id': msg['id'], 'result': {
                            'result': {}, 'exceptionDetails': {
                                'text': 'Uncaught', 'exception': {
                                    'description': 'Error: boom'}}}})
                    else:
                        self.send({'id': msg['id'], 'result': {
                            'result': {'value': len(expr)}}})
                elif msg['method'] == 'Page.navigate':
                    self.send({'method': 'Page.loadEventFired',
                               'sessionId': msg.get('sessionId'),
                               'params': {}})
                    self.send({'id': msg['id'], 'result': {}})
                elif msg['method'] == 'Fail.me':
                    self.send({'id': msg['id'], 'error': {
                        'message': 'nope'}})
                elif msg['method'] == 'Close.pipe':
                    os.close(self.resp_w)
                    return
                else:
                    self.send({'id': msg['id'], 'result': {'echo':
                                                           msg['method']}})


class DevToolsPipe(unittest.TestCase):

    def test_client(self):
        fake = FakeBrowser()
        client = cdp.Cdp(fake.cmd_w, fake.resp_r)
        self.assertEqual(client.call('Target.getTargets')['echo'],
                         'Target.getTargets')
        self.assertEqual(client.evaluate('s1', '12345'), 5)
        with self.assertRaises(cdp.CdpError):
            client.evaluate('s1', 'boom')
        with self.assertRaises(cdp.CdpError):
            client.call('Fail.me')
        self.assertIsNone(client.navigate('s1', 'https://x.example/'))
        self.assertIsNone(client.wait_event(lambda e: False, timeout=0.2))
        with self.assertRaises(cdp.CdpClosed):
            client.call('Close.pipe', timeout=5)
        deadline = time.time() + 5
        while not client.closed and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(client.closed)
        with self.assertRaises(cdp.CdpClosed):
            client.call('Target.getTargets')


if __name__ == '__main__':
    unittest.main()
