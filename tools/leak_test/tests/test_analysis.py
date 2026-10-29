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
import struct
import subprocess
import tempfile
import unittest

from leakkit import catalog
from leakkit import expectations as ex
from leakkit import harness
from leakkit import micinput
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
        # What pages can learn about the processor, and the microphone.
        self.assertEqual(status['uniform.cpu.wasm_simd'], 'PASS')
        self.assertEqual(status['uniform.cpu.float16_nan_canonical'], 'PASS')
        self.assertEqual(status['harness.mic_processing'], 'PASS')
        self.assertEqual(status['uniform.audio.mic_settings'], 'PASS')
        self.assertEqual(status['uniform.audio.mic_processed_stable'],
                         'FAIL')
        self.assertEqual(status['uniform.audio.mic_capabilities'], 'PASS')
        self.assertEqual(status['uniform.audio.mic_system_aec'], 'PASS')
        self.assertEqual(status['uniform.audio.mic_system_aec_settings'],
                         'PASS')
        self.assertEqual(status['uniform.audio.mic_remote_only'], 'FAIL')
        self.assertEqual(status['uniform.audio.mic_remote_only_settings'],
                         'PASS')
        # The camera's menu formats, and a frame of the device's own size;
        # its ImageCapture and frame timing, with timestamps of the device's
        # own rate.
        for key in ('video.camera_settings', 'video.camera_capabilities',
                    'video.camera_hd_settings', 'video.camera_hd_frame',
                    'video.camera_device_capabilities',
                    'video.camera_overconstrained',
                    'video.camera_image_capture', 'video.camera_timing_steps',
                    'video.camera_color_space',
                    'video.camera_frame_callbacks', 'video.camera_stats'):
            self.assertEqual(status['uniform.' + key], 'PASS', key)
        self.assertEqual(status['uniform.video.camera_frame'], 'FAIL')
        self.assertEqual(status['uniform.video.camera_timing_on_grid'],
                         'FAIL')
        self.assertEqual(rep['arch'], 'x86_64')

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

    def test_nothing_held_after_end(self):
        def run(data, paths=()):
            manifest = {'phases': [{'name': 'new_identity', 'data': data}]}
            log = [{'path': p, 'status': 200} for p in paths]
            return report._held_after_end_check(manifest, log)

        good = {'held_nonce': 'endheld1', 'held_setup': {'fetch_later': True}}
        self.assertEqual(run(good)['status'], report.PASS)
        # Another nonce's canary, or another test's, isn't this one's.
        self.assertEqual(run(good, ['/canary/endheld12-later',
                                    '/canary/x-hide'])['status'],
                         report.PASS)
        for path in ('/canary/endheld1-later', '/canary/endheld1-hide?x=1'):
            c = run(good, [path])
            self.assertEqual(c['status'], report.FAIL)
            self.assertIn(path, ' '.join(c['evidence']))
        self.assertEqual(run({})['status'], report.SKIP)
        self.assertEqual(run({'held_nonce': 'endheld1', 'held_setup': {
            'error': 'x'}})['status'], report.ERROR)
        c = run({'held_nonce': 'endheld1', 'held_setup': {
            'fetch_later': False}})
        self.assertEqual(c['status'], report.PASS)
        self.assertIn('only the pagehide beacon', ' '.join(c['evidence']))

    def test_nothing_after_end_race(self):
        began = 1760000092.0

        def run(data, paths=()):
            manifest = {'phases': [{'name': 'new_identity', 'data': data}]}
            log = [{'path': p, 'status': 204} for p in paths]
            return report._race_after_end_check(manifest, log)

        after_ms = int(began * 1000) + 40
        good = {'race_nonce': 'endrace1', 'race_setup': {'started': True},
                'session_marks': [began, began + 4],
                'race_events': [
                    {'what': 'started', 't': after_ms - 900},
                    {'what': 'after', 't': after_ms, 'seq': 9,
                     'tried': ['fetch', 'keepalive', 'beacon', 'image',
                               'websocket', 'prefetch']}]}
        raced = ['/canary/endrace1-race0-1', '/canary/endrace1-race1-2']
        c = run(good, raced)
        self.assertEqual(c['status'], report.PASS)
        self.assertIn('2 racing request(s)', ' '.join(c['evidence']))
        self.assertIn('40 ms after', ' '.join(c['evidence']))
        # Another nonce's requests aren't this one's.
        self.assertEqual(run(good, [
            '/canary/endrace12after%d-fetch' % after_ms,
            '/ws/endrace12after%d' % after_ms])['status'], report.PASS)
        # Any kind started after the refusal that reached the server fails.
        for path in ('/canary/endrace1after%d-beacon' % after_ms,
                     '/ws/endrace1after%d' % after_ms):
            c = run(good, raced + [path])
            self.assertEqual(c['status'], report.FAIL)
            self.assertIn(path, ' '.join(c['evidence']))
        # Even when the page could not say so before it closed.
        self.assertEqual(run(dict(good, race_events=[]), [
            '/canary/endrace1after%d-fetch' % after_ms])['status'],
            report.FAIL)
        # A refusal before New Identity began (Tor or the server failing)
        # isn't the end: not judged, whatever reached the server.
        early = int(began * 1000) - 500
        self.assertEqual(run(dict(good, race_events=[
            {'what': 'after', 't': early}]), [
            '/canary/endrace1after%d-fetch' % early])['status'], report.SKIP)
        # The page closed before it saw a refusal: nothing to judge.
        self.assertEqual(run(dict(good, race_events=[
            {'what': 'started', 't': 1}]))['status'], report.SKIP)
        self.assertEqual(run({})['status'], report.SKIP)
        self.assertEqual(run(dict(good, race_setup={'error': 'x'}))['status'],
                         report.ERROR)
        self.assertEqual(run(dict(good, session_marks=[]))['status'],
                         report.ERROR)
        self.assertEqual(report._race_after_time('/ws/endrace1after12', 'x'),
                         None)
        self.assertEqual(report._race_after_time('/canary/xafter', 'x'), None)
        self.assertEqual(report._race_after_time('/canary/xafter7-a', 'x'), 7)

    # The racing page's script (harness.py RACE_END_JS) in node, with a
    # stand-in page whose fetches start failing: it starts each kind once,
    # at the first refusal, with paths the report reads the time from.
    RACE_PAGE = r"""
const sent = []; const said = []; let n = 0;
globalThis.location = {origin: 'http://a.onion'};
globalThis.fetch = (u) => { sent.push(u); ++n;
  return n < 8 ? Promise.resolve({}) : Promise.reject(new TypeError('x')); };
Object.defineProperty(globalThis, 'navigator', {value: {
  sendBeacon: (u) => { sent.push(u); return true; }}});
globalThis.Image = function () { return {set src(u) { sent.push(u); }}; };
globalThis.WebSocket = function (u) { sent.push(u); return {}; };
globalThis.document = {createElement: () => ({}),
                       head: {appendChild: (l) => sent.push(l.href)}};
console.log = (s) => said.push(s);
const started = eval(require('fs').readFileSync(0, 'utf8'));
setTimeout(() => {
  process.stdout.write(JSON.stringify({started, sent, said}));
}, 300);
"""

    @unittest.skipUnless(shutil.which('node'), 'node is not installed')
    def test_race_page_script(self):
        nonce = 'endrace1'
        out = subprocess.run(
            ['node', '-e', self.RACE_PAGE],
            input=harness.Run.RACE_END_JS % json.dumps(nonce),
            capture_output=True, text=True, timeout=30, check=True)
        res = json.loads(out.stdout)
        self.assertEqual(res['started'], {'started': True})
        prefix = 'moltleak-race %s ' % nonce
        said = [s[len(prefix):].split(' ', 1) for s in res['said']
                if s.startswith(prefix)]
        events = [dict(json.loads(v), what=w) for w, v in said]
        afters = [e for e in events if e['what'] == 'after']
        self.assertEqual(len(afters), 1)
        self.assertEqual(afters[0]['tried'], [
            'fetch', 'keepalive', 'beacon', 'image', 'websocket', 'prefetch'])
        after_paths = [u for u in res['sent'] if nonce + 'after' in u]
        self.assertEqual(len(after_paths), 6)
        self.assertIn('ws://a.onion/ws/%safter%d' % (nonce, afters[0]['t']),
                      after_paths)
        for u in after_paths:
            self.assertEqual(report._race_after_time(u, nonce),
                             afters[0]['t'], u)
        self.assertTrue(any('/canary/%s-race' % nonce in u
                            for u in res['sent']))

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

    def test_background_fetch_circuits(self):
        a, b = 'a' * 55 + 'd.onion', 'b' * 55 + 'd.onion'
        manifest = {'onion': {'a': a, 'b': b}}
        fa, fb = 'bgf-aaaa1111.moltleak.test', 'bgf-bbbb2222.moltleak.test'
        pages = (('bgfetch_a', a, 'https://' + a), ('cert_fetch', a,
                                                    'http://' + a),
                 ('bgfetch_b', b, 'https://' + b))

        def status(*fetches):
            return report._background_fetch_circuit_check(
                manifest, self.socks(*(pages + fetches)))['status']

        # Each site's fetch under its own site's username.
        self.assertEqual(status(('bgfetch_a', fa, 'https://' + a),
                                ('bgfetch_b', fb, 'https://' + b)),
                         report.PASS)
        # Filed under the destination (the fetched name's site).
        self.assertEqual(status(('bgfetch_a', fa, 'https://moltleak.test'),
                                ('bgfetch_b', fb, 'https://moltleak.test')),
                         report.FAIL)
        self.assertEqual(status(('bgfetch_b', fb, 'https://moltleak.test')),
                         report.FAIL)
        # Under the other site's.
        self.assertEqual(status(('bgfetch_b', fb, 'https://' + a)),
                         report.FAIL)
        # Without a username.
        self.assertEqual(status(('bgfetch_a', fa, None)), report.FAIL)
        # No fetch reached Tor, or no page request of its site to compare.
        self.assertEqual(status(), report.SKIP)
        self.assertEqual(report._background_fetch_circuit_check(
            manifest, self.socks(('bgfetch_a', fa, 'https://' + a)))[
                'status'], report.SKIP)

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

    def test_per_arch(self):
        rule = ('cpu.float16_nan_canonical', 'per_arch_eq',
                {'x86_64': True}, '')
        v = {'cpu.float16_nan_canonical': True}
        self.assertEqual(uniformity.check_rule(v, rule, 'Linux', 'x86_64')[
            'status'], 'PASS')
        self.assertEqual(uniformity.check_rule(
            {'cpu.float16_nan_canonical': False}, rule, 'Linux',
            'x86_64')['status'], 'FAIL')
        for arch in ('arm64', 'unknown', None):
            res = uniformity.check_rule(v, rule, 'Linux', arch)
            self.assertEqual(res['status'], 'INFO', arch)
            self.assertIn('same architecture', res['note'])
        self.assertEqual(uniformity.arch_of({'arch': 'aarch64'}), 'arm64')
        self.assertEqual(uniformity.arch_of({'arch': 'AMD64'}), 'x86_64')
        self.assertEqual(uniformity.arch_of(
            {'os': 'Linux-6.8.0-45-generic-x86_64-with-glibc2.39'}),
            'x86_64')
        self.assertEqual(uniformity.arch_of(
            {'os': 'Linux-6.1.0-arm64-aarch64-with-glibc2.36'}), 'arm64')
        self.assertEqual(uniformity.arch_of({}), 'unknown')
        self.assertEqual(uniformity.arch_of(None), 'unknown')

    def test_mic_record_rules(self):
        processing = {'echoCancellation': True, 'noiseSuppression': True,
                      'autoGainControl': True}
        plain = {'hash': '0f4c2a9e71b3d8565ae0c4f1', 'frames': 480}
        rec = {'label': 'mic', 'kind': 'mic', 'target': 'moltnet',
               'values': {
                   'audio.mic_processed': dict(plain),
                   'audio.mic_processed_stable': True,
                   'audio.mic_settings': dict(processing),
                   'audio.mic_capabilities': {
                       'echoCancellation': ['all', 'false', 'remote-only',
                                            'true'],
                       'autoGainControl': ['false', 'true'],
                       'noiseSuppression': ['false', 'true'],
                       'voiceIsolation': ['false']},
                   'audio.mic_system_aec': dict(plain),
                   'audio.mic_system_aec_settings': dict(
                       processing, echoCancellation='all'),
                   'audio.mic_remote_only': dict(plain),
                   'audio.mic_remote_only_settings': dict(
                       processing, echoCancellation='remote-only'),
                   # One format and generic names for every microphone.
                   'audio.mic_format': {'sampleRate': 48000,
                                        'sampleSize': 16, 'channelCount': 1,
                                        'latency': 0.01},
                   'audio.mic_format_ranges': dict(ex.MIC_FORMAT_RANGES),
                   'audio.mic_device_ranges': [dict(ex.MIC_FORMAT_RANGES)],
                   'audio.mic_names': {'labels': True, 'groups': True,
                                       'track': True,
                                       'communications': False},
                   # Recorded only: the echo canceller hears the tones.
                   'audio.mic_playback': {'hash': 'other', 'frames': 480},
                   # The camera's menu formats.
                   'video.camera_settings': dict(ex.CAMERA_SETTINGS),
                   'video.camera_capabilities': dict(ex.CAMERA_CAPABILITIES),
                   'video.camera_frame': dict(ex.CAMERA_FRAME),
                   'video.camera_hd_settings': dict(ex.CAMERA_HD_SETTINGS),
                   'video.camera_hd_frame': dict(ex.CAMERA_HD_FRAME),
                   'video.camera_device_capabilities': [
                       dict(c) for c in ex.CAMERA_DEVICE_CAPABILITIES],
                   'video.camera_overconstrained': dict(
                       ex.CAMERA_OVERCONSTRAINED),
                   # Its ImageCapture and frame timing.
                   'video.camera_image_capture': dict(
                       ex.CAMERA_IMAGE_CAPTURE),
                   'video.camera_timing_on_grid': True,
                   'video.camera_timing_steps': [1, 2],
                   'video.camera_color_space': dict(ex.CAMERA_COLOR_SPACE),
                   'video.camera_frame_callbacks': dict(
                       ex.CAMERA_FRAME_CALLBACKS),
                   'video.camera_stats': dict(ex.CAMERA_STATS)}}
        res = uniformity.check_record(rec, 'Linux', 'x86_64')
        self.assertEqual(sorted(r['key'] for r in res),
                         sorted(r[0] for r in ex.MIC_RULES))
        self.assertTrue(all(r['status'] == 'PASS' for r in res), res)
        # The same on every OS (no per-OS expectations).
        for os_name in ('Windows', 'macOS'):
            self.assertTrue(all(r['status'] == 'PASS' for r in
                                uniformity.check_record(rec, os_name)))
        self.assertNotIn('audio.mic_playback',
                         [r[0] for r in ex.MIC_RULES])
        self.assertIn('audio.mic_playback', ex.INFO_KEYS)
        values = rec['values']
        values['audio.mic_settings']['noiseSuppression'] = False
        # System echo cancellation that isn't WebRTC's gives other samples,
        # and a platform's voice isolation shows in the capabilities.
        values['audio.mic_system_aec'] = {'hash': 'other', 'frames': 480}
        values['audio.mic_capabilities']['voiceIsolation'] = ['false',
                                                              'true']
        values['audio.mic_remote_only'] = 'silent'
        values['audio.mic_remote_only_settings']['echoCancellation'] = True
        # The device's own format, its range of rates, a second microphone
        # with another range, and the device's own name.
        values['audio.mic_format']['sampleRate'] = 44100
        values['audio.mic_format_ranges']['sampleRate'] = [44100, 48000]
        values['audio.mic_device_ranges'].append(
            dict(ex.MIC_FORMAT_RANGES, channelCount=[1, 4]))
        values['audio.mic_names']['labels'] = False
        # The camera's own modes: its largest size and rate in the
        # capabilities, its native mode in the settings (resizeMode "none"
        # at 20 fps), its frame wrapped with a smaller visible part, Full HD
        # granted.
        values['video.camera_capabilities']['width'] = [1, 3840]
        values['video.camera_settings']['frameRate'] = 20
        values['video.camera_frame']['visibleRect'] = [160, 0, 960, 720]
        values['video.camera_hd_settings'].update(width=1920, height=1080)
        values['video.camera_hd_frame']['codedWidth'] = 1920
        values['video.camera_device_capabilities'][0]['frameRate'] = [1, 20]
        values['video.camera_overconstrained'] = 'granted'
        # The device's controls and photos, its own rate and colours, its
        # own drops.
        values['video.camera_image_capture'] = dict(
            ex.CAMERA_IMAGE_CAPTURE,
            track_members={'capabilities': ['focusMode', 'zoom'],
                           'settings': ['focusMode', 'zoom']},
            photo={'type': 'image/png', 'width': 1280, 'height': 720})
        values['video.camera_timing_on_grid'] = False
        values['video.camera_timing_steps'] = [1, 3]
        values['video.camera_color_space'] = dict(
            ex.CAMERA_COLOR_SPACE, primaries='bt709', transfer='bt709',
            matrix='bt709')
        values['video.camera_frame_callbacks'] = dict(
            ex.CAMERA_FRAME_CALLBACKS, media_time_on_grid=False)
        values['video.camera_stats'] = {'dropped': 2,
                                        'half_discarded': False}
        res = {r['key']: r['status'] for r in uniformity.check_record(
            rec, 'Linux')}
        for key in ('audio.mic_settings', 'audio.mic_system_aec',
                    'audio.mic_capabilities', 'audio.mic_remote_only',
                    'audio.mic_remote_only_settings', 'audio.mic_format',
                    'audio.mic_format_ranges', 'audio.mic_device_ranges',
                    'audio.mic_names', 'video.camera_settings',
                    'video.camera_capabilities', 'video.camera_frame',
                    'video.camera_hd_settings', 'video.camera_hd_frame',
                    'video.camera_device_capabilities',
                    'video.camera_overconstrained',
                    'video.camera_image_capture',
                    'video.camera_timing_on_grid',
                    'video.camera_timing_steps', 'video.camera_color_space',
                    'video.camera_frame_callbacks', 'video.camera_stats'):
            self.assertEqual(res[key], 'FAIL', key)
        self.assertEqual(res['audio.mic_system_aec_settings'], 'PASS')
        # A case whose getUserMedia() failed is reported, not compared.
        values['audio.mic_remote_only'] = {'error': 'NotReadableError',
                                           'message': ''}
        res = {r['key']: r['status'] for r in uniformity.check_record(
            rec, 'Linux')}
        self.assertEqual(res['audio.mic_remote_only'], 'SKIP')

    def test_new_probes_are_collected(self):
        # Every new key a rule or the diff covers is reported by a page.
        pages = os.path.join(os.path.dirname(HERE), 'pages', 'static')
        with open(os.path.join(pages, 'fp.js')) as f:
            fp = f.read()
        with open(os.path.join(pages, 'mic.js')) as f:
            mic = f.read()
        with open(os.path.join(pages, 'camera.js')) as f:
            camera = f.read()
        for key in ('cpu.wasm_simd', 'cpu.float16_nan',
                    'cpu.float16_nan_canonical', 'pixels.jxl',
                    'audio.stretched', 'audio.subnormal_graph',
                    'audio.subnormal_resampled'):
            self.assertIn("'%s'" % key, fp, key)
        for key in ('audio.mic_processed', 'audio.mic_processed_stable',
                    'audio.mic_settings', 'audio.mic_format',
                    'audio.mic_format_ranges', 'audio.mic_device_ranges',
                    'audio.mic_names',
                    'audio.mic_capabilities', 'audio.mic_system_aec',
                    'audio.mic_system_aec_settings', 'audio.mic_remote_only',
                    'audio.mic_remote_only_settings', 'audio.mic_playback',
                    'audio.mic_playback_tones'):
            self.assertIn("'%s'" % key, mic, key)
        # The camera's values, every one a rule.
        camera_keys = [r[0] for r in ex.MIC_RULES
                       if r[0].startswith('video.camera_')]
        self.assertEqual(len(camera_keys), 13)
        for key in camera_keys:
            self.assertIn("'%s'" % key, camera, key)
        for key in ('audio.stretched', 'pixels.jxl', 'cpu.float16_nan',
                    'audio.mic_processed', 'audio.subnormal_graph',
                    'audio.subnormal_resampled'):
            self.assertIn(key, ex.DIFF_KEYS)
        # The microphone's format is promised now, not only recorded.
        self.assertNotIn('audio.mic_sample_rate', ex.INFO_KEYS)
        self.assertNotIn("'audio.mic_sample_rate'", mic)
        self.assertNotIn('cpu.wasm_simd', ex.INFO_KEYS)
        self.assertTrue(set(ex.ARCH_KEYS) <= set(ex.DIFF_KEYS) |
                        set(r[0] for r in ex.FULL_RULES))
        # The harness times the stretched audio with a user gesture, and
        # runs the microphone's playback cases after the others.
        with open(harness.__file__) as f:
            source = f.read()
        self.assertIn('mlStretchedAudio()', source)
        self.assertLess(source.index('mlMicProcessing()'),
                        source.index('mlMicPlayback()'))
        # The camera's values come after the microphone's cases, from
        # mic.html, which loads camera.js.
        self.assertLess(source.index('mlMicPlayback()'),
                        source.index('mlCameraValues()'))
        self.assertIn('window.mlCameraValues', camera)
        with open(os.path.join(os.path.dirname(pages), 'mic.html')) as f:
            self.assertIn('/static/camera.js', f.read())
        self.assertIn('window.mlStretchedAudio', fp)
        self.assertIn('window.mlMicProcessing', mic)
        self.assertIn('window.mlMicPlayback', mic)

    def test_every_rule_has_a_doc_and_known_operator(self):
        ops = set()
        for rule in ex.GEOMETRY_RULES + ex.FULL_RULES + ex.MIC_RULES:
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
        # The Float16 NaN bits are compared between runs of one
        # architecture; between x86-64 and arm64 a difference is info.
        b = copy.deepcopy(a)
        b[0]['values']['cpu.float16_nan'] = {'array_optimized': '7e7f'}
        b[0]['values']['cpu.float16_nan_canonical'] = False
        b[0]['values']['pixels.jxl'] = {'rgba': 'other'}
        for archs, severity in (((None, None), 'fail'),
                                (('x86_64', 'x86_64'), 'fail'),
                                (('x86_64', 'unknown'), 'fail'),
                                (('x86_64', 'arm64'), 'info')):
            _, diffs = uniformity.diff_records(a, b, *archs)
            got = dict((d['key'], d['severity']) for d in diffs)
            self.assertEqual(got['cpu.float16_nan'], severity, archs)
            self.assertEqual(got['cpu.float16_nan_canonical'], severity,
                             archs)
            self.assertEqual(got['pixels.jxl'], 'fail', archs)
        # Records of the microphone launch are compared too.
        mic = [{'label': 'mic', 'dsf': 1.0, 'window': None,
                'zoom_requested': 100, 'target': 'moltnet', 'kind': 'mic',
                'values': {'audio.mic_processed': {'hash': 'a'},
                           'audio.mic_format': dict(ex.MIC_FORMAT),
                           'audio.mic_system_aec': {'hash': 'a'},
                           'audio.mic_remote_only': {'hash': 'a'},
                           'audio.mic_playback': {'hash': 'c'}}}]
        mic_b = copy.deepcopy(mic)
        for key in ('audio.mic_processed', 'audio.mic_system_aec',
                    'audio.mic_remote_only', 'audio.mic_playback'):
            mic_b[0]['values'][key] = {'hash': 'b'}
        mic_b[0]['values']['audio.mic_format'] = dict(ex.MIC_FORMAT,
                                                      sampleRate=44100)
        _, diffs = uniformity.diff_records(mic, mic_b)
        self.assertEqual(sorted((d['key'], d['severity']) for d in diffs),
                         [('audio.mic_format', 'fail'),
                          ('audio.mic_playback', 'info'),
                          ('audio.mic_processed', 'fail'),
                          ('audio.mic_remote_only', 'fail'),
                          ('audio.mic_system_aec', 'fail')])

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


class MicInput(unittest.TestCase):
    """The fake capture device's WAV (leakkit/micinput.py)."""

    def test_wav(self):
        data = micinput.wav_bytes()
        self.assertEqual(data[:4], b'RIFF')
        self.assertEqual(data[8:16], b'WAVEfmt ')
        fmt, channels, rate, byte_rate, align, bits = struct.unpack(
            '<HHIIHH', data[20:36])
        self.assertEqual((fmt, channels, rate, bits), (1, 2, 44100, 16))
        self.assertEqual((byte_rate, align), (44100 * 4, 4))
        samples = micinput.samples_cache()
        self.assertEqual(len(data), 44 + 2 * len(samples))
        lead = int(micinput.LEAD_S * micinput.RATE) * 2
        self.assertTrue(all(x == 0 for x in samples[:lead]))
        # The burst starts right after the silence and is loud.
        self.assertGreater(max(abs(x) for x in samples[lead:lead + 2000]),
                           8000)
        self.assertTrue(all(-32768 <= x <= 32767 for x in samples))
        # Integer arithmetic only: the same bytes everywhere.
        self.assertEqual(hashlib.sha256(data).hexdigest()[:16],
                         '16399ecd8c0d4d36')


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
