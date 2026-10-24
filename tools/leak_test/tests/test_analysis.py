#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for the traffic classification, the uniformity rules, the run
diff and the report, against the recorded sample run in testdata/.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import copy
import hashlib
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
        self.assertEqual(len(socks), 17)
        self.assertEqual(socks[8]['host'], 'anchor-cccccc3333.moltleak.test')
        self.assertEqual(socks[8]['test'], 'dns_prefetch')
        self.assertEqual([s['test'] for s in socks[9:11]],
                         ['sandboxed_a', 'sandboxed_b'])
        # The SOCKS password is recorded as a digest only.
        self.assertEqual(socks[0]['password_tag'],
                         hashlib.sha256(b'x').hexdigest()[:16])
        self.assertNotIn('password', socks[0])
        self.assertEqual(len({s['password_tag'] for s in socks[11:]}), 4)
        # The certificate tests: by window, or by the nonce in the AIA host
        # (the second site's certificate names the first test's nonce).
        self.assertEqual([(s['test'], s['port']) for s in socks[2:8]],
                         [('cert_fetch', 8443), ('cert_fetch', 80),
                          ('cert_fetch', 80), ('cert_fetch_b', 8443),
                          ('cert_fetch_b', 80), ('cert_fetch', 80)])
        # Loopback packets appear twice in `any` captures; counted once.
        self.assertEqual(self.traffic['capture_stats']['loopback_packets'],
                         34)

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
        self.assertEqual(status['identity.sandboxed_circuits'], 'PASS')
        self.assertEqual(status['identity.cert_fetch_circuits'], 'PASS')
        self.assertEqual(status['identity.session_circuits'], 'PASS')
        self.assertEqual(status['identity.new_identity'], 'PASS')
        self.assertEqual(status['identity.fail_closed'], 'PASS')
        self.assertEqual(status['identity.partitioning_override_refused'],
                         'PASS')
        self.assertEqual(status['identity.policy_change_closes'], 'PASS')
        self.assertEqual(status['net.autofill_server'], 'PASS')
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


