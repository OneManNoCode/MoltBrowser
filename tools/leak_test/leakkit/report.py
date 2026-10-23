# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Turns a recorded run (manifest.json, captures, logs) into report.json
and report.md.

Everything here is recomputed from the recorded data, so `leaktest.py
report <run dir>` can be re-run later (for example after the background
host list was updated) and gives the same answer for the same data.
"""

import datetime
import json
import os

from . import KIT_VERSION, REPORT_SCHEMA
from . import catalog
from . import traffic
from . import uniformity

PASS, FAIL, SKIP, INFO, ERROR = 'PASS', 'FAIL', 'SKIP', 'INFO', 'ERROR'
_ORDER = {FAIL: 0, ERROR: 1, PASS: 2, SKIP: 3, INFO: 4}
_MAX_EVIDENCE = 25


def check(check_id, group, title, status, evidence=(), doc='', details=None):
    ev = [str(e) for e in evidence]
    if len(ev) > _MAX_EVIDENCE:
        ev = ev[:_MAX_EVIDENCE] + ['... %d more' % (len(ev) - _MAX_EVIDENCE)]
    return {'id': check_id, 'group': group, 'title': title, 'status': status,
            'evidence': ev, 'doc': doc, 'details': details}


def _path(results, dotted):
    cur = results
    for part in dotted.split('.'):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return None, False
    return cur, True


def _is_failure(value):
    if isinstance(value, dict):
        return bool(value.get('error')) or value.get('ok') is False
    return value in (False, None) or (isinstance(value, str) and
                                      value.startswith('error'))


def eval_assertion(results, assertion):
    """(status, actual) for one catalog assertion."""
    key, op, expected, _ = assertion
    value, found = _path(results or {}, key)
    if not found:
        return FAIL, 'missing'
    if op == 'eq':
        return (PASS if value == expected else FAIL), value
    if op == 'in':
        return (PASS if value in expected else FAIL), value
    if op == 'lt':
        ok = isinstance(value, (int, float)) and not isinstance(
            value, bool) and value < expected
        return (PASS if ok else FAIL), value
    if op == 'truthy':
        return (PASS if value else FAIL), value
    if op == 'falsy':
        return (PASS if not value else FAIL), value
    if op == 'all_fail':
        if not isinstance(value, dict) or not value:
            return FAIL, value
        ok = all(_is_failure(v) for v in value.values())
        succeeded = sorted(k for k, v in value.items() if not _is_failure(v))
        return (PASS if ok else FAIL), (value if ok else
                                        {'succeeded': succeeded})
    return SKIP, value


def _fmt_event(e):
    what = e['qname'] or ('%s:%s' % (e['dst'], e['dport'])
                          if e['dport'] is not None else e['dst'])
    extra = (' (%s)' % e['detail']) if e.get('detail') else ''
    qtype = (' %s' % e['qtype']) if e.get('qtype') else ''
    return '%s %s%s%s x%d at +%.1fs: %s [%s]' % (
        e['kind'], what, qtype, extra, e['count'], e.get('rel', 0.0),
        e['reason'], ', '.join(e['sources']))


def _server_log(run_dir, manifest):
    rel = (manifest.get('logs') or {}).get('server')
    return traffic._read_jsonl(os.path.join(run_dir, rel)) if rel else []


def _network_checks(manifest, traffic_result, server_log, t0):
    checks = []
    tests = manifest.get('tests') or []
    events = traffic_result['events']
    socks = traffic_result['socks']
    for test in tests:
        spec = catalog.by_id(test['id'])
        title = spec.title if spec else test['id']
        doc = spec.doc if spec else ''
        group = 'network' if test.get('target', 'moltnet') == 'moltnet' \
            else 'regular-window'
        if test.get('skipped') or test.get('start') is None:
            checks.append(check('net.%s' % test['id'], group, title, SKIP,
                                [test.get('skipped') or
                                 'not run (the run stopped earlier)'], doc))
            continue
        mine = [e for e in events if e['test'] == test['id']]
        # A must-not-appear host only counts against the test when the test
        # caused it (its nonce or IP); by time alone it is background noise
        # (reported once, under "background").
        leaks = [e for e in mine if e['category'] == traffic.LEAK or (
            e['category'] == traffic.FORBIDDEN and
            e['attribution'] in ('nonce', 'ip'))]
        other = [e for e in mine if e['category'] == traffic.UNEXPLAINED]
        evidence = []
        if test.get('error'):
            evidence.append('harness: ' + test['error'])
        evidence += ['LEAK ' + _fmt_event(e) for e in leaks]
        evidence += ['unexplained during this test: ' + _fmt_event(e)
                     for e in other]
        nonce = test.get('nonce', '')
        sent = [s for s in socks if nonce and nonce in (s.get('host') or '')]
        for s in sent[:8]:
            evidence.append('sent to Tor inside SOCKS5: %s:%s (username %r)' %
                            (s['host'], s['port'], s['username']))
        if spec:
            for p in spec.via_tor_paths:
                want = p.format(nonce=nonce)
                hits = [r for r in server_log if r.get('path', '').split(
                    '?')[0] == want]
                if hits:
                    evidence.append('reached the kit\'s page server%s: %s' % (
                        ' (through Tor)' if manifest.get('target') ==
                        'moltnet' else '', want))
                else:
                    evidence.append('not requested (info): %s' % want)
        if leaks:
            status = FAIL
        elif test.get('error') and not test.get('results'):
            status = ERROR
        elif other:
            status = FAIL
        else:
            status = PASS
        suffix = ': nothing outside Tor' if group == 'network' else \
            ': no onion name looked up, nothing leaked'
        checks.append(check('net.%s.traffic' % test['id'], group,
                            title + suffix, status, evidence, doc))
        if spec:
            results = test.get('results')
            if spec.expect_load and results is None:
                checks.append(check(
                    'net.%s.page' % test['id'], group,
                    title + ': the page ran', ERROR if not test.get(
                        'load_error') else FAIL,
                    ['no results from the page' +
                     (': ' + test['load_error'] if test.get('load_error')
                      else '')], doc))
                continue
            for i, assertion in enumerate(spec.assertions):
                status, actual = eval_assertion(results, assertion)
                checks.append(check(
                    'net.%s.a%d' % (test['id'], i + 1), group,
                    '%s: %s' % (title, assertion[3]), status,
                    ['%s = %s' % (assertion[0],
                                  json.dumps(actual, sort_keys=True)[:300])],
                    doc))
    # Leaks and unexplained traffic outside any test.
    loose = [e for e in events if not e['test'] or e['test'].startswith(
        'phase:')]
    loose_leaks = [e for e in loose if e['category'] == traffic.LEAK]
    checks.append(check(
        'net.unattributed_leaks', 'network',
        'No leak-shaped traffic outside the tests (onion names, mDNS, LAN, '
        'STUN, QUIC, canaries)',
        FAIL if loose_leaks else PASS,
        [_fmt_event(e) + (' during ' + e['test'] if e['test'] else '')
         for e in loose_leaks], 'MOLTNET_COMPLIANCE.md §4 step 4'))
    unexplained = [e for e in loose if e['category'] == traffic.UNEXPLAINED]
    checks.append(check(
        'net.unexplained', 'network',
        'Every lookup or connection outside Tor is explained',
        FAIL if unexplained else PASS,
        [_fmt_event(e) + (' during ' + e['test'] if e['test'] else '')
         for e in unexplained] or ['(none)'],
        'triage: a new background host goes into leakkit/background.py and '
        'docs/NETWORK_TRAFFIC.md; anything else is a leak'))
    forbidden = [e for e in events if e['category'] == traffic.FORBIDDEN]
    checks.append(check(
        'background.must_not_appear', 'background',
        'No host NETWORK_TRAFFIC.md says must not appear',
        FAIL if forbidden else PASS, [_fmt_event(e) for e in forbidden],
        'NETWORK_TRAFFIC.md "Pass criteria"'))
    bg = [e for e in events if e['category'] == traffic.BACKGROUND]
    checks.append(check(
        'background.expected', 'background',
        'Background lookups outside Tor (expected, MOLTNET_COMPLIANCE.md '
        '§5.6; DNS level only)', INFO,
        [_fmt_event(e) + (' during ' + e['test'] if e['test'] else '')
         for e in bg] or ['(none)'], 'NETWORK_TRAFFIC.md'))
    return checks


def _harness_checks(manifest, tr):
    checks = []
    stats = tr['capture_stats']
    captured = stats['all']['frames'] > 0
    checks.append(check(
        'harness.capture', 'harness', 'The packet capture recorded traffic',
        PASS if captured else ERROR,
        ['all: %(frames)d frames, %(decoded)d IP, %(malformed)d malformed' %
         stats['all'],
         'firewall log: %(frames)d frames' % stats['blocked'],
         'Tor connections: %(frames)d frames' % stats['tor'],
         'loopback packets: %d' % stats['loopback_packets']]))
    checks.append(check(
        'harness.firewall', 'harness',
        'Nothing left the namespace except Tor\'s own connections',
        FAIL if tr['escaped'] else (PASS if captured else SKIP),
        ['escaped: %(proto)s %(src)s -> %(dst)s:%(dport)s (%(packets)d '
         'packets)' % e for e in tr['escaped']] or
        ['%d Tor connections to %d destinations' % (
            tr['tor_connections'], len(tr['tor_destinations']))],
        'docs/LEAK_TESTING.md "How the capture works"'))
    dests = tr['tor_destinations']
    opened = (_phase(manifest, 'open_moltnet') or {}).get('data', {}).get(
        'context')
    if manifest.get('target') == 'moltnet' and not opened:
        checks.append(check(
            'harness.tor_traffic', 'harness',
            'Tor (and only Tor) reached the network', SKIP,
            ['MoltNet did not open in this run']))
    elif manifest.get('target') == 'moltnet':
        checks.append(check(
            'harness.tor_traffic', 'harness',
            'Tor (and only Tor) reached the network', PASS if dests else
            FAIL, ['%(proto)s %(dst)s:%(dport)s, %(connections)d '
                   'connection(s)' % d for d in dests] or
            ['Tor made no connection: was the Tor network reachable?'],
            'MOLTNET_COMPLIANCE.md §4 step 4 ("only the Tor guard (or bridge) '
            'should be contacted")'))
    for err in manifest.get('errors') or []:
        checks.append(check('harness.error', 'harness', 'Harness error',
                            ERROR, [err]))
    return checks


def _phase(manifest, name):
    for phase in manifest.get('phases') or []:
        if phase.get('name') == name:
            return phase
    return None


def _phase_check(manifest, name, check_id, group, title, doc, judge):
    phase = _phase(manifest, name)
    if phase is None:
        return check(check_id, group, title, SKIP, ['not run'], doc)
    if phase.get('skipped'):
        return check(check_id, group, title, SKIP, [phase['skipped']], doc)
    data = phase.get('data') or {}
    if phase.get('error') and not data:
        return check(check_id, group, title, ERROR, [phase['error']], doc)
    status, evidence = judge(data)
    if phase.get('error'):
        evidence = list(evidence) + ['harness: ' + phase['error']]
    return check(check_id, group, title, status, evidence, doc)


def _judge_open(data):
    res = data.get('result') or {}
    ev = ['openWindow -> %s' % json.dumps(res, sort_keys=True)[:300]]
    if data.get('context'):
        ev.append('MoltNet page target in its own browser context after '
                  '%.1fs' % data.get('seconds', 0))
    ok = not res.get('error') and bool(data.get('context'))
    return (PASS if ok else FAIL), ev


def _judge_identity(data):
    ev = []
    status = PASS
    if 'regular_cookie_in_moltnet' in data:
        seen = data['regular_cookie_in_moltnet']
        ev.append('a cookie set for the onion in the regular profile is %s '
                  'in MoltNet' % ('VISIBLE' if seen else 'not visible'))
        if seen:
            status = FAIL
    if 'cookie_after_close' in data:
        kept = bool(data['cookie_after_close'])
        ev.append('after the last MoltNet window closed, its cookie is %s' %
                  ('STILL THERE' if kept else 'gone'))
        if kept:
            status = FAIL
    if data.get('same_context_after_close'):
        ev.append('the reopened window reused the old browser context')
        status = FAIL
    if data.get('canvas_a') and data.get('canvas_b'):
        same = data['canvas_a'] == data['canvas_b']
        ev.append('canvas readback on two sites: %s' % (
            'THE SAME (farbling is not per site)' if same else 'different'))
        if same:
            status = FAIL
    if not ev:
        return SKIP, ['no data']
    return status, ev


# Tor applies at most one NEWNYM every 10 s and acknowledges a later one at
# once. A second New Identity may only open its window after Tor has
# switched again; 2 s of slack for timing between the two calls.
NEWNYM_MIN_GAP_S = 8.0


def _judge_new_identity(data):
    ev = ['newIdentity -> %s' % json.dumps(data.get('result'),
                                           sort_keys=True)[:200]]
    status = PASS
    res = data.get('result') or {}
    if res.get('error'):
        status = FAIL
    if data.get('old_context') and data.get('old_context') == data.get(
            'new_context'):
        ev.append('the browser context did not change')
        status = FAIL
    if data.get('cookie_after'):
        ev.append('the cookie survived New Identity')
        status = FAIL
    else:
        ev.append('cookie gone after New Identity')
    before, after = data.get('canvas_before'), data.get('canvas_after')
    if before and after:
        if before == after:
            ev.append('the canvas readback is unchanged (farbling seed not '
                      'renewed?)')
            status = FAIL
        else:
            ev.append('canvas readback changed (new farbling seed)')
    if data.get('exit_before') or data.get('exit_after'):
        ev.append('exit IP before %s, after %s (normally differs; needs the '
                  'public internet)' % (data.get('exit_before'),
                                        data.get('exit_after')))
    if not data.get('new_context'):
        status = FAIL
        ev.append('no new MoltNet window appeared')
    if 'old_pages_left' in data:
        if data['old_pages_left']:
            ev.append('%d page(s) of the old identity were still open '
                      '(a "Leave site?" handler kept them; hold page: %s)' %
                      (data['old_pages_left'], data.get('hold_page')))
            status = FAIL
        else:
            ev.append('no page of the old identity left (one asked "Leave '
                      'site?": %s)' % data.get('hold_page'))
    if 'second_after_first_s' in data:
        second = data['second_after_first_s']
        if (data.get('second_result') or {}).get('error'):
            ev.append('second New Identity -> %s' % json.dumps(
                data.get('second_result'), sort_keys=True)[:200])
            status = FAIL
        elif second < NEWNYM_MIN_GAP_S:
            ev.append('a second New Identity right after the first finished '
                      '%.1f s later, before Tor could switch circuits again '
                      '(it applies NEWNYM at most every 10 s)' % second)
            status = FAIL
        else:
            ev.append('a second New Identity right after the first finished '
                      '%.1f s later, once Tor had switched circuits' % second)
        if not data.get('second_context'):
            ev.append('no window appeared after the second New Identity')
            status = FAIL
    return status, ev


def _judge_fail_closed(data):
    ev = ['tor killed: %s' % data.get('killed')]
    ev.append('navigation after the kill: %s' % (
        data.get('nav_error') or 'loaded'))
    if data.get('loaded'):
        return FAIL, ev + ['the page LOADED without Tor']
    if not data.get('killed'):
        return ERROR, ev
    err = data.get('nav_error') or ''
    proxyish = any(s in err for s in ('PROXY', 'SOCKS', 'TUNNEL',
                                      'CONNECTION_REFUSED', 'CONNECTION'))
    return (PASS if proxyish else FAIL), ev + (
        [] if proxyish else ['expected a proxy/SOCKS error'])


def _judge_restart(data):
    n = data.get('moltnet_pages')
    if n is None:
        return SKIP, ['no data']
    return (PASS if n == 0 else FAIL), [
        '%d MoltNet page(s) after relaunching with the same profile' % n]


def _judge_override(data):
    res = data.get('result') or {}
    ev = ['launched with %s' % data.get('flag'),
          'openWindow -> %s' % json.dumps(res, sort_keys=True)[:300],
          'MoltNet pages: %s' % data.get('moltnet_pages')]
    ok = bool(res.get('error')) and not data.get('moltnet_pages')
    return (PASS if ok else FAIL), ev


def _judge_extension(data):
    if not data.get('loaded'):
        return SKIP, ['the test extension could not be loaded: %s' %
                      data.get('load_error')]
    ev = ['extension %s, allowed in Incognito: %s (via %s)' % (
        data.get('id'), data.get('incognito'), data.get('how'))]
    ev.append('a regular page was marked by its content script: %s' %
              data.get('regular_marked'))
    ok = bool(data.get('regular_marked')) and bool(data.get('incognito'))
    if not ok:
        ev.append('without this control the MoltNet extension test proves '
                  'nothing')
    return (PASS if ok else ERROR), ev


def _judge_verify_exit(data):
    res = data.get('result') or {}
    ev = ['verifyExit -> %s' % json.dumps(res, sort_keys=True)[:200]]
    if data.get('page'):
        ev.append('check.torproject.org/api/ip in the MoltNet tab: %s' %
                  json.dumps(data['page'])[:200])
    if not res.get('ok') and not data.get('page'):
        return SKIP, ev + ['needs a Tor exit to reach check.torproject.org']
    is_tor = res.get('is_tor') or (data.get('page') or {}).get('IsTor')
    return (PASS if is_tor else FAIL), ev


def _judge_seed(data):
    reg = data.get('regular') or {}
    ev = ['regular window: %s' % json.dumps(reg, sort_keys=True)[:300]]
    ok = (reg.get('doNotTrack') == '1' and reg.get('fontSize') == '24px' and
          not reg.get('js_ran_on_blocked'))
    return (PASS if ok else ERROR), ev + (
        [] if ok else ['the seeded settings did not take effect in the '
                       'regular profile, so the MoltNet settings checks prove '
                       'nothing'])


SANDBOXED_TESTS = ('sandboxed_a', 'sandboxed_b')


def _sandboxed_circuit_check(tr):
    """Pages served with a CSP sandbox have opaque origins. Their requests
    must still be kept apart per site: the SOCKS usernames the two
    sandboxed tests used after their documents loaded must not overlap."""
    by_test = {}
    for s in tr['socks']:
        if s.get('test') in SANDBOXED_TESTS and s.get('host'):
            by_test.setdefault(s['test'], set()).add(s.get('username'))
    ev = ['%s -> SOCKS usernames %s' % (t, sorted(u or '(none)' for u in us))
          for t, us in sorted(by_test.items())]
    if len(by_test) < 2:
        status = SKIP
        ev.append('the two sandboxed-page tests didn\'t both reach Tor')
    else:
        shared = set.intersection(*by_test.values())
        status = FAIL if shared else PASS
        if shared:
            ev.append('two sandboxed pages on different sites shared a '
                      'username: %s' % sorted(u or '(none)' for u in shared))
    return check(
        'identity.sandboxed_circuits', 'identity',
        'Sandboxed pages (opaque origins) on two sites use different SOCKS '
        'usernames', status, ev, 'MOLTNET_COMPLIANCE.md §1.4, §4 step 8')


CERT_FETCH_TESTS = ('cert_fetch', 'cert_fetch_b')
# The certificate tests' pages are on this port (BASE_AIA, BASE_B_AIA); the
# certificate's AIA URLs are plain http, port 80.
CERT_FETCH_PAGE_PORT = 8443


def _cert_fetch_owner(windows, ts):
    """The certificate test whose window holds `ts`: a window proper first,
    then the margins (half a second before, a second after)."""
    for margin_before, margin_after in ((0.0, 0.0), (0.5, 1.0)):
        for test_id, (start, end) in sorted(windows.items()):
            if start - margin_before <= ts <= end + margin_after:
                return test_id
    return None


def _cert_fetch_circuit_check(manifest, tr):
    """The two certificate tests load the same certificate, which lacks its
    intermediate, on two sites, so the browser fetches the same AIA URLs for
    each. Each of those fetches must have a SOCKS username of its own: none
    shared between the two sites' fetches, none that a page used."""
    windows = {}
    for test in manifest.get('tests') or []:
        end = test.get('settle_end') or test.get('end')
        if test.get('id') in CERT_FETCH_TESTS and \
                test.get('start') is not None and end is not None:
            windows[test['id']] = (test['start'], end)
    fetches, reached, others = {}, set(), set()
    for s in tr['socks']:
        if not s.get('host'):
            continue
        owner = _cert_fetch_owner(windows, s.get('ts') or 0)
        if owner and s.get('port') == 80:
            fetches.setdefault(owner, set()).add(s.get('username'))
            continue
        others.add(s.get('username'))
        if owner and s.get('port') == CERT_FETCH_PAGE_PORT:
            reached.add(owner)
    ev = ['%s -> certificate fetches with SOCKS usernames %s' % (
        t, sorted(u or '(none)' for u in us))
          for t, us in sorted(fetches.items())]
    status = PASS
    if len(reached) < 2:
        status = SKIP
        ev.append('the two certificate tests didn\'t both reach their site '
                  'through Tor')
    elif not fetches:
        status = SKIP
        ev.append('no certificate fetch was seen (the browser didn\'t look '
                  'for the missing intermediate)')
    else:
        if len(fetches) < 2:
            status = FAIL
            ev.append('one site\'s certificate check made no fetch of its '
                      'own (answered from a cache, or merged with the other '
                      'site\'s)')
        names = set.union(*fetches.values())
        if None in names:
            status = FAIL
            ev.append('a certificate fetch carried no SOCKS username')
        if len(fetches) == 2:
            shared = set.intersection(*fetches.values()) - {None}
            if shared:
                status = FAIL
                ev.append('the two sites\' certificate fetches shared a '
                          'username: %s' % sorted(shared))
        on_pages = (names & others) - {None}
        if on_pages:
            status = FAIL
            ev.append('a certificate fetch used a username pages use: %s' %
                      sorted(on_pages))
    return check(
        'identity.cert_fetch_circuits', 'identity',
        'Certificate fetches made for two sites use SOCKS usernames of their '
        'own (a circuit per fetch)', status, ev,
        'MOLTNET_COMPLIANCE.md §2 (Certificate fetches), §4 step 8')


