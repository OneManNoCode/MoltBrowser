#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the traffic classification, the uniformity rules, the run
diff and the report, against the recorded sample run in testdata/.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import copy
import json
import os
import shutil
import tempfile
import unittest

from leakkit import catalog
from leakkit import expectations as ex
from leakkit import report
from leakkit import traffic
from leakkit import uniformity
from tests import make_sample_run as sample
from tests import pcapgen as g

HERE = os.path.dirname(os.path.abspath(__file__))
TESTDATA = os.path.join(HERE, 'testdata')


def by(events, **kw):
    return [e for e in events if all(e.get(k) == v for k, v in kw.items())]


class SampleRun(unittest.TestCase):
    """The committed sample run is what make_sample_run.py writes, and the
    report built from it is the committed one."""

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(sample.OUT, 'manifest.json')) as f:
            cls.manifest = json.load(f)
        cls.traffic = traffic.analyze(cls.manifest, sample.OUT)

    def test_generator_reproduces_committed_files(self):
        for rel, data in sample.files().items():
            mode = 'rb' if isinstance(data, bytes) else 'r'
            with open(os.path.join(sample.OUT, rel), mode) as f:
                self.assertEqual(f.read(), data, rel)

    def test_report_matches_golden(self):
        js, md = sample.golden_report()
        with open(os.path.join(TESTDATA, 'sample_report.json')) as f:
            self.assertEqual(json.loads(f.read()), json.loads(js))
        with open(os.path.join(TESTDATA, 'sample_report.md')) as f:
            self.assertEqual(f.read(), md)

    def test_canary_name_and_ip_attributed_to_their_test(self):
        ev = self.traffic['events']
        stun = by(ev, qname='stun-bbbbbb2222.moltleak.test', qtype='A')
        self.assertEqual(len(stun), 1)
        self.assertEqual(stun[0]['category'], traffic.LEAK)
        self.assertEqual((stun[0]['test'], stun[0]['attribution']),
                         ('webrtc', 'nonce'))
        self.assertEqual(sorted(stun[0]['sources']),
                         ['firewall log', 'kit resolver'])
        ip = by(ev, dst='198.51.100.12')
        self.assertEqual((ip[0]['test'], ip[0]['attribution']),
                         ('webrtc', 'ip'))

    def test_background_forbidden_during_a_test(self):
        sb = by(self.traffic['events'], qname='sb-ssl.google.com')[0]
        self.assertEqual((sb['category'], sb['test']),
                         (traffic.LEAK, 'download'))
        bg = by(self.traffic['events'], qname='update.googleapis.com')[0]
        self.assertEqual(bg['category'], traffic.BACKGROUND)

    def test_categories(self):
        ev = self.traffic['events']
        self.assertEqual(by(ev, qname='accounts.google.com')[0]['category'],
                         traffic.FORBIDDEN)
        self.assertEqual(by(ev, qname='weird.example.org')[0]['category'],
                         traffic.UNEXPLAINED)
        onion = [e for e in ev if (e['qname'] or '').endswith('.onion')]
        self.assertEqual(onion[0]['category'], traffic.LEAK)
        self.assertIsNone(onion[0]['test'])
        self.assertEqual(by(ev, kind='mdns')[0]['category'], traffic.LEAK)
        joined = by(ev, kind='multicast')[0]
        self.assertEqual((joined['dst'], joined['category']),
                         ('224.0.0.251', traffic.LEAK))
        local = by(ev, kind='local')
        self.assertEqual(local[0]['test'], 'localhost_lan')

    def test_tor_escaped_and_socks(self):
        self.assertEqual([(d['dst'], d['dport']) for d in
                          self.traffic['tor_destinations']],
                         [('192.0.2.50', 9001), ('192.0.2.51', 443)])
        self.assertEqual([(e['dst'], e['dport']) for e in
                          self.traffic['escaped']], [('203.0.113.99', 80)])
        socks = self.traffic['socks']
        self.assertEqual(len(socks), 3)
        self.assertEqual(socks[2]['host'], 'anchor-cccccc3333.moltleak.test')
        self.assertEqual(socks[2]['test'], 'dns_prefetch')
        # Loopback packets appear twice in `any` captures; counted once.
        self.assertEqual(self.traffic['capture_stats']['loopback_packets'], 6)

    def test_verdicts(self):
        rep = report.build(sample.OUT)
        status = {c['id']: c['status'] for c in rep['checks']}
        self.assertEqual(rep['verdict'], 'FAIL')
        self.assertEqual(status['harness.firewall'], 'FAIL')
        self.assertEqual(status['net.webrtc.traffic'], 'FAIL')
        self.assertEqual(status['net.webrtc.a1'], 'PASS')
        self.assertEqual(status['net.dns_prefetch.traffic'], 'PASS')
        self.assertEqual(status['net.download.traffic'], 'FAIL')
        self.assertEqual(status['net.localhost_lan.traffic'], 'FAIL')
        self.assertEqual(status['net.builtin_ai.traffic'], 'FAIL')
        self.assertEqual(status['net.mdns'], 'SKIP')
        self.assertEqual(status['net.unattributed_leaks'], 'FAIL')
        self.assertEqual(status['regular.proxy_no_onion'], 'FAIL')
        self.assertEqual(status['regular.onion_interstitial'], 'PASS')
        self.assertEqual(status['identity.per_site_circuits'], 'PASS')
        self.assertEqual(status['identity.fail_closed'], 'PASS')
        self.assertEqual(status['uniform.nav.hardwareConcurrency'], 'FAIL')
        self.assertEqual(status['uniform.win.innerWidth'], 'FAIL')
        self.assertEqual(status['uniform.fonts.visible'], 'FAIL')
        self.assertEqual(status['uniform.webgl.p.MAX_VARYING_VECTORS'],
                         'PASS')
        self.assertEqual(status['background.must_not_appear'], 'FAIL')

    def test_report_rebuilds_into_another_directory(self):
        tmp = tempfile.mkdtemp()
        try:
            shutil.copytree(sample.OUT, os.path.join(tmp, 'run'))
            rep = report.write(os.path.join(tmp, 'run'))
            self.assertTrue(os.path.exists(os.path.join(tmp, 'run',
                                                        'report.md')))
            with open(os.path.join(tmp, 'run', 'report.json')) as f:
                self.assertEqual(json.load(f)['verdict'], rep['verdict'])
        finally:
            shutil.rmtree(tmp)


