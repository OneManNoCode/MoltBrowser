# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Classifies everything the browser tried to send outside Tor.

Inputs (all recorded by the harness, see docs/LEAK_TESTING.md):

  capture/blocked.pcap  nflog group 1: every packet the namespace's
                        firewall logged because it was not Tor's: dropped
                        packets, name lookups (also the ones the kit's
                        resolver answers), connections to the loopback
                        canary, IGMP/MLD reports
  capture/tor.pcap      nflog group 2: the first packet of every
                        connection Tor (or its pluggable transport) opened
  capture/all.pcap      everything on every interface of the namespace
  logs/dns_canary.jsonl, logs/loopback_canary.jsonl: the kit's resolver
                        and loopback canary, from their side

The verdict never depends on the firewall having worked: a packet that
left the namespace (all.pcap, outgoing, not loopback) and isn't part of a
connection Tor opened is reported as "escaped".
"""

import ipaddress
import json
import os

from . import background
from . import dissect
from . import pcapread

LEAK = 'leak'
FORBIDDEN = 'forbidden'
BACKGROUND = 'background'
UNEXPLAINED = 'unexplained'
KERNEL = 'kernel'

MDNS_GROUPS = ('224.0.0.251', 'ff02::fb')
DNS_PORTS = (53, 853, 5353, 5355)
STUN_PORTS = (3478, 3479, 5349, 19302)
SSDP = ('239.255.255.250', 'ff02::c')
_HOUSEKEEPING_ICMPV6 = (133, 134, 135, 136)  # router/neighbour discovery
_MLD = (130, 131, 132, 143)


def _addr(text):
    try:
        return ipaddress.ip_address(text)
    except ValueError:
        return None


class Context(object):
    """What the analysis needs from the run manifest."""

    def __init__(self, manifest):
        net = manifest.get('net') or {}
        canary = manifest.get('canary') or {}
        self.canary_domain = (canary.get('domain') or 'moltleak.test').lower()
        self.loopback_port = canary.get('loopback_port')
        self.ns_addrs = set(a for a in (_addr(net.get('ns_ipv4') or ''),
                                        _addr(net.get('ns_ipv6') or ''))
                            if a is not None)
        self.gw_addrs = set(a for a in (_addr(net.get('gw_ipv4') or ''),
                                        _addr(net.get('gw_ipv6') or ''))
                            if a is not None)
        self.tests = [t for t in manifest.get('tests') or []]
        self.nonces = {}
        self.test_ips = {}
        for test in self.tests:
            if test.get('nonce'):
                self.nonces[test['nonce'].lower()] = test['id']
            for ip in (test.get('ipv4'), test.get('ipv6')):
                a = _addr(ip or '')
                if a is not None:
                    self.test_ips[a] = test['id']
        self.windows = []
        for test in self.tests:
            start = test.get('start')
            end = test.get('settle_end') or test.get('end')
            if start is not None and end is not None:
                self.windows.append((start - 0.5, end + 1.0, test['id'],
                                     test.get('target', 'moltnet')))
        for phase in manifest.get('phases') or []:
            if phase.get('start') is not None and phase.get('end') is not None:
                self.windows.append((phase['start'] - 0.5, phase['end'] + 1.0,
                                     'phase:' + phase['name'],
                                     phase.get('target', 'moltnet')))
        self.windows.sort()

    def window_at(self, ts):
        # A window proper first; only then its margins (half a second before,
        # a second after), which may overlap the next test's start.
        for start, end, test_id, target in self.windows:
            if start + 0.5 <= ts <= end - 1.0:
                return test_id, target
        for start, end, test_id, target in self.windows:
            if start <= ts <= end:
                return test_id, target
        return None, None

    def nonce_in(self, text):
        text = (text or '').lower()
        for nonce, test_id in self.nonces.items():
            if nonce in text:
                return test_id
        return None


class Event(object):
    """One kind of attempt to reach something outside Tor (aggregated)."""

    def __init__(self, ts, kind, proto, dst, dport, qname=None, qtype=None):
        self.first = ts
        self.last = ts
        self.count = 1
        self.kind = kind      # dns, mdns, tcp, udp, icmp, multicast, local
        self.proto = proto
        self.dst = dst
        self.dport = dport
        self.qname = qname
        self.qtype = qtype
        self.detail = ''
        self.category = UNEXPLAINED
        self.reason = ''
        self.test = None
        self.attribution = None   # nonce, ip, window
        self.sources = set()

    def key(self):
        return (self.kind, self.proto, self.dst, self.dport, self.qname,
                self.qtype, self.test, self.category)

    def as_dict(self):
        return {
            'first': round(self.first, 6), 'last': round(self.last, 6),
            'count': self.count, 'kind': self.kind, 'proto': self.proto,
            'dst': self.dst, 'dport': self.dport, 'qname': self.qname,
            'qtype': self.qtype, 'detail': self.detail,
            'category': self.category, 'reason': self.reason,
            'test': self.test, 'attribution': self.attribution,
            'sources': sorted(self.sources),
        }


def _proto_name(proto):
    return {6: 'tcp', 17: 'udp', 1: 'icmp', 58: 'icmpv6', 2: 'igmp'}.get(
        proto, str(proto))


def _dns_questions(packet):
    if packet.proto == pcapread.PROTO_UDP:
        msg = dissect.parse_dns(packet.payload)
        return [msg] if msg is not None else []
    if packet.proto == pcapread.PROTO_TCP and packet.payload:
        return dissect.parse_dns_tcp(packet.payload)
    return []


def _classify_name(ctx, ev, qname, is_mdns):
    """Category for a name looked up outside Tor."""
    name = (qname or '').rstrip('.')
    test = ctx.nonce_in(name)
    if test:
        ev.test, ev.attribution = test, 'nonce'
        ev.category, ev.reason = LEAK, 'a test page\'s canary name was ' \
                                       'looked up outside Tor'
        return
    if name == 'onion' or name.endswith('.onion'):
        ev.category, ev.reason = LEAK, 'an onion name was looked up ' \
                                       'outside Tor'
        return
    if name == ctx.canary_domain or name.endswith('.' + ctx.canary_domain):
        ev.category, ev.reason = LEAK, 'a canary name (unknown test) was ' \
                                       'looked up outside Tor'
        return
    if is_mdns or name.endswith('.local'):
        ev.category, ev.reason = LEAK, 'multicast DNS on the local network'
        return
    hit = background.lookup(name, background.MUST_NOT_APPEAR)
    if hit:
        ev.category, ev.reason = FORBIDDEN, 'must not appear: ' + hit[1]
        return
    hit = background.lookup(name, background.EXPECTED)
    if hit:
        ev.category, ev.reason = BACKGROUND, hit[1]
        return
    ev.category, ev.reason = UNEXPLAINED, 'a name looked up outside Tor ' \
                                          'that no list explains'


def _classify_addr(ctx, ev, packet):
    dst = packet.dst
    if dst in ctx.test_ips:
        ev.test, ev.attribution = ctx.test_ips[dst], 'ip'
        ev.category, ev.reason = LEAK, 'a packet to a test page\'s IP ' \
                                       'literal outside Tor'
        return
    if str(dst) in MDNS_GROUPS or packet.dport == 5353:
        ev.category, ev.reason = LEAK, 'multicast DNS on the local network'
        return
    if str(dst) in SSDP or packet.dport == 1900:
        ev.category, ev.reason = LEAK, 'SSDP/DIAL discovery on the local ' \
                                       'network'
        return
    if dst.is_multicast:
        ev.category, ev.reason = LEAK, 'multicast on the local network'
        return
    if dst in ctx.gw_addrs:
        ev.category, ev.reason = LEAK, 'a LAN neighbour (the namespace\'s ' \
                                       'gateway)'
        return
    if dst.is_private or dst.is_link_local or dst.is_loopback:
        ev.category, ev.reason = LEAK, 'a private or link-local address'
        return
    if packet.proto == pcapread.PROTO_UDP and (
            packet.dport in STUN_PORTS or dissect.is_stun(packet.payload)):
        ev.category, ev.reason = LEAK, 'STUN/TURN outside Tor'
        return
    if packet.proto == pcapread.PROTO_UDP and (
            packet.dport == 443 or dissect.is_quic_long_header(
                packet.payload)):
        ev.category, ev.reason = LEAK, 'QUIC / HTTP/3 outside Tor'
        return
    if str(dst) in background.DOH_IPS and packet.dport in (443, 853):
        ev.category, ev.reason = BACKGROUND, 'DNS over HTTPS/TLS to a ' \
                                             'public resolver'
        return
    sni = dissect.tls_client_hello_sni(packet.payload) if packet.payload \
        else None
    if sni:
        ev.detail = 'TLS server name ' + sni
        _classify_name(ctx, ev, sni, False)
        return
    ev.category, ev.reason = UNEXPLAINED, 'a direct connection outside Tor'


def _attribute_by_window(ctx, ev):
    if ev.test is not None:
        return
    test, target = ctx.window_at(ev.first)
    if test is None:
        return
    ev.test, ev.attribution = test, 'window'


def _forbidden_by_test(ev, forbidden):
    """A background host a test forbids during its window becomes a leak."""
    if ev.category != BACKGROUND or not ev.test or not ev.qname:
        return
    patterns = forbidden.get(ev.test) or ()
    if background.is_forbidden_in(ev.qname, patterns):
        ev.category = LEAK
        ev.reason = 'looked up outside Tor during this test: ' + ev.reason


class _Aggregator(object):
    def __init__(self):
        self.events = {}

    def add(self, ev, source):
        key = ev.key()
        old = self.events.get(key)
        if old is None:
            ev.sources.add(source)
            self.events[key] = ev
            return
        old.count += 1
        old.first = min(old.first, ev.first)
        old.last = max(old.last, ev.last)
        old.sources.add(source)

    def result(self):
        return sorted(self.events.values(), key=lambda e: (e.first, e.kind))


def _event_from_blocked(ctx, packet):
    """Events for one packet the firewall logged (nflog group 1)."""
    prefix = (packet.prefix or '').strip()
    proto = _proto_name(packet.proto)
    dst = str(packet.dst)
    out = []
    if prefix in ('igmp', 'mld') or packet.proto == pcapread.PROTO_IGMP or (
            packet.proto == pcapread.PROTO_ICMPV6 and
            packet.icmp_type in _MLD):
        groups = dissect.multicast_groups(packet.proto, packet.icmp_type,
                                          packet.payload)
        for group in groups or ['?']:
            ev = Event(packet.ts, 'multicast', proto, group, None)
            ev.detail = 'joined multicast group ' + group
            if group in MDNS_GROUPS:
                ev.category = LEAK
                ev.reason = 'a socket joined the mDNS group'
            else:
                ev.category, ev.reason = KERNEL, 'multicast membership'
            out.append(ev)
        return out
    if packet.proto in (pcapread.PROTO_ICMP, pcapread.PROTO_ICMPV6):
        ev = Event(packet.ts, 'icmp', proto, dst, None)
        ev.detail = 'ICMP type %s' % packet.icmp_type
        ev.category, ev.reason = KERNEL, 'an ICMP message (sent by the ' \
                                         'kernel, not a socket)'
        return [ev]
    if prefix == 'lo-canary':
        ev = Event(packet.ts, 'local', proto, dst, packet.dport)
        ev.category, ev.reason = LEAK, 'a page reached a service on this ' \
                                       'machine (the loopback canary)'
        return [ev]
    if packet.dport in DNS_PORTS and packet.proto in (
            pcapread.PROTO_UDP, pcapread.PROTO_TCP):
        msgs = _dns_questions(packet)
        is_mdns = packet.dport == 5353 or dst in MDNS_GROUPS
        kind = 'mdns' if is_mdns else 'dns'
        questions = [q for m in msgs if not m.is_response for q in m.questions]
        if not questions:
            if packet.proto == pcapread.PROTO_TCP and not packet.payload:
                return []  # handshake or ACK of a DNS-over-TCP query
            ev = Event(packet.ts, kind, proto, dst, packet.dport)
            if is_mdns:
                ev.category, ev.reason = LEAK, 'multicast DNS on the local ' \
                                               'network'
            elif msgs:
                ev.category, ev.reason = UNEXPLAINED, 'a DNS response sent ' \
                                                      'outside Tor'
            else:
                ev.category, ev.reason = UNEXPLAINED, 'unparsed DNS traffic'
            return [ev]
        for q in questions:
            ev = Event(packet.ts, kind, proto, dst, packet.dport, q.name,
                       dissect.qtype_name(q.qtype))
            if packet.dport == 5355:
                ev.detail = 'LLMNR'
            _classify_name(ctx, ev, q.name, is_mdns)
            out.append(ev)
        return out
    kind = 'tcp' if packet.proto == pcapread.PROTO_TCP else (
        'udp' if packet.proto == pcapread.PROTO_UDP else proto)
    ev = Event(packet.ts, kind, proto, dst, packet.dport)
    _classify_addr(ctx, ev, packet)
    return [ev]


def _is_loopback(ctx, packet):
    if packet.linktype == pcapread.LINKTYPE_LINUX_SLL2 and \
            packet.ifindex == 1:
        return True
    return (packet.dst is not None and
            (packet.dst.is_loopback or packet.dst in ctx.ns_addrs) and
            (packet.src.is_loopback or packet.src in ctx.ns_addrs))


def _counted(ctx, packet):
    """One copy of each packet: an `any` capture shows a loopback packet
    as received (and on some kernels also as sent), and a packet leaving
    through the veth as sent."""
    if _is_loopback(ctx, packet):
        return packet.pkttype in (None, pcapread.PACKET_HOST)
    return packet.pkttype in (None, pcapread.PACKET_OUTGOING)


def _socks_requests(ctx, packets):
    """SOCKS5 requests the browser sent to Tor, from the loopback capture."""
    streams = {}
    for p in packets:
        if p.proto != pcapread.PROTO_TCP or not _is_loopback(ctx, p) or \
                not _counted(ctx, p):
            continue
        key = (str(p.src), p.sport, str(p.dst), p.dport)
        if p.is_syn:
            streams[key] = {'isn': p.seq, 'ts': p.ts, 'chunks': {}}
            continue
        stream = streams.get(key)
        if stream is None or not p.payload:
            continue
        offset = (p.seq - stream['isn'] - 1) & 0xFFFFFFFF
        if offset < 2048:
            stream['chunks'].setdefault(offset, p.payload)
    out = []
    for (src, sport, dst, dport), stream in sorted(
            streams.items(), key=lambda kv: kv[1]['ts']):
        data = b''
        for offset in sorted(stream['chunks']):
            chunk = stream['chunks'][offset]
            if offset > len(data):
                break
            data += chunk[len(data) - offset:]
        req = dissect.parse_socks5_client(data[:2048])
        if req is None or not req.complete:
            continue
        out.append({'ts': round(stream['ts'], 6), 'client_port': sport,
                    'server_port': dport, 'username': req.username,
                    'host': req.host, 'port': req.port,
                    'command': req.command,
                    'test': ctx.nonce_in(req.host) or
                    ctx.window_at(stream['ts'])[0]})
    return out


def _read_jsonl(path):
    out = []
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    item = json.loads(line)
                except ValueError:
                    continue
                if isinstance(item, dict):
                    out.append(item)
    except (IOError, OSError):
        pass
    return out


def analyze(manifest, run_dir):
    """Returns the traffic section of the report."""
    ctx = Context(manifest)
    captures = manifest.get('captures') or {}
    logs = manifest.get('logs') or {}

    def path(rel):
        return os.path.join(run_dir, rel) if rel else ''

    blocked, blocked_stats = pcapread.read_file(path(captures.get('blocked')))
    tor_new, tor_stats = pcapread.read_file(path(captures.get('tor')))
    allp, all_stats = pcapread.read_file(path(captures.get('all')))

    forbidden = {}
    for test in ctx.tests:
        if test.get('forbidden_hosts'):
            forbidden[test['id']] = test['forbidden_hosts']

    agg = _Aggregator()
    for packet in blocked:
        for ev in _event_from_blocked(ctx, packet):
            _attribute_by_window(ctx, ev)
            _forbidden_by_test(ev, forbidden)
            agg.add(ev, 'firewall log')

    # The firewall already logged every query the kit's resolver answered;
    # its own log confirms them and catches any the capture missed.
    seen = {}
    for ev in agg.events.values():
        if ev.kind == 'dns' and ev.qname is not None:
            seen.setdefault((ev.qname, ev.qtype), []).append(ev)
    for item in _read_jsonl(path(logs.get('dns'))):
        qname = str(item.get('qname', ''))[:300]
        qtype = str(item.get('qtype', ''))
        ts = float(item.get('ts', 0))
        match = [e for e in seen.get((qname, qtype), [])
                 if e.first - 2 <= ts <= e.last + 2]
        if match:
            match[0].sources.add('kit resolver')
            continue
        ev = Event(ts, 'dns', item.get('proto', 'udp'),
                   item.get('server', '127.0.0.1'), 53, qname, qtype)
        _classify_name(ctx, ev, qname, False)
        _attribute_by_window(ctx, ev)
        _forbidden_by_test(ev, forbidden)
        agg.add(ev, 'kit resolver')

    local_seen = [e for e in agg.events.values() if e.kind == 'local']
    for item in _read_jsonl(path(logs.get('loopback'))):
        ts = float(item.get('ts', 0))
        match = [e for e in local_seen if e.dport == item.get('port') and
                 e.dst == str(item.get('local', '')) and
                 e.first - 2 <= ts <= e.last + 2]
        if match:
            match[0].sources.add('loopback canary')
            if not match[0].detail:
                match[0].detail = str(item.get('first_line', ''))[:200]
            continue
        ev = Event(ts, 'local', 'tcp', str(item.get('local', '')),
                   item.get('port'))
        ev.detail = str(item.get('first_line', ''))[:200]
        ev.category, ev.reason = LEAK, 'a page reached a service on this ' \
                                       'machine (the loopback canary)'
        _attribute_by_window(ctx, ev)
        agg.add(ev, 'loopback canary')

    tor_flows = {}
    for p in tor_new:
        key = p.flow_key()
        flow = tor_flows.get(key)
        if flow is None:
            tor_flows[key] = {'dst': str(p.dst), 'dport': p.dport,
                              'proto': _proto_name(p.proto),
                              'first': round(p.ts, 6), 'packets': 1}
        else:
            flow['packets'] += 1
    destinations = {}
    for flow in tor_flows.values():
        dkey = (flow['dst'], flow['dport'], flow['proto'])
        d = destinations.setdefault(dkey, {
            'dst': flow['dst'], 'dport': flow['dport'],
            'proto': flow['proto'], 'first': flow['first'],
            'connections': 0})
        d['connections'] += 1
        d['first'] = min(d['first'], flow['first'])

    escaped = {}
    loopback_packets = 0
    for p in allp:
        if not _counted(ctx, p):
            continue
        if _is_loopback(ctx, p):
            loopback_packets += 1
            continue
        if p.proto == pcapread.PROTO_ICMPV6 and (
                p.icmp_type in _HOUSEKEEPING_ICMPV6 or p.icmp_type in _MLD):
            continue
        if p.proto == pcapread.PROTO_IGMP:
            continue
        if p.flow_key() in tor_flows:
            continue
        key = p.flow_key()
        e = escaped.setdefault(key, {
            'first': round(p.ts, 6), 'proto': _proto_name(p.proto),
            'src': str(p.src), 'dst': str(p.dst), 'dport': p.dport,
            'packets': 0})
        e['packets'] += 1

    events = agg.result()
    return {
        'capture_stats': {
            'blocked': blocked_stats.as_dict(),
            'tor': tor_stats.as_dict(),
            'all': all_stats.as_dict(),
            'loopback_packets': loopback_packets,
        },
        'events': [e.as_dict() for e in events],
        'tor_destinations': sorted(destinations.values(),
                                   key=lambda d: d['first']),
        'tor_connections': len(tor_flows),
        'escaped': sorted(escaped.values(), key=lambda e: e['first']),
        'socks': _socks_requests(ctx, allp),
    }