SESSION_PHASES = ('identity', 'new_identity')


def _session_circuit_check(manifest, tr):
    """A site visited again in a new MoltNet session (after the last
    MoltNet window closed, and after each New Identity) must not go out on
    the earlier session's circuits: the SOCKS credentials (the username is
    the site, so the password) differ. The phases record when each new
    session begins (`session_marks`); the SOCKS connections of the phase
    before the first mark belong to the old session."""
    ev = []
    compared = 0
    status = PASS
    for phase in manifest.get('phases') or []:
        if phase.get('name') not in SESSION_PHASES:
            continue
        marks = (phase.get('data') or {}).get('session_marks') or []
        start, end = phase.get('start'), phase.get('end')
        if not marks or start is None or end is None:
            continue
        bounds = [start] + sorted(marks) + [end]
        sessions = []  # One {host: {(username, password_tag)}} per session.
        for lo, hi in zip(bounds, bounds[1:]):
            creds = {}
            for s in tr['socks']:
                ts = s.get('ts') or 0
                if s.get('host') and lo <= ts < hi:
                    creds.setdefault(s['host'], set()).add(
                        (s.get('username'), s.get('password_tag')))
            sessions.append(creds)
        for i, (old, new) in enumerate(zip(sessions, sessions[1:]), 1):
            for host in sorted(set(old) & set(new)):
                compared += 1
                if any(tag is None for _, tag in old[host] | new[host]):
                    status = FAIL
                    ev.append('%s, session %d: %s connected without a SOCKS '
                              'password' % (phase['name'], i, host))
                    continue
                shared = old[host] & new[host]
                ev.append('%s, session %d -> %d: %s, %d and %d set(s) of '
                          'credentials, %s' % (
                              phase['name'], i - 1, i, host, len(old[host]),
                              len(new[host]),
                              'SHARED' if shared else 'none shared'))
                if shared:
                    status = FAIL
                    ev.append('a new MoltNet session reused the previous '
                              'session\'s SOCKS credentials for %s (so its '
                              'circuits): %s' % (host, sorted(
                                  u or '(none)' for u, _ in shared)))
    if not compared:
        status = SKIP
        ev.append('no site was reached through Tor both before and after a '
                  'new MoltNet session began')
    return check(
        'identity.session_circuits', 'identity',
        'A site visited again in a new MoltNet session (after the last '
        'window closed, after New Identity) uses new SOCKS credentials, so '
        'new circuits', status, ev,
        'MOLTNET_COMPLIANCE.md §1.2-§1.4, §4 steps 6 and 7')


