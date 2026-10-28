# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Certificates (openssl CLI), the kit's own Tor for the onion services,
a SOCKS5 probe, the consensus check and the test extension."""

import base64
import binascii
import json
import os
import random
import socket
import struct
import subprocess
import time

BASE32 = 'abcdefghijklmnopqrstuvwxyz234567'


def fake_onion(rng=None):
    """A syntactically v3-shaped onion name nobody hosts (regular-window
    tests in control runs, where no onion service exists)."""
    rng = rng or random.SystemRandom()
    return ''.join(rng.choice(BASE32) for _ in range(55)) + 'd.onion'


def nonce(rng=None, n=10):
    rng = rng or random.SystemRandom()
    return ''.join(rng.choice('abcdefghijkmnpqrstuvwxyz23456789')
                   for _ in range(n))


def _openssl(*args, **kw):
    proc = subprocess.run(['openssl'] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kw)
    if proc.returncode != 0:
        raise RuntimeError('openssl %s: %s' % (args[0], proc.stderr.decode(
            'utf-8', 'replace').strip()[:300]))
    return proc.stdout


def spki_sha256_b64(cert_path):
    pub = _openssl('x509', '-in', cert_path, '-pubkey', '-noout')
    der = _openssl('pkey', '-pubin', '-outform', 'der', input=pub)
    import hashlib
    return base64.b64encode(hashlib.sha256(der).digest()).decode()


def make_certs(out_dir, names, ips, aia):
    """Writes server.crt/key (self-signed, for the HTTPS test origins; the
    browser accepts it through --ignore-certificate-errors-spki-list, which
    names only this key) and aia.crt/key (a leaf whose issuer is NOT sent,
    with AIA, OCSP and CRL URLs, to see where the browser fetches the
    missing intermediate). `aia` = {"ca_issuers": [urls], "ocsp": url,
    "crl": url}. Returns the SPKI pin of server.crt."""
    os.makedirs(out_dir, exist_ok=True)

    def p(name):
        return os.path.join(out_dir, name)
    san = ','.join(['DNS:' + n for n in names] + ['IP:' + i for i in ips])
    _openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt',
             'ec_paramgen_curve:prime256v1', '-nodes', '-keyout',
             p('server.key'), '-out', p('server.crt'), '-days', '7',
             '-subj', '/CN=moltleak test server', '-addext',
             'subjectAltName=' + san)
    # Root -> intermediate -> leaf; the server sends only the leaf.
    with open(p('ca.cnf'), 'w') as f:
        f.write('[v3_ca]\nbasicConstraints=critical,CA:TRUE\n'
                'keyUsage=critical,keyCertSign,cRLSign\n'
                'subjectKeyIdentifier=hash\n'
                '[v3_leaf]\nbasicConstraints=critical,CA:FALSE\n'
                'keyUsage=critical,digitalSignature\n'
                'extendedKeyUsage=serverAuth\n'
                'subjectAltName=%s\n'
                'authorityInfoAccess=%s\n'
                'crlDistributionPoints=URI:%s\n' % (
                    san, ','.join(['caIssuers;URI:' + u
                                   for u in aia['ca_issuers']] +
                                  ['OCSP;URI:' + aia['ocsp']]), aia['crl']))
    _openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt',
             'ec_paramgen_curve:prime256v1', '-nodes', '-keyout',
             p('root.key'), '-out', p('root.crt'), '-days', '7', '-subj',
             '/CN=moltleak untrusted root', '-extensions', 'v3_ca',
             '-config', _minimal_cnf(out_dir))
    _openssl('req', '-newkey', 'ec', '-pkeyopt',
             'ec_paramgen_curve:prime256v1', '-nodes', '-keyout',
             p('int.key'), '-out', p('int.csr'), '-subj',
             '/CN=moltleak intermediate')
    _openssl('x509', '-req', '-in', p('int.csr'), '-CA', p('root.crt'),
             '-CAkey', p('root.key'), '-CAcreateserial', '-out',
             p('int.crt'), '-days', '7', '-extfile', p('ca.cnf'),
             '-extensions', 'v3_ca')
    _openssl('req', '-newkey', 'ec', '-pkeyopt',
             'ec_paramgen_curve:prime256v1', '-nodes', '-keyout',
             p('aia.key'), '-out', p('aia.csr'), '-subj',
             '/CN=moltleak aia leaf')
    _openssl('x509', '-req', '-in', p('aia.csr'), '-CA', p('int.crt'),
             '-CAkey', p('int.key'), '-CAcreateserial', '-out', p('aia.crt'),
             '-days', '7', '-extfile', p('ca.cnf'), '-extensions', 'v3_leaf')
    return spki_sha256_b64(p('server.crt'))


def _minimal_cnf(out_dir):
    path = os.path.join(out_dir, 'req.cnf')
    with open(path, 'w') as f:
        f.write('[req]\ndistinguished_name=dn\n[dn]\n'
                '[v3_ca]\nbasicConstraints=critical,CA:TRUE\n'
                'keyUsage=critical,keyCertSign,cRLSign\n'
                'subjectKeyIdentifier=hash\n')
    return path


def onion_torrc(data_dir, socks_port, services):
    """torrc for the kit's own Tor: `services` = [(dir, [(virt, port)])]."""
    lines = ['DataDirectory %s' % data_dir,
             'SocksPort 127.0.0.1:%d' % socks_port,
             'Log notice file %s' % os.path.join(data_dir, 'notice.log'),
             'AvoidDiskWrites 1']
    for hs_dir, ports in services:
        lines.append('HiddenServiceDir %s' % hs_dir)
        lines.append('HiddenServiceVersion 3')
        for virt, port in ports:
            lines.append('HiddenServicePort %d 127.0.0.1:%d' % (virt, port))
    return '\n'.join(lines) + '\n'


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def socks5_http_get(socks_port, host, port, path, timeout=60.0):
    """GET http://host:port/path through a SOCKS5 proxy on 127.0.0.1.
    Returns the status line, or raises OSError."""
    s = socket.create_connection(('127.0.0.1', socks_port), timeout=timeout)
    try:
        s.sendall(b'\x05\x01\x00')
        if s.recv(2) != b'\x05\x00':
            raise OSError('SOCKS greeting refused')
        hb = host.encode('ascii')
        s.sendall(b'\x05\x01\x00\x03' + bytes([len(hb)]) + hb +
                  struct.pack('!H', port))
        reply = s.recv(10)
        if len(reply) < 2 or reply[1] != 0:
            raise OSError('SOCKS connect failed (%s)' % (
                reply[1] if len(reply) > 1 else '?'))
        s.sendall(('GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n'
                   % (path, host)).encode('ascii'))
        data = s.recv(256)
        return data.split(b'\r\n', 1)[0].decode('latin-1')
    finally:
        s.close()