class Classification(unittest.TestCase):

    def run_one(self, packet, prefix='drop', ts=100.0, manifest=None):
        m = manifest or {'net': {'ns_ipv4': '10.231.1.2',
                                 'gw_ipv4': '10.231.1.1'},
                         'canary': {'domain': 'moltleak.test'},
                         'tests': [{'id': 'quic', 'nonce': 'nnnnnn0000',
                                    'start': 99.0, 'end': 101.0,
                                    'settle_end': 102.0}]}
        ctx = traffic.Context(m)
        from leakkit import pcapread
        frame = g.nflog(packet, prefix)
        packets, _ = pcapread.read_bytes(g.pcap([(ts, frame)], 239))
        return traffic._event_from_blocked(ctx, packets[0])

    def test_quic_stun_lan_ssdp(self):
        quic = g.ip('10.231.1.2', '93.184.216.34', 17, g.udp(
            5000, 443, b'\xc3\x00\x00\x00\x01' + b'\0' * 30))
        self.assertIn('QUIC', self.run_one(quic)[0].reason)
        stun = g.ip('10.231.1.2', '93.184.216.34', 17,
                    g.udp(5000, 19302, b'x' * 20))
        self.assertIn('STUN', self.run_one(stun)[0].reason)
        lan = g.ip('10.231.1.2', '10.231.1.1', 6, g.tcp(5000, 80, flags=2))
        self.assertIn('gateway', self.run_one(lan)[0].reason)
        ssdp = g.ip('10.231.1.2', '239.255.255.250', 17,
                    g.udp(5000, 1900, b'M-SEARCH'))
        self.assertIn('SSDP', self.run_one(ssdp)[0].reason)
        private = g.ip('10.231.1.2', '192.168.1.1', 6,
                       g.tcp(5000, 80, flags=2))
        self.assertEqual(self.run_one(private)[0].category, traffic.LEAK)

    def test_sni_and_doh(self):
        hello = g.ip('10.231.1.2', '93.184.216.34', 6, g.tcp(
            5000, 443, g.client_hello('safebrowsing.googleapis.com')))
        ev = self.run_one(hello)[0]
        self.assertEqual(ev.category, traffic.BACKGROUND)
        doh = g.ip('10.231.1.2', '1.1.1.1', 6, g.tcp(5000, 443, flags=2))
        self.assertEqual(self.run_one(doh)[0].category, traffic.BACKGROUND)
        raw = g.ip('10.231.1.2', '93.184.216.34', 6,
                   g.tcp(5000, 443, flags=2))
        self.assertEqual(self.run_one(raw)[0].category, traffic.UNEXPLAINED)

    def test_dns_over_tcp_and_window(self):
        q = g.dns_query('cdn.example')
        pkt = g.ip('10.231.1.2', '8.8.8.8', 6, g.tcp(
            5000, 53, len(q).to_bytes(2, 'big') + q))
        ev = self.run_one(pkt, prefix='dns')[0]
        self.assertEqual((ev.qname, ev.category), ('cdn.example',
                                                   traffic.UNEXPLAINED))
        traffic._attribute_by_window(traffic.Context({
            'tests': [{'id': 'quic', 'start': 99.0, 'settle_end': 102.0}]}),
            ev)
        self.assertEqual((ev.test, ev.attribution), ('quic', 'window'))
        # An empty TCP segment to port 53 is not an event.
        ack = g.ip('10.231.1.2', '8.8.8.8', 6, g.tcp(5000, 53, flags=0x10))
        self.assertEqual(self.run_one(ack, prefix='dns'), [])

    def test_kernel_icmp(self):
        icmp = g.ip('10.231.1.2', '10.231.1.1', 1, b'\x03\x03\x00\x00' +
                    b'\0' * 24)
        self.assertEqual(self.run_one(icmp)[0].category, traffic.KERNEL)

    def test_missing_captures(self):
        out = traffic.analyze({'captures': {}, 'logs': {}}, HERE)
        self.assertEqual(out['events'], [])
        self.assertEqual(out['capture_stats']['all']['frames'], 0)