def _circuit_checks(manifest, tr):
    checks = []
    socks = [s for s in tr['socks'] if s.get('host')]
    by_site = {}
    for s in socks:
        host = s['host']
        if not host.endswith('.onion'):
            continue
        by_site.setdefault(host, set()).add(s.get('username'))
    ev = ['%s -> SOCKS usernames %s' % (h, sorted(u or '(none)' for u in us))
          for h, us in sorted(by_site.items())]
    status = SKIP
    if len(by_site) >= 2:
        names = [frozenset(u) for u in by_site.values()]
        overlap = set.intersection(*[set(n) for n in names])
        none_user = any(None in n for n in names)
        status = FAIL if (overlap or none_user) else PASS
        if none_user:
            ev.append('a MoltNet connection carried no SOCKS username')
        if overlap:
            ev.append('two sites shared a username: %s' % sorted(
                u or '(none)' for u in overlap))
    else:
        ev.append('fewer than two onion sites were seen in SOCKS requests')
    checks.append(check(
        'identity.per_site_circuits', 'identity',
        'Different sites use different SOCKS usernames (per-site circuits)',
        status, ev, 'MOLTNET_COMPLIANCE.md §1.4, §4 step 8'))
    checks.append(_sandboxed_circuit_check(tr))
    checks.append(_cert_fetch_circuit_check(manifest, tr))
    checks.append(_session_circuit_check(manifest, tr))
    phase = _phase(manifest, 'circuit')
    if phase and (phase.get('data') or {}).get('hops'):
        data = phase['data']
        ev = []
        missing = []
        for hop in data['hops']:
            found = hop.get('in_consensus')
            ev.append('%s %s %s %s in consensus: %s' % (
                hop.get('role'), hop.get('nickname'), hop.get('ip'),
                hop.get('fingerprint'), found))
            if found is False:
                missing.append(hop.get('fingerprint'))
        known = any(h.get('in_consensus') is not None for h in data['hops'])
        checks.append(check(
            'identity.relays_real', 'identity',
            'The panel\'s relays are in the consensus Tor downloaded',
            FAIL if missing else (PASS if known else SKIP), ev,
            'MOLTNET_COMPLIANCE.md §4 step 10'))
    else:
        checks.append(check(
            'identity.relays_real', 'identity',
            'The panel\'s relays are in the consensus Tor downloaded', SKIP,
            [(phase or {}).get('error') or 'no circuit data'],
            'MOLTNET_COMPLIANCE.md §4 step 10'))
    return checks


