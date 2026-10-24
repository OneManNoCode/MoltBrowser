# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Compares fingerprint records with the promised values
(expectations.py) and diffs two runs.

A record is what the harness stores per collection:

  {"label": "initial", "phase": "main", "dsf": 1, "window": [w, h] or
   null, "zoom_requested": 100, "zoom_actual": 1.0 or null,
   "kind": "full" | "geometry", "target": "moltnet" | "regular",
   "values": {flat key: value}}

Results are dicts {"key", "status": PASS|FAIL|SKIP|INFO, "expected",
"actual", "doc", "note"}.
"""

import re

from . import expectations as ex

PASS, FAIL, SKIP, INFO = 'PASS', 'FAIL', 'SKIP', 'INFO'

_MISSING = object()


def _get(values, key):
    if key in values:
        return values[key]
    return _MISSING


def _is_error(value):
    return isinstance(value, dict) and 'error' in value and len(value) <= 3


def _num(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return value
    return None


def _result(key, status, expected, actual, doc, note=''):
    return {'key': key, 'status': status, 'expected': expected,
            'actual': None if actual is _MISSING else actual, 'doc': doc,
            'note': note}


def _check_baseline(value, base, direction):
    n = _num(value)
    if n is None:
        return SKIP, 'no value (no WebGL context?)'
    if n == base:
        return PASS, ''
    if direction == 'max':
        if n < base:
            return PASS, 'below the baseline: this GPU lacks it'
        return FAIL, 'above the baseline'
    if n > base:
        return PASS, 'above the baseline: this GPU needs it'
    return FAIL, 'below the floor'


def _check_range(value, expected):
    low, high = expected
    if not isinstance(value, list) or len(value) < 2:
        return SKIP, 'no value'
    a, b = value[0], value[1]
    if _num(a) is None or _num(b) is None:
        return SKIP, 'no value'
    if low is not None and a < low:
        return FAIL, 'low end below %s' % low
    if b > high:
        return FAIL, 'high end above %s' % high
    if (low is None or a == low) and b == high:
        return PASS, ''
    return PASS, 'clamped by this GPU'


def _check_precision(value, expected):
    if not isinstance(value, list) or len(value) != 3:
        return SKIP, 'no value'
    if any(_num(v) is None for v in value):
        return SKIP, 'no value'
    if any(v > e for v, e in zip(value, expected)):
        return FAIL, 'above the fixed format'
    if list(value) == list(expected):
        return PASS, ''
    return PASS, 'clamped by this GPU'


def _check_blank_ua(value):
    if value is _MISSING or _is_error(value):
        return SKIP, 'not available (no secure context?)'
    if not isinstance(value, dict):
        return FAIL, 'unexpected value'
    bad = []
    for k in ('platformVersion', 'architecture', 'bitness', 'model',
              'uaFullVersion'):
        if value.get(k, '') not in ('', None):
            bad.append('%s=%r' % (k, value.get(k)))
    for k in ('fullVersionList', 'formFactors'):
        if value.get(k) not in ([], None):
            bad.append('%s=%r' % (k, value.get(k)))
    if value.get('wow64') not in (False, None):
        bad.append('wow64=%r' % value.get('wow64'))
    if bad:
        return FAIL, 'not blank: ' + ', '.join(bad)
    return PASS, ''


def _check_hints(value, allowed):
    if value is _MISSING or value is None:
        return SKIP, 'no request headers recorded'
    if not isinstance(value, dict):
        return FAIL, 'unexpected value'
    extra = sorted(h for h in value if h.lower() not in allowed)
    if extra:
        return FAIL, 'sent: ' + ', '.join(extra)
    return PASS, ''


def check_rule(values, rule, os_name=None):
    key, op, expected, doc = rule
    value = _get(values, key)
    if op == 'blank_ua':
        status, note = _check_blank_ua(value)
        return _result(key, status, 'blank', value, doc, note)
    if op == 'only_hints':
        status, note = _check_hints(value, expected)
        return _result(key, status, 'only ' + ', '.join(expected), value,
                       doc, note)
    if value is _MISSING:
        return _result(key, SKIP, expected, value, doc, 'not collected')
    if _is_error(value) and op not in ('eq', 'in'):
        return _result(key, SKIP, expected, value, doc,
                       'probe failed: %s' % value.get('error'))
    status, note = FAIL, ''
    if op == 'eq':
        status = PASS if value == expected else FAIL
    elif op == 'in':
        status = PASS if value in expected else FAIL
    elif op == 'lt':
        n = _num(value)
        status = PASS if n is not None and n < expected else FAIL
    elif op == 'eq_key':
        other = _get(values, expected)
        if other is _MISSING:
            return _result(key, SKIP, expected, value, doc,
                           '%s not collected' % expected)
        status = PASS if value == other else FAIL
        expected = '%s (%r)' % (expected, other)
    elif op == 'step':
        step, cap = expected
        n = _num(value)
        status = PASS if (n is not None and n > 0 and n % step == 0 and
                          n <= cap) else FAIL
        expected = 'a multiple of %d, at most %d' % (step, cap)
    elif op.startswith('baseline_'):
        status, note = _check_baseline(value, expected, op[len('baseline_'):])
    elif op == 'range_clamp':
        status, note = _check_range(value, expected)
    elif op == 'precision':
        status, note = _check_precision(value, expected)
    elif op == 'all_le':
        if not isinstance(value, list):
            status, note = SKIP, 'no value'
        else:
            status = PASS if all(_num(v) is not None and v <= expected
                                 for v in value) else FAIL
    elif op == 'subset':
        allowed = expected
        if expected == 'per_os':
            allowed = ex.font_allowlist(os_name)
            if allowed is None:
                return _result(key, INFO, 'the %s allow-list' % os_name,
                               value, doc, 'no allow-list in the kit for this '
                               'OS; compared between runs only')
        if not isinstance(value, list):
            status, note = SKIP, 'no value'
        else:
            lowered = set(a.lower() for a in allowed)
            extra = sorted(v for v in value if str(v).lower() not in lowered)
            status = FAIL if extra else PASS
            note = ('not on the list: ' + ', '.join(extra)) if extra else ''
            expected = 'a subset of the %d allowed' % len(allowed)
    elif op == 'orientation':
        w = _num(_get(values, 'screen.width'))
        h = _num(_get(values, 'screen.height'))
        if w is None or h is None:
            return _result(key, SKIP, None, value, doc, 'no screen size')
        expected = 'landscape-primary' if w > h else 'portrait-primary'
        status = PASS if value == expected else FAIL
    elif op == 'regex':
        status = PASS if (isinstance(value, str) and
                          re.search(expected, value)) else FAIL
    elif op == 'per_os_eq':
        if os_name not in expected:
            return _result(key, INFO, expected, value, doc,
                           'no expectation for %s' % os_name)
        expected = expected[os_name]
        status = PASS if value == expected else FAIL
    else:
        return _result(key, SKIP, expected, value, doc, 'unknown rule ' + op)
    return _result(key, status, expected, value, doc, note)


def check_record(record, os_name):
    """All rules that apply to one record."""
    values = record.get('values') or {}
    rules = list(ex.GEOMETRY_RULES)
    if record.get('kind') == 'full':
        rules += list(ex.FULL_RULES)
    return [check_rule(values, rule, os_name) for rule in rules]


def os_of(values):
    platform = values.get('uad.platform') or ''
    if not platform:
        ua = values.get('nav.userAgent') or ''
        if 'Windows' in ua:
            platform = 'Windows'
        elif 'Mac OS X' in ua or 'Macintosh' in ua:
            platform = 'macOS'
        elif 'Linux' in ua or 'X11' in ua:
            platform = 'Linux'
    return platform or 'unknown'


def record_id(record):
    """Identifies "the same collection" across two runs."""
    window = record.get('window')
    return '%s|dsf=%s|window=%s|zoom=%s|%s' % (
        record.get('label'), record.get('dsf'),
        'x'.join(str(v) for v in window) if window else 'initial',
        record.get('zoom_requested'), record.get('target'))


def diff_records(records_a, records_b):
    """Compares two runs' MoltNet records. Returns (compared, diffs), where
    each diff is {"record", "key", "a", "b", "severity"}: "fail" for a key a
    rule or DIFF_KEYS covers, "info" for INFO_KEYS."""
    index_b = {}
    for rec in records_b:
        if rec.get('target') == 'moltnet':
            index_b[record_id(rec)] = rec
    compared = 0
    diffs = []
    rule_keys = set(r[0] for r in ex.GEOMETRY_RULES + ex.FULL_RULES)
    uniform_keys = rule_keys | set(ex.DIFF_KEYS)
    for rec_a in records_a:
        if rec_a.get('target') != 'moltnet':
            continue
        rid = record_id(rec_a)
        rec_b = index_b.get(rid)
        if rec_b is None:
            continue
        compared += 1
        va = rec_a.get('values') or {}
        vb = rec_b.get('values') or {}
        for key in sorted(set(va) | set(vb)):
            a = va.get(key, None)
            b = vb.get(key, None)
            if a == b or key in ex.FARBLED_KEYS:
                continue
            if key in ex.INFO_KEYS:
                severity = 'info'
            elif key in uniform_keys:
                severity = 'fail'
            else:
                continue
            diffs.append({'record': rid, 'key': key, 'a': a, 'b': b,
                          'severity': severity})
    return compared, diffs