class Uniformity(unittest.TestCase):

    def check(self, values, rule, os_name='Linux'):
        return uniformity.check_rule(values, rule, os_name)

    def test_operators(self):
        geo = sample.geometry(1200, 600)
        self.assertEqual(self.check(geo, ('win.innerWidth', 'step',
                                          (200, 1400), ''))['status'], 'PASS')
        for bad in (1201, 1600, 0):
            v = dict(geo, **{'win.innerWidth': bad})
            self.assertEqual(self.check(v, ('win.innerWidth', 'step',
                                            (200, 1400), ''))['status'],
                             'FAIL', bad)
        v = dict(geo, **{'screen.width': 1000})
        self.assertEqual(self.check(v, ('screen.width', 'eq_key',
                                        'win.innerWidth', ''))['status'],
                         'FAIL')
        v = dict(geo, **{'screen.orientation.type': 'portrait-primary'})
        self.assertEqual(self.check(v, ('screen.orientation.type',
                                        'orientation', None, ''))['status'],
                         'FAIL')
        square = sample.geometry(600, 600)
        self.assertEqual(self.check(square, ('screen.orientation.type',
                                             'orientation', None, ''))[
            'status'], 'PASS')

    def test_webgl_rules(self):
        rule = ('p', 'baseline_max', 8192, '')
        self.assertEqual(self.check({'p': 8192}, rule)['status'], 'PASS')
        self.assertEqual(self.check({'p': 4096}, rule)['status'], 'PASS')
        self.assertEqual(self.check({'p': 16384}, rule)['status'], 'FAIL')
        self.assertEqual(self.check({}, rule)['status'], 'SKIP')
        rule = ('p', 'baseline_min', 256, '')
        self.assertEqual(self.check({'p': 64}, rule)['status'], 'FAIL')
        self.assertEqual(self.check({'p': 512}, rule)['status'], 'PASS')
        rule = ('r', 'range_clamp', (1.0, 255.0), '')
        self.assertEqual(self.check({'r': [1, 255]}, rule)['status'], 'PASS')
        self.assertEqual(self.check({'r': [1, 1024]}, rule)['status'],
                         'FAIL')
        self.assertEqual(self.check({'r': [0.5, 64]}, rule)['status'],
                         'FAIL')
        rule = ('v', 'range_clamp', (None, 8192), '')
        self.assertEqual(self.check({'v': [32767, 32767]}, rule)['status'],
                         'FAIL')
        rule = ('f', 'precision', (15, 15, 10), '')
        self.assertEqual(self.check({'f': [127, 127, 23]}, rule)['status'],
                         'FAIL')
        self.assertEqual(self.check({'f': [15, 15, 10]}, rule)['status'],
                         'PASS')

    def test_fonts_hints_ua(self):
        rule = ('fonts.visible', 'subset', 'per_os', '')
        self.assertEqual(self.check({'fonts.visible': ['Arial', 'noto sans']},
                                    rule)['status'], 'PASS')
        self.assertEqual(self.check({'fonts.visible': ['Ubuntu']},
                                    rule)['status'], 'FAIL')
        self.assertEqual(self.check({'fonts.visible': ['Segoe UI']}, rule,
                                    'Windows')['status'], 'INFO')
        hints = ('h', 'only_hints', ex.ALLOWED_HINTS, '')
        self.assertEqual(self.check({'h': {'sec-ch-ua': 'x'}}, hints)[
            'status'], 'PASS')
        self.assertEqual(self.check({'h': {'sec-ch-dpr': '2'}}, hints)[
            'status'], 'FAIL')
        ua = ('u', 'blank_ua', None, '')
        good = sample.GOOD_FP['uad.high_entropy']
        self.assertEqual(self.check({'u': good}, ua)['status'], 'PASS')
        self.assertEqual(self.check({'u': dict(good, architecture='x86')},
                                    ua)['status'], 'FAIL')
        self.assertEqual(self.check({'u': {'error': 'TypeError'}}, ua)[
            'status'], 'SKIP')

    def test_every_rule_has_a_doc_and_known_operator(self):
        ops = set()
        for rule in ex.GEOMETRY_RULES + ex.FULL_RULES:
            self.assertEqual(len(rule), 4, rule)
            self.assertTrue(rule[3], rule)
            ops.add(rule[1])
            res = uniformity.check_rule({}, rule, 'Linux')
            self.assertNotIn('unknown rule', res['note'], rule)
        self.assertIn('step', ops)

    def test_diff(self):
        a = [{'label': 'initial', 'dsf': 1.0, 'window': None,
              'zoom_requested': 100, 'target': 'moltnet', 'kind': 'full',
              'values': dict(sample.GOOD_FP)}]
        b = copy.deepcopy(a)
        compared, diffs = uniformity.diff_records(a, b)
        self.assertEqual((compared, diffs), (1, []))
        b[0]['values']['intl.locale'] = 'de-DE'
        b[0]['values']['storage.quota'] = 1
        b[0]['values']['canvas.hash'] = 'other'
        b[0]['values']['some.unknown'] = 1
        compared, diffs = uniformity.diff_records(a, b)
        got = sorted((d['key'], d['severity']) for d in diffs)
        self.assertEqual(got, [('intl.locale', 'fail'),
                               ('storage.quota', 'info')])

    def test_diff_reports(self):
        tmp = tempfile.mkdtemp()
        try:
            rep = report.build(sample.OUT)
            pa, pb = os.path.join(tmp, 'a.json'), os.path.join(tmp, 'b.json')
            with open(pa, 'w') as f:
                json.dump(rep, f)
            rep['fingerprints'][0]['values']['nav.hardwareConcurrency'] = 4
            with open(pb, 'w') as f:
                json.dump(rep, f)
            verdict, text, data = report.diff_reports(pa, pb)
            self.assertEqual(verdict, 'FAIL')
            self.assertIn('nav.hardwareConcurrency', text)
            verdict, _, _ = report.diff_reports(pa, pa)
            self.assertEqual(verdict, 'PASS')
        finally:
            shutil.rmtree(tmp)