def wait_onion_reachable(socks_port, host, timeout, log):
    deadline = time.time() + timeout
    last = ''
    while time.time() < deadline:
        try:
            status = socks5_http_get(socks_port, host, 80, '/ping', 60)
            if ' 200 ' in status + ' ':
                return True, ''
            last = status
        except OSError as e:
            last = str(e)
        log('onion %s not reachable yet: %s' % (host[:16], last))
        time.sleep(5)
    return False, last


def _read_text(path, read=None):
    """The text of the file `path`, or None. `read(path)`, if given, gives
    its bytes or None (the harness, as root, reads a file of the run
    directory from its descriptor: harness.Run.read_user_path()); else the
    file is opened."""
    if read is not None:
        data = read(path)
        return None if data is None else data.decode('utf-8', 'replace')
    try:
        with open(path, 'r', errors='replace') as f:
            return f.read()
    except (IOError, OSError):
        return None


def consensus_identities(data_dir, read=None):
    """Hex fingerprints of the relays in the consensus Tor cached (`read`:
    as for _read_text())."""
    out = set()
    for name in ('cached-microdesc-consensus', 'cached-consensus'):
        text = _read_text(os.path.join(data_dir, name), read)
        if text is None:
            continue
        for line in text.splitlines():
            if not line.startswith('r '):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            b64 = parts[2] + '=' * (-len(parts[2]) % 4)
            try:
                out.add(binascii.hexlify(
                    base64.b64decode(b64)).decode().upper())
            except (ValueError, binascii.Error):
                continue
    return out


def torrc_value(torrc_path, key, read=None):
    """The value of `key` in the torrc at `torrc_path`, or None (`read`: as
    for _read_text())."""
    text = _read_text(torrc_path, read)
    if text is None:
        return None
    for line in text.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[0] == key:
            return parts[1]
    return None


def write_extension(out_dir, nonce_value, canary, onions):
    """An unpacked test extension (docs/LEAK_TESTING.md, step 16)."""
    os.makedirs(out_dir, exist_ok=True)
    manifest = {
        'manifest_version': 3, 'name': 'moltleak test extension',
        'version': '1.0', 'incognito': 'spanning',
        'description': 'Leak kit probe: marks pages, reports MoltNet tabs.',
        'permissions': ['tabs', 'webRequest'],
        'host_permissions': ['<all_urls>'],
        'background': {'service_worker': 'bg.js'},
        'content_scripts': [{'matches': ['<all_urls>'], 'js': ['cs.js'],
                             'run_at': 'document_start', 'all_frames': True}],
        'web_accessible_resources': [{'resources': ['war.txt'],
                                      'matches': ['<all_urls>']}],
    }
    cfg = json.dumps({'nonce': nonce_value, 'canary': canary,
                      'onions': onions})
    with open(os.path.join(out_dir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(out_dir, 'bg.js'), 'w') as f:
        f.write('const CFG = %s;\n' % cfg + '''
function beacon(kind) {
  fetch('https://' + kind + '-' + CFG.nonce + '.' + CFG.canary + '/')
      .catch(() => {});
}
function seen(url) {
  return CFG.onions.some((o) => (url || '').includes(o));
}
chrome.tabs.onUpdated.addListener((id, info, tab) => {
  if (seen(info.url) || seen(tab && tab.url)) beacon('exttab');
});
chrome.webRequest.onBeforeRequest.addListener((d) => {
  if (seen(d.url)) beacon('extreq');
}, {urls: ['<all_urls>']});
''')
    with open(os.path.join(out_dir, 'cs.js'), 'w') as f:
        f.write('const CFG = %s;\n' % cfg + '''
document.documentElement.setAttribute('data-moltleak-ext', '1');
fetch('https://extcs-' + CFG.nonce + '.' + CFG.canary + '/').catch(() => {});
''')
    with open(os.path.join(out_dir, 'war.txt'), 'w') as f:
        f.write('web accessible\n')
    return out_dir
