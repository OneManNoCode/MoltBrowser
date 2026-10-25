#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Writes testdata/sample_run: a small recorded run with every kind of
finding the analysis knows (a WebRTC leak by canary name and IP, a Safe
Browsing lookup during a MoltNet download, a reachable loopback canary, an
onion name and mDNS outside any test, background and must-not-appear
hosts, a packet that escaped the firewall, per-site SOCKS usernames, a
non-uniform fingerprint value...), and the expected report.

  python3 -I tools/leak_test/tests/make_sample_run.py            # data only
  python3 -I tools/leak_test/tests/make_sample_run.py --golden   # + expected
                                                                 #   report

The test suite checks that this script reproduces the committed files
byte for byte, and that the report matches the committed one.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from tests import pcapgen as g  # noqa: E402

OUT = os.path.join(HERE, 'testdata', 'sample_run')
T0 = 1760000000.0
NS4, GW4 = '10.231.7.2', '10.231.7.1'
CANARY = 'moltleak.test'
ONION_A = 'a' * 55 + 'd.onion'
ONION_B = 'b' * 55 + 'd.onion'


def t(sec):
    return T0 + sec


def test(test_id, nonce, start, end, i, target='moltnet', **extra):
    entry = {'id': test_id, 'nonce': nonce, 'target': target,
             'ipv4': '198.51.100.%d' % (10 + i),
             'ipv6': '2001:db8::%x' % (0x10 + i), 'start': t(start),
             'end': t(end - 2), 'settle_end': t(end),
             'url': 'http://%s/t/%s/x.html' % (ONION_A, nonce)}
    entry.update(extra)
    return entry