def _phase_checks(manifest):
    out = [
        _phase_check(manifest, 'open_moltnet', 'moltnet.open', 'harness',
                     'A MoltNet window opened (Tor bootstrapped, routing '
                     'verified)', 'MOLTNET_COMPLIANCE.md §2 Fail-closed',
                     _judge_open),
        _phase_check(manifest, 'verify_exit', 'moltnet.on_tor', 'identity',
                     'check.torproject.org says the MoltNet window uses Tor',
                     'MOLTNET_COMPLIANCE.md §4 step 1', _judge_verify_exit),
        _phase_check(manifest, 'identity', 'identity.separate', 'identity',
                     'Separate identity: cookies don\'t cross, and go with '
                     'the last window', 'MOLTNET_COMPLIANCE.md §4 step 6',
                     _judge_identity),
        _phase_check(manifest, 'new_identity', 'identity.new_identity',
                     'identity', 'New Identity: new profile, cookies gone, '
                     'new farbling seed', 'MOLTNET_COMPLIANCE.md §4 step 7',
                     _judge_new_identity),
        _phase_check(manifest, 'fail_closed', 'identity.fail_closed',
                     'identity', 'Fail-closed: with Tor killed, pages fail '
                     'with a proxy error', 'MOLTNET_COMPLIANCE.md §4 step 5',
                     _judge_fail_closed),
        _phase_check(manifest, 'restart', 'identity.no_restore', 'identity',
                     'No MoltNet window comes back after a restart',
                     'MOLTNET_COMPLIANCE.md §4 step 5', _judge_restart),
        _phase_check(manifest, 'override', 'identity.override_refused',
                     'identity', 'A --proxy-server override blocks MoltNet',
                     'MOLTNET_COMPLIANCE.md §4 step 9', _judge_override),
        _phase_check(manifest, 'extension', 'identity.extension_loaded',
                     'harness', 'The test extension loaded and ran in a '
                     'regular window (control for the extension test)',
                     'MOLTNET_COMPLIANCE.md §4 step 16', _judge_extension),
        _phase_check(manifest, 'settings_seed', 'harness.settings_seed',
                     'harness', 'The seeded regular-profile settings took '
                     'effect (control for the settings checks)',
                     'MOLTNET_COMPLIANCE.md §4 step 15', _judge_seed),
    ]
    return out