class IdentityChecks(unittest.TestCase):
    """The New Identity and sandboxed-page judgements, failing cases."""

    GOOD = {'result': {'running': True}, 'old_context': 'C1',
            'new_context': 'C2', 'cookie_after': False,
            'canvas_before': '1', 'canvas_after': '2', 'hold_page': True,
            'old_pages_left': 0, 'second_result': {'running': True},
            'second_after_first_s': 10.2, 'second_context': 'C3'}

    def judge(self, **changes):
        data = dict(self.GOOD)
        data.update(changes)
        return report._judge_new_identity(data)

    def test_new_identity(self):
        self.assertEqual(self.judge()[0], report.PASS)
        # A "Leave site?" handler kept the old identity's page open.
        status, ev = self.judge(old_pages_left=1)
        self.assertEqual(status, report.FAIL)
        self.assertIn('still open', ' '.join(ev))
        # The second window opened before Tor could switch circuits.
        status, ev = self.judge(second_after_first_s=1.5)
        self.assertEqual(status, report.FAIL)
        self.assertIn('before Tor could switch', ' '.join(ev))
        self.assertEqual(self.judge(second_context=None)[0], report.FAIL)
        self.assertEqual(
            self.judge(second_result={'error': 'x'})[0], report.FAIL)
        # Runs from before these fields existed are judged as before.
        data = dict(self.GOOD)
        for key in ('hold_page', 'old_pages_left', 'second_result',
                    'second_after_first_s', 'second_context'):
            del data[key]
        self.assertEqual(report._judge_new_identity(data)[0], report.PASS)

    def partition_runs(self, **changes):
        runs = []
        for f in ('PartitionConnectionsByNetworkIsolationKey',
                  'SplitCacheByNetworkIsolationKey',
                  'ThirdPartyStoragePartitioning'):
            run = {'feature': f, 'flag': '--disable-features=' + f,
                   'result': {'error': 'turned that off (%s)' % f},
                   'moltnet_pages': 0}
            run.update(changes.get(f, {}))
            runs.append(run)
        return report._judge_partitioning_override({'runs': runs})

    def test_partitioning_override(self):
        cache = 'SplitCacheByNetworkIsolationKey'
        self.assertEqual(self.partition_runs()[0], report.PASS)
        # MoltNet opened with the HTTP cache unsplit.
        self.assertEqual(self.partition_runs(**{cache: {
            'result': {'running': True}, 'moltnet_pages': 1}})[0],
            report.FAIL)
        # Refused, but for another reason (Tor didn't start, say): the
        # switch isn't what stopped it.
        self.assertEqual(self.partition_runs(**{cache: {
            'result': {'error': 'Tor could not start.'}}})[0], report.FAIL)
        # The harness itself failed.
        self.assertEqual(self.partition_runs(**{cache: {
            'result': {'error': 'harness: SplitCacheByNetworkIsolationKey '
                                'timeout'}}})[0], report.FAIL)
        # An error, yet a MoltNet page is there.
        self.assertEqual(self.partition_runs(**{cache: {
            'moltnet_pages': 1}})[0], report.FAIL)
        self.assertEqual(report._judge_partitioning_override({})[0],
                         report.SKIP)

    POLICY_HOST = 'plc-pppppp9999.moltleak.test'
    LANGUAGES = {
        'policy': {'ForcedLanguages': ['ja'],
                   'ReduceAcceptLanguageEnabled': False},
        'moltnet_before': {'languages': ['en-US', 'en'],
                           'accept_language': 'en-US'},
        'moltnet_after': {'languages': ['en-US', 'en'],
                          'accept_language': 'en-US'},
        'regular_before': {'languages': ['de-DE', 'de'],
                           'accept_language': 'de-DE'},
        'regular_after': {'languages': ['ja', 'de-DE', 'de'],
                          'accept_language': 'ja,de-DE;q=0.9,de;q=0.8'},
        'pages_before': 1, 'pages_kept': 1, 'written': 140.0,
        'removed': 160.0}

    def policy_live(self, events=(), socks=(), skipped=None, **changes):
        """The checks of the policy_live phase (100 s to 136 s), for a
        phase whose data is the sample's with `changes`."""
        data = {'flags': ['--enable-features=AutofillServerCommunication,'
                          'ReduceAcceptLanguage'],
                'autofill_start': 101.0, 'autofill_end': 111.0,
                'autofill_nav_error': None, 'canary_host': self.POLICY_HOST,
                'moltnet_pages_before': 1, 'policy': {'ProxyMode': 'direct'},
                'policy_written': 115.0, 'closed_after_s': 6.5,
                'moltnet_pages_after': 0,
                'reopen_with_policy': {'error': 'A policy or command-line '
                                                'flag controls the proxy'},
                'pages_with_policy': 0, 'policy_removed': 124.0,
                'reopen_without_policy': {'running': True},
                'reopened': True, 'languages': self.LANGUAGES}
        data.update(changes)
        phase = {'name': 'policy_live', 'start': 100.0, 'end': 136.0,
                 'data': data}
        if skipped:
            phase = {'name': 'policy_live', 'skipped': skipped}
        tr = {'events': [dict({'first': 120.0, 'kind': 'dns', 'dst': None,
                               'dport': 53, 'qtype': 'A', 'count': 1,
                               'detail': None, 'reason': 'x',
                               'sources': ['kit resolver']}, **e)
                         for e in events],
              'socks': [dict({'port': 443}, **s) for s in socks]}
        checks = report._policy_live_checks({'phases': [phase]}, tr)
        return {c['id']: c['status'] for c in checks}

    def test_policy_live(self):
        closes, autofill = ('identity.policy_change_closes',
                            'net.autofill_server')
        ok = self.policy_live(socks=[{'ts': 113.0, 'host': self.POLICY_HOST},
                                     {'ts': 117.0, 'host': self.POLICY_HOST}])
        self.assertEqual(ok[closes], report.PASS)
        self.assertEqual(ok[autofill], report.PASS)
        # The windows stayed open.
        self.assertEqual(self.policy_live(closed_after_s=None,
                                          moltnet_pages_after=1)[closes],
                         report.FAIL)
        # MoltNet opened although the policy was set.
        self.assertEqual(self.policy_live(
            reopen_with_policy={'running': True}, pages_with_policy=1)[closes],
            report.FAIL)
        # ... or refused only because the harness failed.
        self.assertEqual(self.policy_live(
            reopen_with_policy={'error': 'harness: timeout'})[closes],
            report.FAIL)
        # The page's canary name was looked up outside Tor (the policy's
        # DIRECT reached MoltNet's network context).
        self.assertEqual(self.policy_live(
            events=[{'qname': self.POLICY_HOST}])[closes], report.FAIL)
        # MoltNet didn't open again once the policy was gone.
        self.assertEqual(self.policy_live(reopened=False)[closes],
                         report.FAIL)
        # No MoltNet page to begin with: the check proves nothing.
        self.assertEqual(self.policy_live(moltnet_pages_before=0)[closes],
                         report.ERROR)
        # Autofill asked its server, through Tor or outside it.
        self.assertEqual(self.policy_live(socks=[{
            'ts': 104.0, 'host': 'content-autofill.googleapis.com'}])[
                autofill], report.FAIL)
        self.assertEqual(self.policy_live(events=[{
            'qname': 'chromium-i18n.appspot.com', 'first': 105.0}])[
                autofill], report.FAIL)
        # Not run.
        self.assertEqual(set(self.policy_live(skipped='--phases').values()),
                         {report.SKIP})
        self.assertEqual(len(self.policy_live(skipped='--phases')), 5)

    def test_language_policy(self):
        cid = 'identity.language_policy_ignored'

        def languages(**changes):
            return self.policy_live(languages=dict(self.LANGUAGES,
                                                   **changes))[cid]
        self.assertEqual(self.policy_live()[cid], report.PASS)
        # The regular page's languages changed, MoltNet's too.
        self.assertEqual(languages(moltnet_after={
            'languages': ['ja', 'en-US', 'en'],
            'accept_language': 'ja,en-US;q=0.9,en;q=0.8'}), report.FAIL)
        # Only the header (ReduceAcceptLanguageEnabled reached MoltNet).
        self.assertEqual(languages(moltnet_after={
            'languages': ['en-US', 'en'],
            'accept_language': 'en-US,en;q=0.9'}), report.FAIL)
        # A forced language from the start (as before the policy was set).
        ja = {'languages': ['ja', 'en-US'], 'accept_language': 'ja'}
        self.assertEqual(languages(moltnet_before=ja, moltnet_after=ja),
                         report.FAIL)
        # MoltNet closed on it.
        self.assertEqual(languages(pages_kept=0), report.FAIL)
        # The regular page didn't change: the policy didn't arrive.
        self.assertEqual(languages(regular_after=self.LANGUAGES[
            'regular_before']), report.ERROR)
        # A page didn't load.
        self.assertEqual(languages(moltnet_after={
            'error': 'net::ERR_TIMED_OUT'}), report.ERROR)
        # MoltNet didn't open again for the step.
        self.assertEqual(self.policy_live(languages=None)[cid], report.SKIP)
        self.assertEqual(self.policy_live(
            languages=None, reopened=False)[cid], report.SKIP)

    INCOGNITO = {
        'flags': ['--auto-open-devtools-for-tabs'], 'nav_error': None,
        'devtools_pages': 1, 'devtools_own_window': True,
        'hold_page': True, 'dialog': 'beforeunload',
        'moltnet_pages_before': 2, 'off_written': 210.0,
        'closed_after_s': 5.5, 'pages_left': [],
        'reopen_off': {'error': 'Incognito is turned off '
                                '(IncognitoModeAvailability or parental '
                                'controls), and MoltNet windows follow it'},
        'pages_off': 0, 'off_removed': 230.0,
        'reopen_after_off': {'running': True}, 'reopened_after_off': True,
        'pages_before_forced': 2, 'forced_written': 250.0,
        'pages_kept_forced': 2, 'open_forced': {'running': True},
        'opened_forced': True}

    def incognito(self, error=None, skipped=None, **changes):
        data = dict(self.INCOGNITO, **changes)
        phase = {'name': 'policy_live_incognito', 'start': 200.0,
                 'end': 280.0, 'data': data}
        if error:
            phase['error'] = error
        if skipped:
            phase = {'name': 'policy_live_incognito', 'skipped': skipped}
        checks = report._incognito_policy_checks({'phases': [phase]})
        return {c['id']: c['status'] for c in checks}

    def test_incognito_policy(self):
        off, forced = ('identity.incognito_off_closes',
                       'identity.incognito_forced_allows')
        self.assertEqual(self.incognito(), {off: report.PASS,
                                            forced: report.PASS})
        # A window (the page waiting on "Leave site?", or DevTools) stayed.
        self.assertEqual(self.incognito(
            closed_after_s=None,
            pages_left=['devtools://devtools/bundled/devtools_app.html'])[
                off], report.FAIL)
        # MoltNet opened with Incognito off.
        self.assertEqual(self.incognito(reopen_off={'running': True},
                                        pages_off=1)[off], report.FAIL)
        # Refused, but not saying it is Incognito.
        self.assertEqual(self.incognito(reopen_off={
            'error': 'MoltNet could not start'})[off], report.FAIL)
        # Refused only because the harness failed.
        self.assertEqual(self.incognito(reopen_off={
            'error': 'harness: Incognito timeout'})[off], report.FAIL)
        # Didn't open again without the policy.
        self.assertEqual(self.incognito(reopened_after_off=False)[off],
                         report.FAIL)
        # No DevTools window to close: the check proves less than it says.
        self.assertEqual(self.incognito(devtools_pages=0)[off],
                         report.ERROR)
        # Incognito only: MoltNet closed, or didn't open.
        self.assertEqual(self.incognito(pages_kept_forced=1)[forced],
                         report.FAIL)
        self.assertEqual(self.incognito(open_forced={
            'error': 'Incognito is turned off'}, opened_forced=False)[forced],
            report.FAIL)
        self.assertEqual(self.incognito(pages_before_forced=0)[forced],
                         report.ERROR)
        # The harness stopped before it got there.
        partial = dict((k, v) for k, v in self.INCOGNITO.items()
                       if k in ('flags', 'devtools_pages'))
        phase = {'name': 'policy_live_incognito', 'start': 200.0,
                 'end': 280.0, 'data': partial, 'error': 'MoltNet did not '
                 'open'}
        self.assertEqual(
            {c['id']: c['status'] for c in
             report._incognito_policy_checks({'phases': [phase]})},
            {off: report.ERROR, forced: report.ERROR})
        # Not run.
        self.assertEqual(self.incognito(skipped='--phases'),
                         {off: report.SKIP, forced: report.SKIP})
        self.assertEqual(
            {c['id']: c['status'] for c in
             report._incognito_policy_checks({'phases': []})},
            {off: report.SKIP, forced: report.SKIP})

    def socks(self, *entries):
        return {'socks': [{'test': t, 'host': h, 'username': u}
                          for t, h, u in entries]}

    def test_sandboxed_circuits(self):
        a, b = 'a' * 55 + 'd.onion', 'b' * 55 + 'd.onion'
        ok = report._sandboxed_circuit_check(self.socks(
            ('sandboxed_a', a, 'opaque null [internally: (1) derived from a]'),
            ('sandboxed_b', b, 'opaque null [internally: (2) derived from b]'),
        ))
        self.assertEqual(ok['status'], report.PASS)
        shared = report._sandboxed_circuit_check(self.socks(
            ('sandboxed_a', a, 'http://' + a), ('sandboxed_a', a, 'opaque'),
            ('sandboxed_b', b, 'https://' + b), ('sandboxed_b', b, 'opaque')))
        self.assertEqual(shared['status'], report.FAIL)
        one = report._sandboxed_circuit_check(self.socks(
            ('sandboxed_a', a, 'opaque')))
        self.assertEqual(one['status'], report.SKIP)

    def session_run(self, *streams, marks=(5.0,)):
        """`streams`: (seconds, host, username, password tag)."""
        manifest = {'phases': [
            {'name': 'identity', 'start': 0.0, 'end': 10.0,
             'data': {'session_marks': list(marks)}},
            {'name': 'fail_closed', 'start': 10.0, 'end': 20.0, 'data': {}}]}
        tr = {'socks': [{'ts': ts, 'host': h, 'username': u,
                         'password_tag': p} for ts, h, u, p in streams]}
        return report._session_circuit_check(manifest, tr)

    def test_session_circuits(self):
        a, b = 'a' * 55 + 'd.onion', 'b' * 55 + 'd.onion'
        ua, ub = 'http://' + a, 'https://' + b
        ok = self.session_run((1, a, ua, 'p1'), (2, b, ub, 'p1'),
                              (6, a, ua, 'p2'))
        self.assertEqual(ok['status'], report.PASS)
        # The new session reused the old credentials: the old circuits.
        same = self.session_run((1, a, ua, 'p1'), (6, a, ua, 'p1'))
        self.assertEqual(same['status'], report.FAIL)
        self.assertIn('reused', ' '.join(same['evidence']))
        # Any overlap fails, also next to new credentials.
        mixed = self.session_run((1, a, ua, 'p1'), (6, a, ua, 'p2'),
                                 (7, a, ua, 'p1'))
        self.assertEqual(mixed['status'], report.FAIL)
        no_password = self.session_run((1, a, ua, None), (6, a, ua, None))
        self.assertEqual(no_password['status'], report.FAIL)
        # Several marks: each session against the one before it.
        chain = self.session_run((1, a, ua, 'p1'), (4, a, ua, 'p2'),
                                 (7, a, ua, 'p2'), marks=(3.0, 6.0))
        self.assertEqual(chain['status'], report.FAIL)
        # Nothing to compare: another site after the mark, a connection
        # outside the phase, or no marks.
        self.assertEqual(self.session_run(
            (1, a, ua, 'p1'), (6, b, ub, 'p2'))['status'], report.SKIP)
        self.assertEqual(self.session_run(
            (1, a, ua, 'p1'), (12, a, ua, 'p1'))['status'], report.SKIP)
        self.assertEqual(self.session_run(
            (1, a, ua, 'p1'), (6, a, ua, 'p1'), marks=())['status'],
            report.SKIP)

    def cert_run(self, *streams):
        """`streams`: (seconds, host, port, username)."""
        manifest = {'tests': [
            {'id': 'cert_fetch', 'start': 10.0, 'settle_end': 18.0},
            {'id': 'cert_fetch_b', 'start': 18.0, 'settle_end': 26.0},
            {'id': 'onion_http', 'start': 2.0, 'settle_end': 8.0}]}
        tr = {'socks': [{'ts': ts, 'host': h, 'port': p, 'username': u}
                        for ts, h, p, u in streams]}
        return report._cert_fetch_circuit_check(manifest, tr)

    def test_cert_fetch_circuits(self):
        a, b = 'a' * 55 + 'd.onion', 'b' * 55 + 'd.onion'
        pages = ((3, a, 80, 'http://' + a), (11, a, 8443, 'https://' + a),
                 (19, b, 8443, 'https://' + b))
        ok = self.cert_run(*pages + (
            (12, a, 80, 'opaque (1)'), (12.1, 'aia-x.test', 80, 'opaque (2)'),
            (20, a, 80, 'opaque (3)'), (20.1, 'aia-x.test', 80, 'opaque (4)')))
        self.assertEqual(ok['status'], report.PASS)
        # Keyed by the AIA URL's site: the page's username, shared by both.
        by_site = self.cert_run(*pages + (
            (12, a, 80, 'http://' + a), (20, a, 80, 'http://' + a)))
        self.assertEqual(by_site['status'], report.FAIL)
        self.assertIn('shared', ' '.join(by_site['evidence']))
        self.assertIn('pages use', ' '.join(by_site['evidence']))
        # One key for all certificate fetches.
        one_key = self.cert_run(*pages + (
            (12, a, 80, 'opaque (1)'), (20, a, 80, 'opaque (1)')))
        self.assertEqual(one_key['status'], report.FAIL)
        # The second site's check made no request of its own.
        cached = self.cert_run(*pages + ((12, a, 80, 'opaque (1)'),))
        self.assertEqual(cached['status'], report.FAIL)
        no_user = self.cert_run(*pages + ((12, a, 80, None),
                                          (20, a, 80, 'opaque (2)')))
        self.assertEqual(no_user['status'], report.FAIL)
        # Not enough to judge.
        self.assertEqual(self.cert_run(*pages)['status'], report.SKIP)
        self.assertEqual(self.cert_run(
            (11, a, 8443, 'https://' + a), (12, a, 80, 'opaque (1)'))[
                'status'], report.SKIP)

    def client_cert_run(self, handshakes, target='moltnet'):
        """`handshakes`: (seconds, port, ok, client_cert)."""
        test = {'id': 'client_cert', 'start': 10.0, 'settle_end': 14.0}
        manifest = {'target': target, 'tests': [test]}
        log = [{'event': 'tls_handshake', 'ts': ts, 'port': port, 'ok': ok,
                'client_cert': cert, 'error': None if ok else 'EOF'}
               for ts, port, ok, cert in handshakes]
        # Requests and other ports' handshakes don't count.
        log.append({'ts': 11.0, 'port': 28445, 'path': '/t/x/basic.html'})
        log.append({'event': 'tls_handshake', 'ts': 11.0, 'port': 28444,
                    'ok': False})
        return report._client_cert_check(manifest, test, log)

    def test_client_cert(self):
        # MoltNet: every handshake completes, no certificate.
        ok = self.client_cert_run([(10.5, 28445, True, False),
                                   (12.0, 28445, True, False)])
        self.assertEqual(ok['status'], report.PASS)
        # Chromium's behaviour: the first handshake dropped to ask, then a
        # second connection.
        retried = self.client_cert_run([(10.5, 28445, False, False),
                                        (11.5, 28445, True, False)])
        self.assertEqual(retried['status'], report.FAIL)
        self.assertIn('dropped a handshake', ' '.join(retried['evidence']))
        sent = self.client_cert_run([(10.5, 28445, True, True)])
        self.assertEqual(sent['status'], report.FAIL)
        # Outside the test's window, or no handshake at all: no verdict.
        self.assertEqual(self.client_cert_run(
            [(30.0, 28445, False, False)])['status'], report.SKIP)
        # A regular window is reported, not judged.
        regular = self.client_cert_run([(10.5, 18445, False, False),
                                        (11.0, 18445, True, False)],
                                       target='regular')
        self.assertEqual(regular['status'], report.SKIP)
        self.assertIn('1 dropped', regular['evidence'][0])


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
        rule = ('scrollbar.overlay', 'per_os_eq', ex.OVERLAY_SCROLLBARS, '')
        self.assertEqual(self.check(geo, rule)['status'], 'PASS')
        self.assertEqual(self.check(geo, rule, 'Windows')['status'], 'PASS')
        self.assertEqual(self.check(geo, rule, 'macOS')['status'], 'FAIL')
        overlay = dict(geo, **{'scrollbar.overlay': True})
        self.assertEqual(self.check(overlay, rule, 'macOS')['status'], 'PASS')
        self.assertEqual(self.check(overlay, rule)['status'], 'FAIL')
        self.assertEqual(self.check(overlay, rule, 'unknown')['status'],
                         'INFO')

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
                                     'aia_b', 'clientauth', 'local'))
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