GOOD_FP = {
    'tz.resolved': 'UTC', 'tz.offset_jan': 0, 'tz.offset_jul': 0,
    'tz.date_string_utc': True, 'intl.locale': 'en-US',
    'intl.number': '1,234,567.891', 'nav.language': 'en-US',
    'nav.languages': ['en-US'], 'hdr.accept-language': 'en-US',
    'nav.hardwareConcurrency': 8, 'nav.deviceMemory': 8,
    'nav.cpuPerformance': 0, 'nav.maxTouchPoints': 0,
    'nav.connection': {'effectiveType': '4g', 'rtt': 200, 'downlink': 1.6,
                       'saveData': False},
    'worker.connection': {'effectiveType': '4g', 'rtt': 200,
                          'downlink': 1.6, 'saveData': False},
    'nav.connection_changes': 0,
    'mq.pointer_fine': True, 'mq.pointer_coarse': False,
    'mq.any_pointer_coarse': False, 'mq.hover_hover': True,
    'mq.any_hover_none': False, 'touch.ontouchstart': False,
    'touch.createEvent': 'NotSupportedError',
    'uad.platform': 'Linux',
    'uad.high_entropy': {'platformVersion': '', 'architecture': '',
                         'bitness': '', 'model': '', 'uaFullVersion': '',
                         'fullVersionList': [], 'wow64': False,
                         'formFactors': []},
    'hdr.client_hints': {'sec-ch-ua': '"Chromium";v="152"',
                         'sec-ch-ua-mobile': '?0',
                         'sec-ch-ua-platform': '"Linux"'},
    'webgl.context': True, 'webgl.debug_renderer_info': False,
    'webgl.extensions': ['ANGLE_instanced_arrays', 'OES_texture_float'],
    'webgl.p.MAX_TEXTURE_SIZE': 8192, 'webgl.p.MAX_VARYING_VECTORS': 15,
    'webgl.p.ALIASED_POINT_SIZE_RANGE': [1, 255],
    'webgl.p.MAX_VIEWPORT_DIMS': [8192, 8192],
    'webgl.precision.FRAGMENT_SHADER.HIGH_FLOAT': [127, 127, 23],
    'webgl2.context': False,
    'webgpu.adapter': None,
    'fonts.visible': ['DejaVu Sans', 'Liberation Sans', 'Arial', 'Ubuntu'],
    'text.metrics_whole': True,
    'form.validationMessage': 'Please fill out this field.',
    'form.numberMessage': 'Value must be greater than or equal to 10.',
    'doc.characterSet_undeclared': 'windows-1252',
    'css.default_font_size': '16px', 'nav.doNotTrack': None,
    'hdr.dnt': None, 'nav.globalPrivacyControl': True, 'hdr.sec-gpc': '1',
    'perm.window-management': 'denied', 'screenDetails.error':
    'NotAllowedError', 'perm.geolocation': 'prompt',
    'storage.persist': False, 'autoplay.mediaelement': 'allowed-muted',
    'sharedStorage.set': 'rejected', 'cookies.third_party': False,
    'js.enabled_on_blocked_site': True, 'webrtc.error': 'NotSupportedError',
    'ai.LanguageModel': 'unavailable', 'ai.Summarizer': 'unavailable',
    'ai.Writer': 'unavailable', 'ai.Rewriter': 'unavailable',
    'ai.Translator': 'unavailable',
    'ua.reduced': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
                  '(KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36',
    'nav.userAgent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
                     '(KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36',
    'storage.quota': 10737418240,
    'media.devices': {'audioinput': 1, 'videoinput': 1, 'audiooutput': 1},
    'storage.quota_over_usage': 10737418240,
    'audio.render_quantum_hardware': 128,
    'audio.sample_rate': 48000, 'audio.output_latency': 0.024,
    'audio.base_latency': {'interactive': 0.01, 'balanced': 0.01,
                           'playback': 1024 / 48000,
                           'exact_0': 128 / 48000,
                           'exact_20ms': 1024 / 48000, 'rate_44100': 0.01},
    'audio.max_channel_count': 2, 'audio.channel_count': 2,
    'audio.channel_count_3': 'IndexSizeError',
    'audio.script_processor_buffer': 2048,
    'audio.output_timestamp_ok': True, 'audio.playback_latency_max': 0,
    'audio.playback_underruns': 0, 'audio.sink_id': '',
    'audio.cadence_interval_ms': 10, 'audio.cadence_burst_frames': [384, 512],
    'media.audiooutput': 1,
    'media.device_order': ['audioinput', 'videoinput', 'audiooutput'],
    'media.device_blank': True, 'media.gum': 'pending',
    'media.gum_overconstrained': 'pending', 'media.sink_default': 'resolved',
    'media.clock_running': True, 'media.clock_start_ms': 40,
    'perf.jsHeapSizeLimit': 2330000000, 'perf.heap_rounded': True,
    'perf.heap_cached': True,
    'eme.clearkey': 'NotSupportedError', 'eme.widevine': 'NotSupportedError',
    'eme.playready': 'NotSupportedError',
    'mc.vp8_360p': {'supported': True, 'smooth': True,
                    'powerEfficient': False},
    'mc.vp9_1080p': {'supported': True, 'smooth': True,
                     'powerEfficient': False},
    'mc.av1_2160p60': {'supported': True, 'smooth': True,
                       'powerEfficient': False},
    'mc.opus': {'supported': True, 'smooth': True, 'powerEfficient': True},
    'mc.encrypted': {'supported': False, 'smooth': False,
                     'powerEfficient': False},
    'mc.webrtc_decoding': {'supported': False, 'smooth': False,
                           'powerEfficient': False},
    'mc.webrtc_encoding': {'supported': False, 'smooth': False,
                           'powerEfficient': False},
    'webcodecs.decoder_hardware': False, 'webcodecs.decoder_software': True,
    'webcodecs.encoder_hardware': False, 'webcodecs.encoder_vp8': True,
    'mq.color_gamut_srgb': True, 'mq.color_gamut_p3': False,
    'mq.color_gamut_rec2020': False, 'mq.dynamic_range_high': False,
    'mq.video_dynamic_range_high': False, 'mq.color_bits': 8,
    'mq.monochrome': False,
    'recorder.mime_type': {'default': 'video/webm;codecs=vp8',
                           'video/webm': 'video/webm;codecs=vp8',
                           'video/x-matroska': 'video/x-matroska;codecs=vp8',
                           'video/mp4': 'video/mp4;codecs=vp9'},
    'recorder.supported': ['video/webm;codecs=vp8', 'video/webm;codecs=vp9',
                           'video/webm;codecs=av01',
                           'video/mp4;codecs=av01.0.04M.08',
                           'audio/webm;codecs=opus', 'audio/webm;codecs=pcm',
                           'audio/mp4;codecs=opus'],
    'fonts.local_hidden': [],
    'fonts.local_listed': ['DejaVu Sans', 'DejaVuSans-Bold',
                           'DejaVu Sans Bold Oblique', 'DejaVuSansMono',
                           'Liberation Sans', 'LiberationSans-Bold',
                           'Liberation Serif Italic', 'Noto Sans',
                           'NotoSans-Bold'],
    'storage.enforced_quota': 60 * 1024 ** 2,
    'pixels.video_frame_format': 'I420',
    'pixels.video_frame': {'format': 'I420',
                           'planes': '35ec97d035df255c26cac4ae',
                           'rgba': 'ee1de75109443c46529f39c1'},
    'pixels.encoded_opus_48k': {'chunks': 26, 'bytes': 4283,
                                'hash': 'bbcab34ea3e06d12e5058b12'},
    'cpu.wasm_relaxed_simd': False, 'cpu.wasm_simd': True,
    'cpu.math': '5d0c1e7a92b4f3608e2d17c4',
    'pixels.p3_readback': {'rgba': '3f9a61c2d84e0b7a5c1e9f20',
                           'i420': 'a07b3e95c6d2148f0e6b7d31'},
    'audio.decoded_vorbis': {'frames': 12288,
                             'hash': '9c41e2b07d35a8f61e04c2b9'},
    'canvas.hash': '0123456789abcdef01234567',
}