def _proxy_checks(manifest, run_dir):
    rel = (manifest.get('logs') or {}).get('proxy')
    phase = _phase(manifest, 'override')
    if not rel or phase is None:
        return [check('regular.proxy_no_onion', 'regular-window',
                      'Regular window: no onion request reaches a configured '
                      'proxy', SKIP, ['not run'],
                      'MOLTNET_COMPLIANCE.md §4 step 13')]
    entries = traffic._read_jsonl(os.path.join(run_dir, rel))
    onion = [e for e in entries if '.onion' in str(e.get('target', '')).lower()
             or '.onion' in str(e.get('host', '')).lower()]
    ev = ['the proxy saw %d request(s)' % len(entries)]
    ev += ['ONION via proxy: %s %s' % (e.get('method'), e.get('target'))
           for e in onion]
    return [check('regular.proxy_no_onion', 'regular-window',
                  'Regular window: no onion request reaches a configured '
                  'proxy', FAIL if onion else PASS, ev,
                  'MOLTNET_COMPLIANCE.md §4 step 13')]


def _regular_nav_check(manifest):
    for test in manifest.get('tests') or []:
        if test['id'] != 'regular_onion_navigation':
            continue
        if test.get('start') is None or test.get('skipped'):
            return []
        res = test.get('results') or {}
        title = res.get('title')
        ok = title == 'Tor onion site'
        return [check('regular.onion_interstitial', 'regular-window',
                      'Regular window: an onion address shows the "Tor onion '
                      'site" page', PASS if ok else FAIL,
                      ['title: %r' % title, 'url: %s' % res.get('url')],
                      'MOLTNET_COMPLIANCE.md §1.9, §4 step 12')]
    return []