class Catalog(unittest.TestCase):

    def test_tests_are_well_formed(self):
        pages = os.path.join(os.path.dirname(HERE), 'pages')
        ids = set()
        for t in catalog.ALL_TESTS:
            self.assertNotIn(t.id, ids)
            ids.add(t.id)
            if t.page:
                self.assertTrue(os.path.exists(os.path.join(pages, t.page)),
                                t.page)
            for a in t.assertions:
                self.assertEqual(len(a), 4, a)
                self.assertIn(a[1], ('eq', 'in', 'lt', 'truthy', 'falsy',
                                     'all_fail'), a)
            self.assertIn(t.origin, ('http', 'https', 'https_b', 'aia',
                                     'local'))
        self.assertEqual([s[0] for s in catalog.COMPLIANCE_STEPS],
                         list(range(1, 21)))

    def test_assertions(self):
        ev = report.eval_assertion
        self.assertEqual(ev({'a': {'b': 1}}, ('a.b', 'eq', 1, ''))[0], 'PASS')
        self.assertEqual(ev({}, ('a.b', 'eq', 1, ''))[0], 'FAIL')
        self.assertEqual(ev({'f': {'x': {'ok': False}, 'y': {'ok': True}}},
                            ('f', 'all_fail', None, ''))[0], 'FAIL')
        self.assertEqual(ev({'f': {'x': {'ok': False}, 'y': 'error'}},
                            ('f', 'all_fail', None, ''))[0], 'PASS')
        self.assertEqual(ev({'n': 5}, ('n', 'lt', 3, ''))[0], 'FAIL')
        self.assertEqual(ev({'n': True}, ('n', 'lt', 3, ''))[0], 'FAIL')


if __name__ == '__main__':
    unittest.main()