def geometry(w, h):
    return {'win.innerWidth': w, 'win.innerHeight': h, 'screen.width': w,
            'screen.height': h, 'screen.availWidth': w,
            'screen.availHeight': h, 'screen.availLeft': 0,
            'screen.availTop': 0, 'win.outerWidth': w, 'win.outerHeight': h,
            'win.screenX': 0, 'win.screenY': 0, 'win.devicePixelRatio': 1,
            'mq.resolution_1dppx': True, 'mq.device_size_matches': True,
            'mq.viewport_exact': True, 'scrollbar.box': 16,
            'scrollbar.overlay': False,
            'screen.colorDepth': 24, 'screen.pixelDepth': 24,
            'screen.isExtended': False,
            'screen.orientation.type': 'landscape-primary' if w > h else
            'portrait-primary', 'screen.orientation.angle': 0,
            'mouse.screen_equals_client': True}


def manifest():
    full = dict(GOOD_FP)
    full.update(geometry(1400, 900))
    bad_geo = geometry(1401, 900)
    return {
        'schema': 'moltleak-run/1', 'kit_version': '1.0', 'run_id': 'sample',
        'target': 'moltnet', 't0': T0, 'started': '2025-10-09T08:53:20+00:00',
        'finished': '2025-10-09T08:56:00+00:00',
        'browser': {'path': '/opt/moltbrowser/moltbrowser',
                    'product': 'MoltBrowser', 'version': '0.3.0'},
        'host': {'os': 'Linux-6.8-x86_64', 'python': '3.12.3'},
        'options': {'dsfs': [1.0]},
        'onion': {'a': ONION_A, 'b': ONION_B},
        'net': {'ns_ipv4': NS4, 'gw_ipv4': GW4, 'ns_ipv6': 'fd6d:6c6b:7::2',
                'gw_ipv6': 'fd6d:6c6b:7::1', 'subnet': '10.231.7.0/30'},
        'canary': {'domain': CANARY, 'loopback_port': 47321},
        'captures': {'all': 'capture/all.pcap',
                     'blocked': 'capture/blocked.pcap',
                     'tor': 'capture/tor.pcap'},
        'logs': {'dns': 'logs/dns_canary.jsonl',
                 'loopback': 'logs/loopback_canary.jsonl',
                 'server': 'logs/server.jsonl', 'proxy': 'logs/proxy.jsonl'},
        'tests': [
            test('onion_http', 'aaaaaa1111', 10, 15, 0,
                 results={'loaded': True, 'secure_context': False}),
            # The certificate without its intermediate, on two sites.
            test('cert_fetch', 'kkkkkk1212', 15, 17.5, 21),
            test('cert_fetch_b', 'llllll3434', 17.5, 20, 22),
            test('webrtc', 'bbbbbb2222', 20, 30, 2, results={
                'pc': {'error': 'NotSupportedError', 'message':
                       'No PeerConnection handler can be created, perhaps '
                       'WebRTC is disabled?'}}),
            test('dns_prefetch', 'cccccc3333', 30, 40, 3,
                 results={'loaded': True}),
            test('download', 'dddddd4444', 40, 50, 11,
                 forbidden_hosts=['sb-ssl.google.com'],
                 results={'clicked': True}),
            test('localhost_lan', 'eeeeee5555', 50, 60, 12, results={
                'fetch': {'loopback4': {'ok': False, 'error': 'TypeError'},
                          'gateway': {'ok': False, 'error': 'TypeError'}}}),
            test('builtin_ai', 'ffffff6666', 60, 70, 16, results={
                'availability': {'LanguageModel': 'unavailable',
                                 'Summarizer': 'unavailable',
                                 'Writer': 'unavailable',
                                 'Rewriter': 'unavailable',
                                 'Translator': 'unavailable'}}),
            test('regular_onion_navigation', 'gggggg7777', 70, 75, 18,
                 target='regular', results={'title': 'Tor onion site',
                                            'url': 'http://%s/' % ONION_A}),
            test('sandboxed_a', 'iiiiii9999', 76.5, 78.5, 19,
                 results={'opaque_origin': True, 'img': 'load'}),
            test('sandboxed_b', 'jjjjjj0000', 79, 80, 20,
                 results={'opaque_origin': True, 'img': 'load'}),
            {'id': 'mdns', 'nonce': 'hhhhhh8888', 'target': 'moltnet',
             'skipped': 'not selected'},
        ],
        'phases': [
            {'name': 'open_moltnet', 'target': 'moltnet', 'start': t(1),
             'end': t(2), 'data': {'result': {'running': True},
                                   'context': 'CTX1', 'seconds': 12.5}},
            {'name': 'settings_seed', 'target': 'regular', 'start': t(0.5),
             'end': t(0.9), 'data': {'regular': {
                 'doNotTrack': '1', 'fontSize': '24px',
                 'languages': ['de-DE', 'de'], 'js_ran_on_blocked': False}}},
            {'name': 'verify_exit', 'skipped': '--offline'},
            {'name': 'circuit', 'start': t(2), 'end': t(3), 'data': {
                'hops': [{'role': 'guard', 'nickname': 'g1', 'ip':
                          '192.0.2.50', 'fingerprint': 'A' * 40,
                          'in_consensus': True},
                         {'role': 'middle', 'nickname': 'm1', 'ip':
                          '192.0.2.60', 'fingerprint': 'B' * 40,
                          'in_consensus': True},
                         {'role': 'exit', 'nickname': 'e1', 'ip':
                          '192.0.2.70', 'fingerprint': 'C' * 40,
                          'in_consensus': True}],
                'consensus_relays': 7000,
                'tor_options': {'ConnLimit': '1000',
                                'MaxClientCircuitsPending': '64'}}},
            {'name': 'identity', 'start': t(80), 'end': t(90), 'data': {
                'regular_cookie_in_moltnet': False,
                'cookie_after_close': False,
                'same_context_after_close': False,
                'canvas_a': '11', 'canvas_b': '22',
                'session_marks': [t(84)]}},
            {'name': 'new_identity', 'start': t(90), 'end': t(100), 'data': {
                'result': {'running': True}, 'old_context': 'CTX2',
                'new_context': 'CTX3', 'cookie_after': False,
                'canvas_before': '33', 'canvas_after': '44',
                'hold_page': True, 'old_pages_left': 0,
                'second_result': {'running': True},
                'second_after_first_s': 10.4, 'second_context': 'CTX4',
                'session_marks': [t(92), t(96)]}},
            {'name': 'fail_closed', 'start': t(100), 'end': t(105), 'data': {
                'killed': [4242], 'nav_error':
                'net::ERR_SOCKS_CONNECTION_FAILED', 'loaded': False}},
            {'name': 'restart', 'start': t(110), 'end': t(120),
             'data': {'moltnet_pages': 0}},
            {'name': 'override', 'target': 'regular', 'start': t(120),
             'end': t(130), 'data': {
                 'flag': '--proxy-server=http://127.0.0.1:18888',
                 'result': {'error': 'A command-line flag controls the '
                                     'proxy.'}, 'moltnet_pages': 0}},
            {'name': 'partitioning_override', 'target': 'regular',
             'start': t(130), 'end': t(140), 'data': {'runs': [
                 {'feature': f, 'flag': '--disable-features=' + f,
                  'result': {'error': 'MoltNet keeps each site\'s '
                             'connections, cache and storage apart, and a '
                             'command-line flag or field trial has turned '
                             'that off for the whole browser (%s), so '
                             'MoltNet can\'t open.' % f},
                  'moltnet_pages': 0}
                 for f in ('PartitionConnectionsByNetworkIsolationKey',
                           'SplitCacheByNetworkIsolationKey',
                           'ThirdPartyStoragePartitioning')]}},
            {'name': 'policy_live', 'target': 'moltnet', 'start': t(142),
             'end': t(178), 'data': {
                 'flags': ['--enable-features=AutofillServerCommunication,'
                           'ReduceAcceptLanguage'],
                 'autofill_start': t(143), 'autofill_end': t(153),
                 'autofill_nav_error': None,
                 'canary_host': 'plc-pppppp9999.' + CANARY,
                 'canary_url': 'http://plc-pppppp9999.%s/x' % CANARY,
                 'moltnet_pages_before': 1,
                 'policy': {'ProxyMode': 'direct'},
                 'policy_written': t(157), 'closed_after_s': 6.5,
                 'moltnet_pages_after': 0,
                 'reopen_with_policy': {
                     'error': 'A policy or command-line flag controls the '
                              'proxy, so MoltNet can\'t guarantee Tor '
                              'routing'},
                 'pages_with_policy': 0, 'policy_removed': t(166),
                 'reopen_without_policy': {'running': True},
                 'reopened': True,
                 'languages': {
                     'policy': {'ForcedLanguages': ['ja'],
                                'ReduceAcceptLanguageEnabled': False},
                     'moltnet_before': {'languages': ['en-US', 'en'],
                                        'accept_language': 'en-US'},
                     'regular_before': {'languages': ['de-DE', 'de'],
                                        'accept_language': 'de-DE'},
                     'pages_before': 1, 'written': t(170),
                     'moltnet_after': {'languages': ['en-US', 'en'],
                                       'accept_language': 'en-US'},
                     'regular_after': {
                         'languages': ['ja', 'de-DE', 'de'],
                         'accept_language': 'ja,de-DE;q=0.9,de;q=0.8'},
                     'pages_kept': 1, 'removed': t(176)}}},
            {'name': 'policy_live_incognito', 'target': 'moltnet',
             'start': t(179), 'end': t(178 + 75), 'data': {
                 'flags': ['--auto-open-devtools-for-tabs'],
                 'nav_error': None, 'devtools_pages': 1,
                 'devtools_own_window': True, 'hold_page': True,
                 'dialog': 'beforeunload', 'moltnet_pages_before': 2,
                 'off_written': t(200), 'closed_after_s': 5.5,
                 'pages_left': [],
                 'reopen_off': {
                     'error': 'Incognito is turned off '
                              '(IncognitoModeAvailability or parental '
                              'controls), and MoltNet windows follow it'},
                 'pages_off': 0, 'off_removed': t(210),
                 'reopen_after_off': {'running': True},
                 'reopened_after_off': True, 'pages_before_forced': 2,
                 'forced_written': t(225), 'pages_kept_forced': 2,
                 'open_forced': {'running': True}, 'opened_forced': True}},
            {'name': 'extension', 'skipped': '--no-extension'},
        ],
        'fingerprints': [
            {'label': 'initial', 'phase': 'main', 'dsf': 1.0, 'window': None,
             'zoom_requested': 100, 'zoom_actual': 1.0, 'kind': 'full',
             'target': 'moltnet', 'values': full},
            {'label': 'window', 'phase': 'main', 'dsf': 1.0,
             'window': [1366, 768], 'zoom_requested': 100,
             'zoom_actual': 1.0, 'kind': 'geometry', 'target': 'moltnet',
             'values': geometry(1200, 600)},
            {'label': 'window', 'phase': 'main', 'dsf': 1.0,
             'window': [1280, 800], 'zoom_requested': 100,
             'zoom_actual': 1.0, 'kind': 'geometry', 'target': 'moltnet',
             'values': bad_geo},
            {'label': 'zoom', 'phase': 'main', 'dsf': 1.0,
             'window': [1920, 1080], 'zoom_requested': 125,
             'zoom_actual': None, 'kind': 'geometry', 'target': 'moltnet',
             'note': 'Ctrl++ through DevTools did not change the zoom',
             'values': geometry(1400, 800)},
            {'label': 'initial', 'phase': 'main', 'dsf': 1.0, 'window': None,
             'zoom_requested': 100, 'kind': 'full', 'target': 'regular',
             'values': {'tz.resolved': 'Europe/Berlin',
                        'nav.hardwareConcurrency': 8,
                        'mq.pointer_fine': True}},
        ],
        'errors': [],
    }