def _uniformity_checks(manifest):
    records = [r for r in manifest.get('fingerprints') or []
               if r.get('target') == 'moltnet']
    out = []
    if not records:
        return [check('uniformity.collected', 'uniformity',
                      'Fingerprint values were collected in MoltNet', SKIP,
                      ['no MoltNet fingerprint records'])], []
    per_key = {}
    os_names = set(uniformity.os_of(r.get('values') or {}) for r in records
                   if r.get('kind') == 'full') or set(['unknown'])
    os_name = sorted(os_names)[0]
    for rec in records:
        for res in uniformity.check_record(rec, os_name):
            per_key.setdefault(res['key'], []).append((rec, res))
    detail = []
    for key in sorted(per_key):
        items = per_key[key]
        statuses = set(res['status'] for _, res in items)
        if FAIL in statuses:
            status = FAIL
        elif PASS in statuses:
            status = PASS
        elif INFO in statuses:
            status = INFO
        else:
            status = SKIP
        ev = []
        for rec, res in items:
            if res['status'] == status or status == PASS and len(ev) < 1:
                ev.append('%s: actual %s, expected %s%s' % (
                    uniformity.record_id(rec),
                    json.dumps(res['actual'], sort_keys=True)[:160],
                    json.dumps(res['expected'], sort_keys=True)[:120]
                    if not isinstance(res['expected'], str)
                    else res['expected'],
                    (' (%s)' % res['note']) if res['note'] else ''))
        doc = items[0][1]['doc']
        out.append(check('uniform.' + key, 'uniformity',
                         '%s is uniform' % key, status, ev[:6], doc))
        detail.append({'key': key, 'status': status})
    display = manifest.get('display') or {}
    screen = display.get('screen') or [0, 0]
    first = [r for r in records if r.get('kind') == 'full' and
             r.get('dsf') in (1, 1.0) and not r.get('label', '').startswith(
                 'dsf-')]
    if display.get('kind') == 'xvfb' and screen[0] >= 1600 and \
            screen[1] >= 1050 and first:
        v = first[0].get('values') or {}
        size = (v.get('win.innerWidth'), v.get('win.innerHeight'))
        out.append(check(
            'uniform.new_window_size', 'uniformity',
            'A new MoltNet window opens with a 1400 x 900 page',
            PASS if size == (1400, 900) else FAIL,
            ['first MoltNet page: %s x %s on a %d x %d screen' % (
                size[0], size[1], screen[0], screen[1])],
            'FINGERPRINTING.md "New windows"'))
    for rec in records:
        if rec.get('zoom_requested') not in (None, 100) and \
                rec.get('zoom_actual') is None:
            out.append(check('uniform.zoom_driven.%s' % rec.get('label'),
                             'uniformity', 'Zoom level %s%% could be set' %
                             rec.get('zoom_requested'), SKIP,
                             [rec.get('note') or 'the browser zoom did not '
                              'change; zoom levels stay manual'],
                             'docs/LEAK_TESTING.md "Zoom"'))
    return out, sorted(os_names)


