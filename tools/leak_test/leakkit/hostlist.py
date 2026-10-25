# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The hosts docs/NETWORK_TRAFFIC.md says a regular window may contact.

The doc carries a machine-readable table between two markers:

    <!-- hosts:begin -->
    | Host | Ports | Expect | Why | § |
    |---|---|---|---|---|
    | `update.googleapis.com` | 443, 80 | allowed | Component updater ... | 1 |
    <!-- hosts:end -->

  Host    one or more backticked patterns, comma-separated: an exact host,
          "*.suffix" for every name under suffix, or an IP literal
  Ports   comma-separated ports, TCP unless written "53/udp"; "*" for any
  Expect  allowed      may appear at any time
          step: a, b   only during those steps of the traffic capture
                       (CAPTURE_STEPS), for example a download check
          user         only after something a user does that the capture
                       never does (a model download, a cloud AI key, ...)
          forbidden    must never appear: the doc says it is off
  Why     what sends it: the report quotes it as the likely source
  §       the doc section with the details

A second table, between `<!-- components:begin -->` and
`<!-- components:end -->`, lists the components the component updater's
check must never name (| Component | ID | Why | § |, the ID being the
32-letter component ID). The sinkhole reads the IDs out of each update
check, and the report fails on any of these.

`build()` turns the doc into docs/network_traffic_hosts.json, which the
traffic capture reads (`leaktest.sh hosts --write` regenerates it,
`--check` and the kit's tests fail when the two differ). Everything here is
pure: no root, no network.
"""

import ipaddress
import json
import os
import re

SCHEMA = 'molt-network-traffic-hosts/1'
BEGIN, END = '<!-- hosts:begin -->', '<!-- hosts:end -->'
COMPONENTS_BEGIN = '<!-- components:begin -->'
COMPONENTS_END = '<!-- components:end -->'
COMPONENT_ID_RE = re.compile(r'^[a-p]{32}$')

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DOC = os.path.join(REPO, 'docs', 'NETWORK_TRAFFIC.md')
JSON_PATH = os.path.join(REPO, 'docs', 'network_traffic_hosts.json')
DOC_REL = 'docs/NETWORK_TRAFFIC.md'

ALLOWED, STEP, USER, FORBIDDEN = 'allowed', 'step', 'user', 'forbidden'

# The traffic capture's steps (leakkit/bgcapture.py), in order. "step:"
# rows may name the browse steps and the update check.
CAPTURE_STEPS = (
    ('first_run', 'first launch with a fresh profile'),
    ('idle', 'left idle on the start page'),
    ('home', 'opens the new tab page'),
    ('form', 'fills and submits a sign-in form and an address form'),
    ('translate', 'opens a French page (Translate offers itself)'),
    ('spellcheck', 'types misspelt text into a text area'),
    ('download', 'downloads a Windows program (.exe)'),
    ('dns_error', 'opens a name that does not resolve'),
    ('cert_error', 'opens a page with an expired certificate'),
    ('shutdown', 'closes the browser'),
    ('restart', 'starts again with the same profile, idle'),
    ('update_check', 'opens molt://ai-update, which checks for an update'),
    ('final', 'waits for late requests, then closes the browser'),
)
STEP_NAMES = tuple(name for name, _ in CAPTURE_STEPS)
BROWSE_STEPS = ('home', 'form', 'translate', 'spellcheck', 'download',
                'dns_error', 'cert_error')

_HOST_RE = re.compile(r'^(\*\.)?[a-z0-9]([a-z0-9-]*[a-z0-9])?'
                      r'(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$')
_PORT_RE = re.compile(r'^(\d{1,5})(/(tcp|udp))?$')


class HostListError(ValueError):
    pass


def _split_row(line):
    line = line.strip()
    if not (line.startswith('|') and line.endswith('|')):
        return None
    return [c.strip() for c in line[1:-1].split('|')]


def _is_ip(text):
    try:
        ipaddress.ip_address(text)
        return True
    except ValueError:
        return False


def parse_hosts(cell, lineno):
    pats = re.findall(r'`([^`]*)`', cell)
    rest = re.sub(r'`[^`]*`', '', cell).replace(',', '').strip()
    if not pats or rest:
        raise HostListError('line %d: the host cell must be backticked '
                            'patterns separated by commas: %r' % (lineno,
                                                                 cell))
    out = []
    for p in pats:
        p = p.strip().lower().rstrip('.')
        if not (_HOST_RE.match(p) or _is_ip(p)):
            raise HostListError('line %d: not a host, *.suffix or IP '
                                'literal: %r' % (lineno, p))
        out.append(p)
    return out


def parse_ports(cell, lineno):
    cell = cell.strip()
    if cell == '*':
        return ['*']
    out = []
    for item in cell.split(','):
        item = item.strip().lower()
        m = _PORT_RE.match(item)
        if not m or not 0 < int(m.group(1)) < 65536:
            raise HostListError('line %d: bad port %r (write 443, 53/udp '
                                'or *)' % (lineno, item))
        spec = '%d/%s' % (int(m.group(1)), m.group(3) or 'tcp')
        if spec not in out:
            out.append(spec)
    if not out:
        raise HostListError('line %d: no ports' % lineno)
    return out


def parse_expect(cell, lineno):
    cell = cell.strip().lower()
    if cell in (ALLOWED, USER, FORBIDDEN):
        return cell, []
    if cell.startswith('step:'):
        steps = [s.strip() for s in cell[5:].split(',') if s.strip()]
        bad = [s for s in steps if s not in STEP_NAMES]
        if not steps or bad:
            raise HostListError('line %d: unknown capture step(s) %s (known: '
                                '%s)' % (lineno, bad or '(none)',
                                         ', '.join(STEP_NAMES)))
        return STEP, steps
    raise HostListError('line %d: Expect must be allowed, user, forbidden '
                        'or "step: a, b": %r' % (lineno, cell))


def _plain(text):
    return re.sub(r'\s+', ' ', text.replace('`', '')).strip()


def parse_doc(text):
    """The table between the markers, as a list of host entries."""
    lines = text.split('\n')
    try:
        start = lines.index(BEGIN)
        end = lines.index(END, start)
    except ValueError:
        raise HostListError('%s or %s not found in the doc' % (BEGIN, END))
    rows = []
    header = None
    for i in range(start + 1, end):
        lineno = i + 1
        line = lines[i]
        if not line.strip():
            continue
        cells = _split_row(line)
        if cells is None:
            raise HostListError('line %d: not a table row: %r' % (lineno,
                                                                  line))
        if header is None:
            header = [c.lower() for c in cells]
            if header != ['host', 'ports', 'expect', 'why', '§']:
                raise HostListError('line %d: the header must be | Host | '
                                    'Ports | Expect | Why | § |' % lineno)
            continue
        if all(set(c) <= set('-: ') for c in cells):
            continue
        if len(cells) != 5:
            raise HostListError('line %d: %d cells, want 5' % (lineno,
                                                                len(cells)))
        hosts = parse_hosts(cells[0], lineno)
        ports = parse_ports(cells[1], lineno)
        expect, steps = parse_expect(cells[2], lineno)
        why = _plain(cells[3])
        if not why:
            raise HostListError('line %d: Why is empty' % lineno)
        section = _plain(cells[4])
        for host in hosts:
            rows.append({'host': host, 'ports': ports, 'expect': expect,
                         'steps': steps, 'why': why, 'section': section,
                         'line': lineno})
    seen = {}
    for row in rows:
        if row['host'] in seen:
            raise HostListError('line %d: %s is already on line %d' % (
                row['line'], row['host'], seen[row['host']]))
        seen[row['host']] = row['line']
    if not rows:
        raise HostListError('the host table is empty')
    return rows


def parse_components(text):
    """The component table between its markers, as a list of entries; []
    when the doc has no such table."""
    lines = text.split('\n')
    if COMPONENTS_BEGIN not in lines:
        return []
    start = lines.index(COMPONENTS_BEGIN)
    try:
        end = lines.index(COMPONENTS_END, start)
    except ValueError:
        raise HostListError('%s without %s' % (COMPONENTS_BEGIN,
                                               COMPONENTS_END))
    rows = []
    header = None
    seen = {}
    for i in range(start + 1, end):
        lineno = i + 1
        line = lines[i]
        if not line.strip():
            continue
        cells = _split_row(line)
        if cells is None:
            raise HostListError('line %d: not a table row: %r' % (lineno,
                                                                  line))
        if header is None:
            header = [c.lower() for c in cells]
            if header != ['component', 'id', 'why', '§']:
                raise HostListError('line %d: the header must be | '
                                    'Component | ID | Why | § |' % lineno)
            continue
        if all(set(c) <= set('-: ') for c in cells):
            continue
        if len(cells) != 4:
            raise HostListError('line %d: %d cells, want 4' % (lineno,
                                                                len(cells)))
        name = _plain(cells[0])
        ids = re.findall(r'`([^`]*)`', cells[1])
        if len(ids) != 1 or not COMPONENT_ID_RE.match(ids[0]):
            raise HostListError('line %d: the ID must be one backticked '
                                'component ID (32 letters a-p): %r' % (
                                    lineno, cells[1]))
        why = _plain(cells[2])
        if not name or not why:
            raise HostListError('line %d: Component and Why must not be '
                                'empty' % lineno)
        cid = ids[0]
        if cid in seen:
            raise HostListError('line %d: %s is already on line %d' % (
                lineno, cid, seen[cid]))
        seen[cid] = lineno
        rows.append({'id': cid, 'name': name, 'why': why,
                     'section': _plain(cells[3]), 'line': lineno})
    return rows


def build(doc_text):
    """The JSON document generated from the doc's text."""
    hosts = []
    for row in parse_doc(doc_text):
        entry = dict(row)
        del entry['line']
        hosts.append(entry)
    components = []
    for row in parse_components(doc_text):
        entry = dict(row)
        del entry['line']
        components.append(entry)
    return {'schema': SCHEMA, 'generated_from': DOC_REL,
            'regenerate': 'tools/leak_test/leaktest.sh hosts --write',
            'hosts': hosts, 'components': components}


def dumps(doc):
    return json.dumps(doc, indent=1, sort_keys=True, ensure_ascii=False) + \
        '\n'


def check_files(doc_path=DOC, json_path=JSON_PATH):
    """None if the JSON is what the doc generates, else why not."""
    with open(doc_path, 'r', encoding='utf-8') as f:
        want = dumps(build(f.read()))
    try:
        with open(json_path, 'r', encoding='utf-8') as f:
            have = f.read()
    except (IOError, OSError) as e:
        return 'cannot read %s: %s' % (json_path, e)
    if have != want:
        return ('%s is out of date: run tools/leak_test/leaktest.sh hosts '
                '--write' % os.path.relpath(json_path, REPO))
    return None


def write_json(doc_path=DOC, json_path=JSON_PATH):
    with open(doc_path, 'r', encoding='utf-8') as f:
        text = dumps(build(f.read()))
    with open(json_path, 'w', encoding='utf-8') as f:
        f.write(text)
    return json_path


class HostList(object):
    """Lookups in the generated table."""

    def __init__(self, doc):
        if not isinstance(doc, dict) or doc.get('schema') != SCHEMA:
            raise HostListError('not a %s document' % SCHEMA)
        self.entries = list(doc.get('hosts') or [])
        self.exact = {}
        self.wild = []
        for e in self.entries:
            if e['host'].startswith('*.'):
                self.wild.append(e)
            else:
                self.exact[e['host']] = e
        # The most specific suffix first.
        self.wild.sort(key=lambda e: -len(e['host']))
        # Components the update check must not name, by ID.
        self.components = {}
        for c in doc.get('components') or []:
            if isinstance(c, dict) and COMPONENT_ID_RE.match(
                    str(c.get('id') or '')):
                self.components[c['id']] = c

    @classmethod
    def load(cls, path=JSON_PATH):
        with open(path, 'r', encoding='utf-8') as f:
            return cls(json.load(f))

    def lookup(self, host):
        """The entry for `host` (a name or an IP literal), or None."""
        if not host:
            return None
        host = str(host).lower().rstrip('.')
        if host.startswith('[') and host.endswith(']'):
            host = host[1:-1]
        e = self.exact.get(host)
        if e is not None:
            return e
        if _is_ip(host):
            return None
        for e in self.wild:
            suffix = e['host'][1:]
            if host.endswith(suffix) and len(host) > len(suffix):
                return e
        return None


def port_allowed(entry, port, proto='tcp'):
    """Whether `entry` allows `port` (None: a name lookup, no port)."""
    if port is None or '*' in entry['ports']:
        return True
    return '%d/%s' % (int(port), proto) in entry['ports']


# Hints for a host the doc doesn't list (or lists only as off), so that a
# failure points somewhere. First match wins; the doc's own Why is quoted
# before these.
_HOST_HINTS = (
    ('clientservices.googleapis.com', 'the variations (Finch) seed'),
    ('optimizationguide-pa.googleapis.com', 'Optimization Guide hints or '
     'model downloads'),
    ('*.gvt2.com', 'Domain Reliability beacons (gvt2) or component '
     'downloads'),
    ('*.gvt1.com', 'a component or dictionary download'),
    ('www.gstatic.com', 'a model, key or image download from gstatic'),
    ('*.gstatic.com', 'a Google static-content download'),
    ('*.fastly-edge.com', 'an Oblivious HTTP relay (Safe Browsing '
     'real-time lookups)'),
    ('*.fastly-masque.net', 'a Google privacy proxy (IP Protection or '
     'Private AI)'),
    ('*.googleapis.com', 'a Google API: the first label names the service '
     '(look it up in Chromium\'s source)'),
    ('accounts.google.com', 'Google sign-in or account cookies'),
    ('*.google.com', 'a Google service'),
    ('*.googleusercontent.com', 'Google-hosted content (images, '
     'downloads)'),
    ('*.appspot.com', 'a Google App Engine service'),
    ('*.github.com', 'GitHub (MoltBrowser updates or MoltShield lists)'),
    ('*.githubusercontent.com', 'GitHub content (MoltShield lists or '
     'updates)'),
)

_PATH_HINTS = (
    ('/service/update2', 'the Omaha update protocol: the component updater '
     '(or extension updates)'),
    ('/time/1/current', 'network time'),
    ('/v4/', 'Safe Browsing (V4)'),
    ('/v5/', 'Safe Browsing (V5)'),
    ('/safebrowsing/', 'a Safe Browsing ping or report'),
    ('/generate_204', 'a connectivity (captive portal) check'),
    ('/chrome-variations/', 'the variations seed'),
    ('/translate_a/', 'Google Translate'),
    ('/edgedl/', 'a component download'),
    ('/dict/', 'a spell-check dictionary'),
    ('/async/', 'a Google Search feature (AI Mode, ...)'),
    ('/complete/search', 'Google search suggestions'),
    ('/repos/onemannocode/moltbrowser', 'MoltBrowser\'s update check'),
)

STARTUP_WINDOW_S = 75.0


def _match(host, pattern):
    if pattern.startswith('*.'):
        suffix = pattern[1:]
        return host.endswith(suffix) and len(host) > len(suffix)
    return host == pattern


def likely_source(host, path=None, step=None, since_launch=None,
                  entry=None):
    """A short guess at what sent a request, for the report."""
    parts = []
    if entry is not None:
        parts.append('%s (NETWORK_TRAFFIC.md §%s)' % (entry['why'],
                                                     entry['section']))
    else:
        h = (host or '').lower().rstrip('.')
        for pattern, hint in _HOST_HINTS:
            if _match(h, pattern):
                parts.append(hint)
                break
    low = (path or '').lower()
    for prefix, hint in _PATH_HINTS:
        if prefix in low:
            parts.append('path: ' + hint)
            break
    if step in BROWSE_STEPS or step == 'update_check':
        desc = dict(CAPTURE_STEPS).get(step, '')
        parts.append('during the %s step (%s)' % (step, desc))
    elif since_launch is not None and 0 <= since_launch <= STARTUP_WINDOW_S:
        parts.append('%.0f s after a launch: a startup timer (the component '
                     'updater checks about 60 s after start, MoltBrowser\'s '
                     'update check at 30 s, MoltShield at 10 s)' %
                     since_launch)
    elif step:
        parts.append('during the %s step' % step)
    return '; '.join(parts) or 'unknown'