def captures():
    blocked = []   # nflog group 1
    tor = []       # nflog group 2
    allf = []      # tcpdump -i any (SLL2)

    def dns(sec, name, prefix='dns-kit', dst='127.0.0.1', qtype=1):
        pkt = g.ip(NS4 if dst != '127.0.0.1' else '127.0.0.1', dst, 17,
                   g.udp(40000 + int(sec), 53, g.dns_query(name, qtype)))
        blocked.append((t(sec), g.nflog(pkt, prefix)))

    dns(3, 'update.googleapis.com')
    dns(4, 'accounts.google.com')
    dns(5, ONION_B, prefix='dns', dst='192.0.2.53')
    dns(6, 'weird.example.org')
    dns(21, 'stun-bbbbbb2222.' + CANARY)
    dns(21.1, 'stun-bbbbbb2222.' + CANARY, qtype=28)
    dns(45, 'sb-ssl.google.com')
    dns(65.5, 'cdn.example.net')        # unexplained, during builtin_ai
    # mDNS query and the IGMP join that comes with it
    mdns = g.ip(NS4, '224.0.0.251', 17,
                g.udp(5353, 5353, g.dns_query('printer.local', 12)))
    blocked.append((t(9), g.nflog(mdns, 'dns')))
    igmp = g.ip(NS4, '224.0.0.251', 2, b'\x16\x00\x00\x00' +
                bytes([224, 0, 0, 251]))
    blocked.append((t(8), g.nflog(igmp, 'igmp')))
    # WebRTC STUN to the test's IP literal (UDP, dropped)
    stun = g.ip(NS4, '198.51.100.12', 17, g.udp(50000, 3478, b'\x00\x01\x00'
                                                b'\x00\x21\x12\xa4\x42' +
                                                b'\0' * 12))
    blocked.append((t(22), g.nflog(stun, 'drop')))
    # loopback canary reached during localhost_lan
    lo = g.ip('127.0.0.1', '127.0.0.1', 6, g.tcp(51000, 47321, flags=0x02))
    blocked.append((t(55), g.nflog(lo, 'lo-canary')))
    # Tor's own connections
    for sec, dst, port, sport in ((1.5, '192.0.2.50', 9001, 41000),
                                  (1.6, '192.0.2.51', 443, 41001)):
        syn = g.ip(NS4, dst, 6, g.tcp(sport, port, flags=0x02))
        tor.append((t(sec), g.nflog(syn, 'tor')))
        allf.append((t(sec), g.sll2(syn, ifindex=2)))
        allf.append((t(sec + 0.1), g.sll2(g.ip(NS4, dst, 6, g.tcp(
            sport, port, b'\x16\x03\x01' + b'\0' * 50, seq=1001)),
            ifindex=2)))
    # A packet that left without being Tor's (firewall failure)
    allf.append((t(7), g.sll2(g.ip(NS4, '203.0.113.99', 6, g.tcp(
        4444, 80, flags=0x02)), ifindex=2)))
    # Incoming copy (not outgoing): ignored
    allf.append((t(7.5), g.sll2(g.ip('192.0.2.50', NS4, 6, g.tcp(
        9001, 41000, b'x')), ifindex=2, pkttype=0)))
    # Loopback SOCKS sessions (browser -> Tor's SOCKS port 9150)
    sessions = ((11, 52000, 'http://' + ONION_A, ONION_A, 80),
                (12, 52001, 'https://' + ONION_B, ONION_B, 443),
                (31, 52002, 'http://' + ONION_A,
                 'anchor-cccccc3333.' + CANARY, 443),
                # Sandboxed pages: opaque top-level sites, one per site.
                (77, 52003, 'opaque null [internally: (11AA) derived from '
                 'http://' + ONION_A + ']', ONION_A, 80),
                (79.3, 52004, 'opaque null [internally: (22BB) derived from '
                 'https://' + ONION_B + ']', ONION_B, 443),
                # Certificate tests: each page on its site (port 8443), and
                # each site's fetches of the same two AIA URLs, every fetch
                # with a one-off (anonymous opaque) key of its own.
                (15.2, 52005, 'https://' + ONION_A, ONION_A, 8443),
                (15.8, 52006, 'opaque null [internally: (33CC) anonymous]',
                 ONION_A, 80),
                (15.9, 52007, 'opaque null [internally: (44DD) anonymous]',
                 'aia-kkkkkk1212.' + CANARY, 80),
                (17.7, 52008, 'https://' + ONION_B, ONION_B, 8443),
                (18.3, 52009, 'opaque null [internally: (55EE) anonymous]',
                 ONION_A, 80),
                (18.4, 52010, 'opaque null [internally: (66FF) anonymous]',
                 'aia-kkkkkk1212.' + CANARY, 80),
                # The identity phases: the same site before and after the
                # last window closed (mark at 84 s), and around each New
                # Identity (marks at 92 s and 96 s). Same username, a new
                # password for each MoltNet session.
                (82, 52011, 'http://' + ONION_A, ONION_A, 80),
                (83, 52012, 'https://' + ONION_B, ONION_B, 443),
                (86, 52013, 'http://' + ONION_A, ONION_A, 80, b'S2'),
                (91, 52014, 'http://' + ONION_A, ONION_A, 80, b'S2'),
                (93, 52015, 'http://' + ONION_A, ONION_A, 80, b'S3'),
                (97, 52016, 'http://' + ONION_A, ONION_A, 80, b'S4'))
    for sec, sport, user, host, port, *password in sessions:
        syn = g.ip('127.0.0.1', '127.0.0.1', 6, g.tcp(sport, 9150, seq=5000,
                                                      flags=0x02))
        data = g.socks5_client(host, port, username=user,
                               password=password[0] if password else b'x')
        allf.append((t(sec), g.sll2(syn, ifindex=1)))
        allf.append((t(sec), g.sll2(syn, ifindex=1, pkttype=0)))
        pkt = g.ip('127.0.0.1', '127.0.0.1', 6, g.tcp(sport, 9150, data,
                                                      seq=5001))
        allf.append((t(sec + 0.01), g.sll2(pkt, ifindex=1)))
        allf.append((t(sec + 0.01), g.sll2(pkt, ifindex=1, pkttype=0)))
    allf.sort(key=lambda x: x[0])
    return {
        'capture/blocked.pcap': g.pcap(blocked, 239),
        'capture/tor.pcap': g.pcap(tor, 239),
        # pcapng, as dumpcap/tshark write it, whatever the file name says.
        'capture/all.pcap': g.pcapng(allf, 276, tsresol=9),
    }


