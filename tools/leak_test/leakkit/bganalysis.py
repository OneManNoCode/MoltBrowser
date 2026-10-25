# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Turns a recorded traffic capture (bgcapture.py) into report.json and
report.md: every name the browser looked up and every connection it
opened, compared with the allow-list generated from docs/NETWORK_TRAFFIC.md
(hostlist.py).

Inputs, all in the run directory:
  logs/fakedns.jsonl    the kit's resolver: each query and the sinkhole
                        address it answered with
  logs/sinkhole.jsonl   the sinkhole: each connection's address, port, TLS
                        server name and HTTP request line, and the
                        component IDs each component update check names
                        (checked against the doc's component table)
  capture/blocked.pcap  nflog group 1: the first packet of each flow to the
                        sinkhole ("sinkhole"), queries to the kit's resolver
                        ("dns-kit"), DNS sent anywhere else ("dns") and every
                        other packet the firewall dropped ("drop")
  capture/all.pcap      everything; a packet that left the namespace is
                        "escaped" (nothing may leave in a capture)

Pure: no root, no network; re-run with `leaktest.sh report <run dir>`.
Captures are untrusted input: the readers never raise on bad data.
"""

import datetime
import ipaddress
import json
import os

from . import KIT_VERSION
from . import dissect
from . import hostlist
from . import pcapread
from . import traffic

REPORT_SCHEMA = 'moltleak-traffic-report/1'
PASS, FAIL, SKIP, INFO, ERROR = 'PASS', 'FAIL', 'SKIP', 'INFO', 'ERROR'
_ORDER = {FAIL: 0, ERROR: 1, PASS: 2, SKIP: 3, INFO: 4}

EXPECTED = 'expected'
PAGE = 'page'
NOT_LISTED = 'not listed'
FORBIDDEN = 'forbidden'
USER_ONLY = 'user only'
WRONG_STEP = 'outside its step'
WRONG_PORT = 'unexpected port'
KERNEL = 'kernel'
FAILING = (NOT_LISTED, FORBIDDEN, USER_ONLY, WRONG_STEP, WRONG_PORT)

MDNS_GROUPS = ('224.0.0.251', 'ff02::fb')
_MAX_ROWS = 400


def _addr(text):
    try:
        return ipaddress.ip_address(str(text))
    except ValueError:
        return None


class Steps(object):
    """The capture's steps (manifest phases) by time."""

    def __init__(self, manifest):
        self.phases = sorted(
            [p for p in manifest.get('phases') or []
             if p.get('start') is not None],
            key=lambda p: p['start'])
        self.launches = sorted(
            float(x.get('ts') or 0) for x in manifest.get('launches') or [])

    def at(self, ts):
        """The step `ts` falls in: the last one that started before it (a
        late request belongs to the step that caused it, or to the gap
        right after it)."""
        found = None
        for p in self.phases:
            if p['start'] - 0.5 <= ts:
                found = p['name']
            else:
                break
        return found

    def since_launch(self, ts):
        before = [t for t in self.launches if t <= ts]
        return (ts - before[-1]) if before else None


class Event(object):
    def __init__(self, ts, kind, host, port=None, proto=None):
        self.first = ts
        self.last = ts
        self.count = 1
        self.kind = kind          # dns, tcp, udp, icmp, multicast
        self.host = host          # name, or the address when there is none
        self.port = port
        self.proto = proto
        self.dst = None
        self.qtypes = set()
        self.sni = set()
        self.requests = []        # "GET /path" (capped)
        self.step = None
        self.since_launch = None
        self.category = None
        self.reason = ''
        self.source = ''
        self.entry = None

    def key(self):
        return (self.kind, self.host, self.port, self.proto, self.step,
                self.category)

    def merge(self, other):
        self.count += other.count
        self.first = min(self.first, other.first)
        self.last = max(self.last, other.last)
        self.qtypes |= other.qtypes
        self.sni |= other.sni
        for r in other.requests:
            if r not in self.requests and len(self.requests) < 8:
                self.requests.append(r)

    def as_dict(self, t0):
        return {
            'first': round(self.first, 3), 'last': round(self.last, 3),
            'rel': round(self.first - t0, 1) if t0 else None,
            'count': self.count, 'kind': self.kind, 'host': self.host,
            'port': self.port, 'proto': self.proto, 'dst': self.dst,
            'qtypes': sorted(self.qtypes), 'sni': sorted(self.sni),
            'requests': list(self.requests), 'step': self.step,
            'since_launch': (round(self.since_launch, 1)
                             if self.since_launch is not None else None),
            'category': self.category, 'reason': self.reason,
            'likely_source': self.source,
            'doc_host': self.entry['host'] if self.entry else None,
        }


def _read_jsonl(path):
    return traffic._read_jsonl(path)


def _float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _port(value):
    try:
        port = int(value)
    except (TypeError, ValueError):
        return None
    return port if 0 < port < 65536 else None


def _item_host(item):
    """The host a sinkhole log entry was for: its Host header, else its TLS
    server name."""
    headers = item.get('headers')
    host = headers.get('host') if isinstance(headers, dict) else None
    return str(host or item.get('sni') or '').split(':')[0][:255].lower()


def classify(ev, hosts, canary_domain):
    """Sets ev.category, ev.reason and ev.entry."""
    host = (ev.host or '').lower().rstrip('.')
    a = _addr(host)
    if a is not None and (a.is_loopback or host == '0.0.0.0'):
        ev.category, ev.reason = PAGE, 'the kit\'s own pages on this machine'
        return
    if host == 'localhost' or host.endswith('.localhost'):
        ev.category, ev.reason = PAGE, 'the kit\'s own pages on this machine'
        return
    if host == canary_domain or host.endswith('.' + canary_domain):
        ev.category, ev.reason = PAGE, 'a name the kit\'s own pages use'
        return
    if host == 'invalid' or host.endswith('.invalid'):
        ev.category, ev.reason = PAGE, 'the dns_error step\'s name (does not ' \
                                       'resolve on purpose)'
        return
    entry = hosts.lookup(host)
    if entry is None and ev.dst and ev.dst != host:
        entry = hosts.lookup(ev.dst)
    ev.entry = entry
    if entry is None:
        ev.category = NOT_LISTED
        ev.reason = 'not in NETWORK_TRAFFIC.md'
        return
    if entry['expect'] == hostlist.FORBIDDEN:
        ev.category = FORBIDDEN
        ev.reason = 'NETWORK_TRAFFIC.md says it must not appear'
        return
    if entry['expect'] == hostlist.USER:
        ev.category = USER_ONLY
        ev.reason = 'needs a user action the capture never takes'
        return
    if entry['expect'] == hostlist.STEP and ev.step not in entry['steps']:
        ev.category = WRONG_STEP
        ev.reason = 'expected only during %s' % ', '.join(entry['steps'])
        return
    if ev.kind in ('tcp', 'udp') and not hostlist.port_allowed(
            entry, ev.port, ev.proto or ev.kind):
        ev.category = WRONG_PORT
        ev.reason = 'NETWORK_TRAFFIC.md lists %s' % ', '.join(entry['ports'])
        return
    ev.category = EXPECTED
    ev.reason = 'NETWORK_TRAFFIC.md §%s' % entry['section']


def _sinkhole_net(manifest):
    try:
        return ipaddress.ip_network((manifest.get('sinkhole') or {}).get(
            'net') or '198.18.0.0/15')
    except ValueError:
        return ipaddress.ip_network('198.18.0.0/15')


def collect(manifest, run_dir):
    """(events, facts): the raw events of a capture, unclassified."""
    logs = manifest.get('logs') or {}
    caps = manifest.get('captures') or {}
    net = _sinkhole_net(manifest)

    def path(rel):
        return os.path.join(run_dir, rel) if rel else ''

    events = []
    names = {}
    dns_log = _read_jsonl(path(logs.get('dns')))
    for item in dns_log:
        qname = str(item.get('qname') or '')[:255]
        if not qname:
            continue
        ev = Event(_float(item.get('ts')), 'dns', qname.lower().rstrip('.'))
        ev.qtypes.add(str(item.get('qtype') or '?')[:10])
        answer = str(item.get('answer') or '')
        ev.dst = answer[:64] if answer else None
        if _addr(answer) is not None:
            names[answer] = ev.host
        events.append(ev)

    sink_log = _read_jsonl(path(logs.get('sinkhole')))
    sink_by_flow = {}
    update_checks = []
    for item in sink_log:
        apps = item.get('update_apps')
        if isinstance(apps, list):
            ids = []
            for a in apps[:400]:
                if isinstance(a, str) and hostlist.COMPONENT_ID_RE.match(a) \
                        and a not in ids:
                    ids.append(a)
            update_checks.append({'ts': _float(item.get('ts')),
                                  'host': _item_host(item), 'apps': ids})
        key = (str(item.get('dst') or '')[:64], _port(item.get('port')),
               _port(item.get('peer_port')))
        sink_by_flow.setdefault(key, []).append(item)

    blocked, bstats = pcapread.read_file(path(caps.get('blocked')))
    allp, astats = pcapread.read_file(path(caps.get('all')))
    matched = set()
    for p in blocked:
        prefix = (p.prefix or '').strip()
        proto = {6: 'tcp', 17: 'udp'}.get(p.proto)
        if prefix == 'sinkhole' or (p.dst is not None and p.dst in net and
                                    prefix not in ('dns', 'dns-kit')):
            host = names.get(str(p.dst), str(p.dst))
            ev = Event(p.ts, proto or str(p.proto), host, p.dport, proto)
            ev.dst = str(p.dst)
            for item in sink_by_flow.get((str(p.dst), p.dport, p.sport), []):
                matched.add(id(item))
                _attach(ev, item)
            if proto == 'udp' and dissect.is_quic_long_header(p.payload):
                ev.requests.append('QUIC')
            events.append(ev)
        elif prefix == 'dns' and proto:
            msgs = [dissect.parse_dns(p.payload)] if proto == 'udp' else \
                dissect.parse_dns_tcp(p.payload)
            qs = [q.name for m in msgs if m is not None and not m.is_response
                  for q in m.questions]
            if proto == 'tcp' and not p.payload:
                continue
            ev = Event(p.ts, proto, str(p.dst), p.dport, proto)
            ev.dst = str(p.dst)
            for q in qs[:4]:
                ev.requests.append('DNS query %s' % str(q)[:200])
            events.append(ev)
        elif prefix in ('igmp', 'mld') or p.proto == pcapread.PROTO_IGMP:
            for group in dissect.multicast_groups(
                    p.proto, p.icmp_type, p.payload) or ['?']:
                ev = Event(p.ts, 'multicast', group)
                ev.dst = group
                events.append(ev)
        elif prefix == 'drop':
            if p.proto in (pcapread.PROTO_ICMP, pcapread.PROTO_ICMPV6):
                ev = Event(p.ts, 'icmp', str(p.dst))
            else:
                # A connection straight to an address (no lookup): the
                # TLS server name, when the first packet has one, says what
                # it was for (DNS over HTTPS to a public resolver, ...).
                sni = dissect.tls_client_hello_sni(p.payload) \
                    if p.payload else None
                ev = Event(p.ts, proto or str(p.proto),
                           sni[:255].lower() if sni else str(p.dst), p.dport,
                           proto)
                if sni:
                    ev.sni.add(sni[:255])
            ev.dst = str(p.dst)
            events.append(ev)
    # Sinkhole connections the firewall log missed (it logs only the first
    # packet of a flow; the sinkhole's own log is the second witness).
    for items in sink_by_flow.values():
        for item in items:
            if id(item) in matched:
                continue
            dst = str(item.get('dst') or '')[:64]
            ev = Event(_float(item.get('ts')), 'tcp', names.get(dst, dst),
                       _port(item.get('port')), 'tcp')
            ev.dst = dst
            _attach(ev, item)
            events.append(ev)

    ns = manifest.get('net') or {}
    ns_addrs = set(a for a in (_addr(ns.get('ns_ipv4') or ''),
                               _addr(ns.get('ns_ipv6') or '')) if a)
    escaped = {}
    for p in allp:
        if p.dst is None or p.pkttype not in (None, pcapread.PACKET_OUTGOING):
            continue
        if p.dst.is_loopback or p.dst in net or p.dst in ns_addrs:
            continue
        if p.linktype == pcapread.LINKTYPE_LINUX_SLL2 and p.ifindex == 1:
            continue
        if p.proto == pcapread.PROTO_ICMPV6 and p.icmp_type in (
                130, 131, 132, 133, 134, 135, 136, 143):
            continue
        if p.proto == pcapread.PROTO_IGMP:
            continue
        key = p.flow_key()
        e = escaped.setdefault(key, {'first': round(p.ts, 3),
                                     'proto': p.proto, 'src': str(p.src),
                                     'dst': str(p.dst), 'dport': p.dport,
                                     'packets': 0})
        e['packets'] += 1
    facts = {'capture_stats': {'blocked': bstats.as_dict(),
                               'all': astats.as_dict()},
             'dns_queries': len(dns_log), 'sinkhole_connections':
             len(sink_log), 'escaped': sorted(escaped.values(),
                                               key=lambda e: e['first']),
             'update_checks': sorted(update_checks, key=lambda u: u['ts'])}
    return events, facts


def _attach(ev, item):
    if item.get('sni'):
        ev.sni.add(str(item['sni'])[:255])
    if item.get('method') or item.get('path'):
        r = '%s %s' % (str(item.get('method') or '?')[:16],
                       str(item.get('path') or '')[:200])
        if r not in ev.requests:
            ev.requests.append(r)
    headers = item.get('headers')
    host = str((headers.get('host') if isinstance(headers, dict) else None)
               or '').split(':')[0][:255]
    if host and (not ev.host or _addr(ev.host) is not None):
        ev.host = host.lower()
    sni = item.get('sni')
    if sni and (not ev.host or _addr(ev.host) is not None):
        ev.host = str(sni).lower()
    if item.get('error') and not ev.requests:
        ev.requests.append('(%s)' % str(item['error'])[:80])


def analyze(manifest, run_dir, hosts):
    steps = Steps(manifest)
    canary = ((manifest.get('canary') or {}).get('domain') or
              'moltleak.test').lower()
    raw, facts = collect(manifest, run_dir)
    agg = {}
    for ev in raw:
        ev.step = steps.at(ev.first)
        ev.since_launch = steps.since_launch(ev.first)
        if ev.kind == 'multicast':
            if ev.host in MDNS_GROUPS:
                ev.category = NOT_LISTED
                ev.reason = 'a socket joined the mDNS group (local network ' \
                            'discovery)'
            else:
                ev.category, ev.reason = KERNEL, 'multicast membership ' \
                                                 '(the kernel)'
        elif ev.kind == 'icmp':
            ev.category, ev.reason = KERNEL, 'an ICMP message (the kernel)'
        else:
            classify(ev, hosts, canary)
        ev.source = hostlist.likely_source(
            ev.host, ev.requests[0] if ev.requests else None, ev.step,
            ev.since_launch, ev.entry)
        old = agg.get(ev.key())
        if old is None:
            agg[ev.key()] = ev
        else:
            old.merge(ev)
    events = sorted(agg.values(), key=lambda e: (e.first, e.kind, e.host))
    return events, facts, steps


def _check(check_id, title, status, evidence=(), doc=''):
    ev = [str(e) for e in evidence]
    if len(ev) > 60:
        ev = ev[:60] + ['... %d more' % (len(ev) - 60)]
    return {'id': check_id, 'group': 'traffic', 'title': title,
            'status': status, 'evidence': ev, 'doc': doc}


def fmt_event(e):
    where = e['host']
    if e['port'] is not None:
        where = '%s %s:%s' % (e['kind'], where, e['port'])
    else:
        where = '%s %s' % (e['kind'], where)
    extra = []
    if e['qtypes']:
        extra.append('/'.join(e['qtypes']))
    if e['sni'] and e['sni'] != [e['host']]:
        extra.append('TLS server name ' + ', '.join(e['sni']))
    if e['dst'] and e['dst'] != e['host'] and e['kind'] != 'dns':
        extra.append('address ' + e['dst'])
    if e['requests']:
        extra.append('; '.join(e['requests'][:3]))
    rel = '+%.1fs' % e['rel'] if e['rel'] is not None else '?'
    return '[%s, %s] %s%s x%d: %s. Likely source: %s' % (
        rel, e['step'] or 'before the first step', where,
        (' (%s)' % ', '.join(extra)) if extra else '', e['count'],
        e['reason'], e['likely_source'])


_TITLES = {
    FORBIDDEN: 'No host NETWORK_TRAFFIC.md says must not appear',
    NOT_LISTED: 'Every host contacted is in NETWORK_TRAFFIC.md',
    USER_ONLY: 'Nothing that needs a user action the capture never takes',
    WRONG_STEP: 'Step-bound hosts appear only in their steps',
    WRONG_PORT: 'Listed hosts only on their listed ports',
}


def checks_for(manifest, events, facts, steps, hosts):
    checks = []
    stats = facts['capture_stats']
    ok = (stats['blocked'].get('decoded') or 0) > 0 and \
        facts['dns_queries'] > 0
    checks.append(_check(
        'traffic.harness.capture', 'The capture recorded traffic',
        PASS if ok else ERROR,
        ['firewall log: %s packets (%s malformed)' % (
            stats['blocked'].get('decoded', 0),
            stats['blocked'].get('malformed', 0)),
         'full capture: %s packets' % stats['all'].get('decoded', 0),
         'resolver queries: %d' % facts['dns_queries'],
         'sinkhole connections: %d' % facts['sinkhole_connections']],
        'LEAK_TESTING.md "Regular-window traffic capture"'))
    checks.append(_check(
        'traffic.harness.escaped', 'Nothing left the namespace',
        FAIL if facts['escaped'] else PASS,
        ['%(proto)s %(src)s -> %(dst)s:%(dport)s x%(packets)d at '
         '%(first)s' % e for e in facts['escaped']] or ['(none)']))
    errors = [p for p in manifest.get('phases') or [] if p.get('error')]
    ran = [p for p in manifest.get('phases') or [] if p.get('start')]
    names = [p['name'] for p in ran]
    missing = [s for s in hostlist.STEP_NAMES if s not in names and not any(
        p.get('name') == s and p.get('skipped')
        for p in manifest.get('phases') or [])]
    ev = ['%s: %.0f s%s' % (p['name'], (p.get('end') or p['start']) -
                            p['start'], (' (error: %s)' % p['error'])
                            if p.get('error') else '') for p in ran]
    ev += ['%s: skipped (%s)' % (p['name'], p['skipped'])
           for p in manifest.get('phases') or [] if p.get('skipped')]
    ev += ['%s: not run' % s for s in missing]
    ev += ['harness: %s' % e for e in manifest.get('errors') or []]
    checks.append(_check(
        'traffic.harness.steps', 'Every step ran',
        ERROR if errors or missing or manifest.get('errors') else PASS, ev))
    update = [e for e in events if e['step'] == 'update_check' and
              e['host'] in ('api.github.com', 'github.com')]
    checks.append(_check(
        'traffic.harness.update_check',
        'The update check reached the sinkhole (control)',
        PASS if update else (SKIP if 'update_check' not in names else ERROR),
        [fmt_event(e) for e in update] or [
            'MoltBrowser\'s update check (api.github.com) was not seen '
            'during the update_check step: the sinkhole or the browser\'s '
            'name resolution did not work, or the build is not MoltBrowser'],
        'NETWORK_TRAFFIC.md §5'))
    checks.append(components_check(manifest, facts, steps, hosts))
    for cat in (FORBIDDEN, NOT_LISTED, USER_ONLY, WRONG_STEP, WRONG_PORT):
        bad = [e for e in events if e['category'] == cat]
        checks.append(_check(
            'traffic.' + cat.replace(' ', '_'), _TITLES[cat],
            FAIL if bad else PASS, [fmt_event(e) for e in bad],
            'triage: a new host that must stay goes into NETWORK_TRAFFIC.md '
            '(the machine-readable table, then `leaktest.sh hosts --write`) '
            'with the reason; anything else is a leak to turn off'))
    good = [e for e in events if e['category'] == EXPECTED]
    checks.append(_check(
        'traffic.expected', 'Expected background traffic (as listed)', INFO,
        [fmt_event(e) for e in good] or ['(none)'], 'NETWORK_TRAFFIC.md'))
    seen = set(e['doc_host'] for e in events if e['doc_host'])
    unseen = [x for x in hosts.entries if x['expect'] in (
        hostlist.ALLOWED, hostlist.STEP) and x['host'] not in seen]
    checks.append(_check(
        'traffic.not_seen', 'Listed as expected, not seen in this capture',
        INFO, ['%s (%s; §%s)' % (x['host'], x['why'][:120], x['section'])
               for x in unseen] or ['(none)'],
        'not a failure: the capture is short and nothing it contacts '
        'answers'))
    page = [e for e in events if e['category'] in (PAGE, KERNEL)]
    checks.append(_check(
        'traffic.kit', 'The kit\'s own pages and the kernel (not the '
        'browser\'s background traffic)', INFO,
        [fmt_event(e) for e in page] or ['(none)']))
    return checks


def components_check(manifest, facts, steps, hosts):
    """No component NETWORK_TRAFFIC.md's component table lists may be named
    by the component updater's check (the IDs the sinkhole read from each
    update check's body)."""
    t0 = manifest.get('t0') or 0.0
    seen = facts.get('update_checks') or []
    with_ids = [u for u in seen if u['apps']]
    doc = 'NETWORK_TRAFFIC.md §6, the component table'
    if not with_ids:
        return _check(
            'traffic.components', 'No component NETWORK_TRAFFIC.md says is '
            'off is in an update check', SKIP,
            ['no component update check with readable component IDs reached '
             'the sinkhole (%d update check(s) seen); the component updater '
             'checks about a minute after start' % len(seen)], doc)
    bad, ok = [], []
    for u in with_ids:
        where = '[+%.1fs, %s] %s' % (u['ts'] - t0, steps.at(u['ts']) or
                                     'before the first step',
                                     u['host'] or 'update check')
        listed = [a for a in u['apps'] if a in hosts.components]
        for a in listed:
            c = hosts.components[a]
            bad.append('%s: %s (%s): %s (NETWORK_TRAFFIC.md §%s)' % (
                where, c['name'], a, c['why'], c['section']))
        ok.append('%s: %d component(s): %s' % (where, len(u['apps']),
                                               ', '.join(u['apps'])))
    return _check(
        'traffic.components', 'No component NETWORK_TRAFFIC.md says is off '
        'is in an update check', FAIL if bad else PASS, bad + ok,
        doc + '; a component that must stay comes off the table, with the '
        'reason; anything else is registered by mistake')


def summarize(checks):
    counts = {PASS: 0, FAIL: 0, SKIP: 0, INFO: 0, ERROR: 0}
    for c in checks:
        counts[c['status']] = counts.get(c['status'], 0) + 1
    if counts[FAIL]:
        return FAIL, counts
    if counts[ERROR]:
        return 'INCOMPLETE', counts
    return PASS, counts


def _hosts_for(manifest, run_dir):
    """The allow-list the run was made against, if it is still there;
    else the repository's current one."""
    path = (manifest.get('hosts') or {}).get('path')
    for p in (path, hostlist.JSON_PATH):
        if p and os.path.isfile(p):
            return hostlist.HostList.load(p), p
    raise hostlist.HostListError('no allow-list found (%s)' % path)


def build(run_dir, hosts=None):
    with open(os.path.join(run_dir, 'manifest.json'), 'r') as f:
        manifest = json.load(f)
    hosts_path = None
    if hosts is None:
        hosts, hosts_path = _hosts_for(manifest, run_dir)
    events, facts, steps = analyze(manifest, run_dir, hosts)
    t0 = manifest.get('t0') or 0.0
    rows = [e.as_dict(t0) for e in events]
    checks = checks_for(manifest, rows, facts, steps, hosts)
    checks.sort(key=lambda c: (_ORDER.get(c['status'], 9), c['id']))
    verdict, counts = summarize(checks)
    step_rows = []
    for p in manifest.get('phases') or []:
        if not p.get('start'):
            step_rows.append({'name': p.get('name'),
                              'skipped': p.get('skipped')})
            continue
        step_rows.append({
            'name': p['name'], 'rel_start': round(p['start'] - t0, 1),
            'seconds': round((p.get('end') or p['start']) - p['start'], 1),
            'error': p.get('error'), 'data': p.get('data') or {}})
    return {
        'schema': REPORT_SCHEMA, 'kit_version': KIT_VERSION,
        'generated': datetime.datetime.now(datetime.timezone.utc).replace(
            microsecond=0).isoformat(),
        'verdict': verdict, 'counts': counts,
        'run': {k: manifest.get(k) for k in (
            'run_id', 'started', 'finished', 'target', 'browser', 'host',
            'options')},
        'allow_list': {'path': hosts_path or (manifest.get('hosts') or {}).get(
            'path'), 'entries': len(hosts.entries)},
        'checks': checks, 'events': rows, 'steps': step_rows,
        'escaped': facts['escaped'],
        'capture_stats': facts['capture_stats'],
    }


def _md(text):
    return str(text).replace('|', '\\|').replace('\n', ' ')


def to_markdown(rep):
    run = rep['run'] or {}
    browser = run.get('browser') or {}
    lines = [
        '# Regular-window traffic capture',
        '',
        '**Verdict: %s**  (%s)' % (rep['verdict'], ', '.join(
            '%d %s' % (rep['counts'][k], k) for k in
            (FAIL, ERROR, PASS, SKIP, INFO))),
        '',
        '| | |', '|---|---|',
        '| Run | %s |' % _md(run.get('run_id')),
        '| Started | %s |' % _md(run.get('started')),
        '| Browser | %s |' % _md('%s %s' % (browser.get('product', ''),
                                           browser.get('version', ''))),
        '| Allow-list | %s (%s entries) |' % (
            _md(rep['allow_list'].get('path')), rep['allow_list']['entries']),
        '| Kit | %s (%s) |' % (rep['kit_version'], rep['schema']),
        '',
        'Send back `report.json` (and this file) with the build\'s version. '
        'Every name the browser looked up and every connection it opened '
        'went to the kit\'s resolver and sinkhole, so nothing reached the '
        'real hosts. FAIL = a host the doc doesn\'t allow at that moment '
        '(the evidence gives the time, the step and the likely source); '
        'ERROR = the capture itself didn\'t work.',
        '',
        '## Checks', '', '| Status | Check |', '|---|---|',
    ]
    for c in rep['checks']:
        lines.append('| %s | %s |' % (c['status'], _md(c['title'])))
    lines.append('')
    for c in rep['checks']:
        if c['status'] == PASS and c['id'].startswith('traffic.harness'):
            continue
        if c['status'] == PASS and not [e for e in c['evidence']
                                        if e != '(none)']:
            continue
        lines.append('### %s %s' % (c['status'], _md(c['title'])))
        lines.append('')
        if c.get('doc'):
            lines.append('_%s_' % _md(c['doc']))
            lines.append('')
        for e in c['evidence']:
            lines.append('- %s' % _md(e))
        lines.append('')
    lines += ['## Steps', '', '| Step | Starts at | Seconds | Note |',
              '|---|---|---|---|']
    for s in rep['steps']:
        if s.get('skipped'):
            lines.append('| %s | | | skipped: %s |' % (_md(s['name']),
                                                     _md(s['skipped'])))
            continue
        note = s.get('error') or (s.get('data') or {}).get('nav_error') or ''
        lines.append('| %s | +%.1f s | %.0f | %s |' % (
            _md(s['name']), s['rel_start'], s['seconds'], _md(note)))
    lines += ['', '## Timeline', '',
              '| Time | Step | What | Host | Port | Detail | Verdict |',
              '|---|---|---|---|---|---|---|']
    for e in rep['events'][:_MAX_ROWS]:
        detail = []
        if e['sni'] and e['sni'] != [e['host']]:
            detail.append('SNI ' + ', '.join(e['sni']))
        detail += e['requests'][:2]
        if e['qtypes']:
            detail.append('/'.join(e['qtypes']))
        lines.append('| +%s s | %s | %s x%d | %s | %s | %s | %s |' % (
            e['rel'], _md(e['step'] or ''), _md(e['kind']), e['count'],
            _md(e['host']), e['port'] if e['port'] is not None else '',
            _md('; '.join(detail)[:160]), _md(e['category'])))
    if len(rep['events']) > _MAX_ROWS:
        lines.append('')
        lines.append('(%d more in report.json)' % (len(rep['events']) -
                                                   _MAX_ROWS))
    lines.append('')
    return '\n'.join(lines)


def write(run_dir, out_dir=None, hosts=None):
    rep = build(run_dir, hosts)
    out_dir = out_dir or run_dir
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(rep, f, indent=1, sort_keys=True)
        f.write('\n')
    with open(os.path.join(out_dir, 'report.md'), 'w') as f:
        f.write(to_markdown(rep))
    return rep


def is_traffic_run(run_dir):
    try:
        with open(os.path.join(run_dir, 'manifest.json'), 'r') as f:
            return json.load(f).get('schema') == 'moltleak-traffic/1'
    except (IOError, OSError, ValueError):
        return False