def _control_checks(manifest):
    """A regular-window fingerprint as a control: values MoltNet fixes that
    are *already* the uniform value outside MoltNet prove less."""
    regular = [r for r in manifest.get('fingerprints') or []
               if r.get('target') == 'regular' and r.get('kind') == 'full']
    if not regular:
        return []
    weak = []
    values = regular[0].get('values') or {}
    for rule in uniformity.ex.FULL_RULES:
        res = uniformity.check_rule(values, rule, uniformity.os_of(values))
        if res['status'] == PASS and not rule[0].startswith(('webgl', 'ai.')):
            weak.append(rule[0])
    return [check('uniformity.control', 'uniformity',
                  'Regular-window control: values that are already uniform '
                  'outside MoltNet on this machine (their MoltNet check '
                  'proves less here)', INFO, weak or ['(none)'])]


def summarize(checks):
    counts = {PASS: 0, FAIL: 0, SKIP: 0, INFO: 0, ERROR: 0}
    for c in checks:
        counts[c['status']] = counts.get(c['status'], 0) + 1
    if counts[FAIL]:
        verdict = FAIL
    elif counts[ERROR]:
        verdict = 'INCOMPLETE'
    else:
        verdict = PASS
    return verdict, counts


def build(run_dir):
    with open(os.path.join(run_dir, 'manifest.json'), 'r') as f:
        manifest = json.load(f)
    tr = traffic.analyze(manifest, run_dir)
    t0 = manifest.get('t0') or 0.0
    for e in tr['events']:
        e['rel'] = round(e['first'] - t0, 1) if t0 else 0.0
    server_log = _server_log(run_dir, manifest)
    checks = []
    checks += _harness_checks(manifest, tr)
    checks += _phase_checks(manifest)
    checks += _network_checks(manifest, tr, server_log, t0)
    checks += _regular_nav_check(manifest)
    checks += _proxy_checks(manifest, run_dir)
    if manifest.get('target') == 'moltnet':
        checks += _circuit_checks(manifest, tr)
    uni, os_names = _uniformity_checks(manifest)
    checks += uni
    checks += _control_checks(manifest)
    checks.sort(key=lambda c: (_ORDER.get(c['status'], 9), c['group'],
                               c['id']))
    verdict, counts = summarize(checks)
    return {
        'schema': REPORT_SCHEMA,
        'kit_version': KIT_VERSION,
        'generated': datetime.datetime.now(datetime.timezone.utc).replace(
            microsecond=0).isoformat(),
        'verdict': verdict,
        'counts': counts,
        'run': {k: manifest.get(k) for k in (
            'run_id', 'started', 'finished', 'target', 'browser', 'host',
            'options', 'onion')},
        'os': os_names,
        'checks': checks,
        'traffic': tr,
        'fingerprints': manifest.get('fingerprints') or [],
        'tests': [{k: t.get(k) for k in ('id', 'target', 'url', 'nonce',
                                         'results', 'error', 'load_error')}
                  for t in manifest.get('tests') or []],
        'manual_steps': [
            {'step': s, 'title': t, 'automation': a, 'note': n}
            for s, t, a, n in catalog.COMPLIANCE_STEPS],
    }