def logs():
    def jl(items):
        return ''.join(json.dumps(i, sort_keys=True) + '\n' for i in items)
    return {
        'logs/dns_canary.jsonl': jl([
            {'ts': t(3.001), 'proto': 'udp', 'client': '127.0.0.1',
             'server': '127.0.0.1', 'qname': 'update.googleapis.com',
             'qtype': 'A', 'txid': 4660},
            {'ts': t(21.001), 'proto': 'udp', 'client': '127.0.0.1',
             'server': '127.0.0.1', 'qname': 'stun-bbbbbb2222.' + CANARY,
             'qtype': 'A', 'txid': 4660}]),
        'logs/loopback_canary.jsonl': jl([
            {'ts': t(55.01), 'peer': '127.0.0.1', 'local': '127.0.0.1',
             'port': 47321, 'first_line': 'GET / HTTP/1.1'}]),
        'logs/server.jsonl': jl([
            {'ts': t(11), 'path': '/t/aaaaaa1111/basic.html', 'status': 200,
             'host': ONION_A, 'method': 'GET', 'scheme': 'http'},
            {'ts': t(15.81), 'path': '/canary/aia-kkkkkk1212.crt',
             'status': 204, 'host': ONION_A, 'method': 'GET',
             'scheme': 'http'},
            {'ts': t(18.31), 'path': '/canary/aia-kkkkkk1212.crt',
             'status': 204, 'host': ONION_A, 'method': 'GET',
             'scheme': 'http'}]),
        'logs/proxy.jsonl': jl([
            {'ts': t(125), 'method': 'GET', 'target': 'http://%s/x' % ONION_A,
             'host': ONION_A, 'peer': '127.0.0.1'}]),
    }


def files():
    out = {'manifest.json': json.dumps(manifest(), indent=1,
                                       sort_keys=True) + '\n'}
    for rel, data in captures().items():
        if data is not None:
            out[rel] = data
    out.update(logs())
    return out


def write(root=OUT):
    for rel, data in files().items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        mode = 'wb' if isinstance(data, bytes) else 'w'
        with open(path, mode) as f:
            f.write(data)


def golden_report(root=OUT):
    from leakkit import report
    rep = report.build(root)
    rep.pop('generated', None)
    return (json.dumps(rep, indent=1, sort_keys=True) + '\n',
            report.to_markdown(rep))


if __name__ == '__main__':
    write()
    if '--golden' in sys.argv:
        js, md = golden_report()
        with open(os.path.join(OUT, '..', 'sample_report.json'), 'w') as f:
            f.write(js)
        with open(os.path.join(OUT, '..', 'sample_report.md'), 'w') as f:
            f.write(md)
    print('wrote', OUT)