def _md_escape(text):
    return str(text).replace('|', '\\|').replace('\n', ' ')


def to_markdown(rep):
    run = rep['run'] or {}
    browser = run.get('browser') or {}
    lines = [
        '# MoltNet leak test report',
        '',
        '**Verdict: %s**  (%s)' % (rep['verdict'], ', '.join(
            '%d %s' % (rep['counts'][k], k) for k in
            (FAIL, ERROR, PASS, SKIP, INFO))),
        '',
        '| | |', '|---|---|',
        '| Run | %s |' % _md_escape(run.get('run_id')),
        '| Started | %s |' % _md_escape(run.get('started')),
        '| Browser | %s |' % _md_escape('%s %s' % (
            browser.get('product', ''), browser.get('version', ''))),
        '| Window under test | %s |' % _md_escape(run.get('target')),
        '| OS (as pages see it) | %s |' % _md_escape(', '.join(rep['os'])),
        '| Kit | %s (%s) |' % (rep['kit_version'], rep['schema']),
        '',
        'Statuses: FAIL = a promise is broken (or traffic needs triage); '
        'ERROR = the kit could not run the check; SKIP = not applicable or '
        'not run; INFO = recorded without a verdict.',
        '',
    ]
    groups = []
    for c in rep['checks']:
        if c['group'] not in groups:
            groups.append(c['group'])
    order = ['harness', 'network', 'regular-window', 'identity',
             'uniformity', 'background']
    groups.sort(key=lambda g: order.index(g) if g in order else 99)
    for group in groups:
        lines += ['## %s' % group, '', '| Status | Check | Reference |',
                  '|---|---|---|']
        for c in rep['checks']:
            if c['group'] == group:
                lines.append('| %s | %s | %s |' % (
                    c['status'], _md_escape(c['title']), _md_escape(c['doc'])))
        lines.append('')
        for c in rep['checks']:
            if c['group'] != group or c['status'] == PASS and not c[
                    'evidence']:
                continue
            if c['status'] in (PASS, SKIP) and group == 'uniformity':
                continue
            lines.append('- **%s** %s' % (c['status'], _md_escape(c['title'])))
            for e in c['evidence']:
                lines.append('  - %s' % _md_escape(e))
        lines.append('')
    tr = rep['traffic']
    lines += ['## Tor\'s connections', '']
    for d in tr['tor_destinations'][:40]:
        lines.append('- %(proto)s %(dst)s:%(dport)s (%(connections)d)' % d)
    if not tr['tor_destinations']:
        lines.append('- (none)')
    lines += ['', '## Still manual (MOLTNET_COMPLIANCE.md §4)', '',
              '| Step | | Kit | Note |', '|---|---|---|---|']
    for s in rep['manual_steps']:
        lines.append('| %d | %s | %s | %s |' % (
            s['step'], _md_escape(s['title']), s['automation'],
            _md_escape(s['note'])))
    lines.append('')
    return '\n'.join(lines)


def write(run_dir, out_dir=None):
    rep = build(run_dir)
    out_dir = out_dir or run_dir
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(rep, f, indent=1, sort_keys=True)
        f.write('\n')
    with open(os.path.join(out_dir, 'report.md'), 'w') as f:
        f.write(to_markdown(rep))
    return rep


def diff_reports(path_a, path_b):
    """Compares the MoltNet fingerprints of two report.json files."""
    with open(path_a, 'r') as f:
        a = json.load(f)
    with open(path_b, 'r') as f:
        b = json.load(f)
    compared, diffs = uniformity.diff_records(a.get('fingerprints') or [],
                                              b.get('fingerprints') or [])
    fails = [d for d in diffs if d['severity'] == 'fail']
    lines = ['# MoltNet uniformity diff', '',
             'A: %s (%s)' % (path_a, ', '.join(a.get('os') or [])),
             'B: %s (%s)' % (path_b, ', '.join(b.get('os') or [])), '',
             '%d matching records compared; %d differences in promised '
             'values, %d in recorded-only values.' % (
                 compared, len(fails), len(diffs) - len(fails)), '']
    if set(a.get('os') or []) != set(b.get('os') or []):
        lines += ['The runs are on different operating systems: the user '
                  'agent, fonts and form-control sizes are expected to '
                  'differ.', '']
    for d in diffs:
        lines.append('- [%s] %s %s: A=%s B=%s' % (
            d['severity'], d['record'], d['key'],
            json.dumps(d['a'], sort_keys=True)[:200],
            json.dumps(d['b'], sort_keys=True)[:200]))
    if compared == 0:
        lines.append('No records with the same label, scale factor, window '
                     'size and zoom were found in both runs.')
    verdict = FAIL if fails else (PASS if compared else SKIP)
    return verdict, '\n'.join(lines) + '\n', {
        'verdict': verdict, 'compared': compared, 'diffs': diffs}
