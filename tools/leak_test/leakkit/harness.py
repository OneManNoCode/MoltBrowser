# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs a built browser through the leak and uniformity suite.

Runs as root (the namespace, the firewall and the capture need it); the
browser, Tor and every server run as the invoking user. Writes a run
directory (manifest.json, capture/, logs/) that report.py turns into
report.md and report.json. See docs/LEAK_TESTING.md for the whole flow.
"""

import datetime
import json
import os
import platform
import pwd
import shutil
import signal
import subprocess
import sys
import threading
import time

from . import KIT_VERSION
from . import catalog
from . import cdp as cdplib
from . import netns
from . import support

HERE = os.path.dirname(os.path.abspath(__file__))
KIT_DIR = os.path.dirname(HERE)
PAGES = os.path.join(KIT_DIR, 'pages')
ENTRY = os.path.join(KIT_DIR, 'leaktest.py')

LOCAL_HTTP, LOCAL_HTTPS, LOCAL_AIA = 18080, 18443, 18444
LOOPBACK_CANARY = 47321
PROXY_PORT = 18888
CONTROL_SOCKS = 19050
CANARY_DOMAIN = 'moltleak.test'

ZOOM_STEPS = {  # Chromium's preset levels from 100%: key presses needed
    25: ('-', 7), 33: ('-', 6), 50: ('-', 5), 67: ('-', 4), 75: ('-', 3),
    80: ('-', 2), 90: ('-', 1), 110: ('+', 1), 125: ('+', 2),
    150: ('+', 3), 175: ('+', 4), 200: ('+', 5), 250: ('+', 6),
    300: ('+', 7),
}
_KEYS = {'+': (187, 'Equal', '='), '-': (189, 'Minus', '-'),
         '0': (48, 'Digit0', '0')}

RESOLV = 'nameserver 127.0.0.1\noptions timeout:1 attempts:1\n'
NSSWITCH = ('passwd: files\ngroup: files\nshadow: files\nhosts: files dns\n'
            'networks: files\nprotocols: files\nservices: files\n'
            'ethers: files\nrpc: files\n')


class Options(object):
    def __init__(self, **kw):
        self.browser = kw.get('browser')
        self.out = kw.get('out') or os.path.join(os.getcwd(), 'moltleak-runs')
        self.target = kw.get('target', 'moltnet')
        self.display = kw.get('display', 'auto')
        self.screen = kw.get('screen', (1920, 1080))
        self.window_sizes = kw.get('window_sizes') or [
            (1920, 1080), (1366, 768), (1280, 800), (1024, 768), (800, 600)]
        self.zooms = kw.get('zooms') if kw.get('zooms') is not None else [
            50, 90, 125, 200]
        self.dsfs = kw.get('dsfs') or [1.0, 2.0]
        self.locale = kw.get('locale')
        self.timezone = kw.get('timezone')
        self.tests = kw.get('tests')          # None = all
        self.phases = kw.get('phases')        # None = all
        self.browser_args = kw.get('browser_args') or []
        self.tor = kw.get('tor')
        self.offline = kw.get('offline', False)
        self.control_tor = kw.get('control_tor', False)
        self.user = kw.get('user')
        self.final_wait = kw.get('final_wait', 20)
        self.onion_timeout = kw.get('onion_timeout', 300)
        self.open_timeout = kw.get('open_timeout', 300)
        self.extension = kw.get('extension', True)
        self.seed_settings = kw.get('seed_settings', True)

    def as_dict(self):
        d = dict(self.__dict__)
        d['screen'] = list(self.screen)
        d['window_sizes'] = [list(s) for s in self.window_sizes]
        return d


def _now():
    return time.time()


def _iso(ts):
    return datetime.datetime.fromtimestamp(
        ts, datetime.timezone.utc).replace(microsecond=0).isoformat()


class Run(object):
    """One run: the network sandbox, the servers, and each browser launch."""

    def __init__(self, opts):
        self.opts = opts
        self.run_id = os.urandom(3).hex()
        user = opts.user or os.environ.get('SUDO_USER')
        if not user or user == 'root':
            raise SystemExit('run this with sudo from your user account, or '
                             'pass --user NAME (the browser never runs as '
                             'root)')
        pw = pwd.getpwnam(user)
        self.user, self.uid, self.gid = user, pw.pw_uid, pw.pw_gid
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        self.dir = os.path.join(os.path.abspath(opts.out), '%s-%s' % (
            stamp, self.run_id))
        self.procs = []
        self.sandbox = None
        self.manifest = {
            'schema': 'moltleak-run/1', 'kit_version': KIT_VERSION,
            'run_id': self.run_id, 'target': opts.target,
            'options': opts.as_dict(), 'tests': [], 'phases': [],
            'fingerprints': [], 'errors': [],
            'captures': {'all': 'capture/all.pcap',
                         'blocked': 'capture/blocked.pcap',
                         'tor': 'capture/tor.pcap'},
            'logs': {'dns': 'logs/dns_canary.jsonl',
                     'loopback': 'logs/loopback_canary.jsonl',
                     'server': 'logs/server.jsonl',
                     'local_server': 'logs/local_server.jsonl'},
            'canary': {'domain': CANARY_DOMAIN,
                       'loopback_port': LOOPBACK_CANARY},
        }
        self.browser = None
        self.display = None
        self.spki = None
        self.onion = None
        self.default_ctx = None
        self.molt_net_tab = None   # (target id, session) of chrome://molt-net
        self.moltnet = None        # (target id, session, context id)
        self.regular = None        # (target id, session)
        self.tests = {}

    # ---- logging ------------------------------------------------------------

    def log(self, msg):
        line = '[%s] %s' % (datetime.datetime.now().strftime('%H:%M:%S'), msg)
        print(line, flush=True)
        try:
            with open(os.path.join(self.dir, 'harness.log'), 'a') as f:
                f.write(line + '\n')
        except (IOError, OSError):
            pass

    def error(self, msg):
        self.log('ERROR: ' + msg)
        self.manifest['errors'].append(msg)

    def path(self, *parts):
        return os.path.join(self.dir, *parts)

    def user_mkdir(self, *parts, mode=0o755):
        p = self.path(*parts)
        os.makedirs(p, exist_ok=True)
        os.chmod(p, mode)
        os.chown(p, self.uid, self.gid)
        return p

    def user_write(self, rel, text, mode=0o644):
        p = self.path(rel)
        with open(p, 'w') as f:
            f.write(text)
        os.chmod(p, mode)
        os.chown(p, self.uid, self.gid)
        return p

    # ---- preflight ----------------------------------------------------------

    def preflight(self):
        problems = []
        if os.geteuid() != 0:
            problems.append('needs root (sudo): network namespace, nftables '
                            'and the capture')
        need = ['ip', 'nft', 'nsenter', 'unshare', 'setpriv', 'tcpdump',
                'openssl']
        if self.opts.target == 'moltnet' or self.opts.control_tor:
            need.append('tor')
        for tool in need:
            if not shutil.which(tool):
                problems.append('missing tool: %s' % tool)
        if not self.opts.browser or not os.access(self.opts.browser, os.X_OK):
            problems.append('browser not found or not executable: %s' %
                            self.opts.browser)
        if not netns.cgroup2_mount():
            problems.append('cgroup v2 is not mounted')
        if self.opts.display in ('auto', 'xvfb') and not shutil.which('Xvfb'):
            if self.opts.display == 'xvfb':
                problems.append('missing tool: Xvfb')
        if problems:
            raise SystemExit('preflight failed:\n  ' + '\n  '.join(problems))

    def real_tor(self):
        if self.opts.tor:
            return os.path.realpath(self.opts.tor)
        bundled = os.path.join(os.path.dirname(os.path.realpath(
            self.opts.browser)), 'tor', 'tor')
        if os.path.isfile(bundled):
            return bundled
        for p in ('/usr/bin/tor', '/usr/sbin/tor'):
            if os.path.isfile(p):
                return p
        return shutil.which('tor')

    # ---- processes ----------------------------------------------------------

    def start(self, name, argv, cgroup='kit', env=None, ready=True,
              in_ns=True, timeout=20):
        logf = open(self.path('logs', name + '.out'), 'w')
        os.chown(logf.name, self.uid, self.gid)
        if in_ns:
            proc = self.sandbox.launch(argv, cgroup, env or self.kit_env(),
                                       stdout=subprocess.PIPE if ready else
                                       logf, stderr=logf)
        else:
            proc = subprocess.Popen(
                ['setpriv', '--reuid=%d' % self.uid, '--regid=%d' % self.gid,
                 '--init-groups', '--', 'env', '-i'] +
                ['%s=%s' % kv for kv in sorted((env or self.kit_env()).items())]
                + list(argv), stdout=subprocess.PIPE if ready else logf,
                stderr=logf, stdin=subprocess.DEVNULL, start_new_session=True)
        self.procs.append((name, proc))
        if ready:
            deadline = _now() + timeout
            line = b''
            while _now() < deadline:
                line = proc.stdout.readline()
                if line.strip() == b'ready' or not line:
                    break
            if line.strip() != b'ready':
                raise RuntimeError('%s did not start (see logs/%s.out)' % (
                    name, name))
            threading.Thread(target=_drain, args=(proc.stdout, logf),
                             daemon=True).start()
        return proc

    def kit_env(self):
        return {'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:'
                        '/sbin:/bin', 'HOME': self.path('home'),
                'LANG': 'C.UTF-8'}

    def service(self, name, *args, in_ns=True):
        return self.start(name, [sys.executable, '-I', ENTRY, 'service'] +
                          list(args), in_ns=in_ns)

    def start_captures(self):
        for name, iface, out in (('tcpdump-all', 'any', 'all.pcap'),
                                 ('tcpdump-blocked', 'nflog:1',
                                  'blocked.pcap'),
                                 ('tcpdump-tor', 'nflog:2', 'tor.pcap')):
            logf = open(self.path('logs', name + '.out'), 'w')
            proc = subprocess.Popen(
                ['nsenter', '--net=/run/netns/' + self.sandbox.ns, '--',
                 'tcpdump', '-i', iface, '-n', '-U', '-s', '0', '-Z',
                 self.user, '-w', self.path('capture', out)],
                stdout=logf, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
            deadline = _now() + 15
            ok = False
            while _now() < deadline:
                line = proc.stderr.readline()
                logf.write(line.decode('utf-8', 'replace'))
                if b'listening on' in line:
                    ok = True
                    break
                if not line:
                    break
            threading.Thread(target=_drain, args=(proc.stderr, logf),
                             daemon=True).start()
            if not ok:
                raise RuntimeError('tcpdump on %s did not start (see '
                                   'logs/%s.out)' % (iface, name))
            self.procs.append((name, proc))

    def stop_all(self):
        for name, proc in reversed(self.procs):
            if proc.poll() is None:
                try:
                    if name.startswith('tcpdump'):
                        proc.send_signal(signal.SIGINT)
                    else:
                        os.killpg(proc.pid, signal.SIGTERM)
                except OSError:
                    pass
        for name, proc in reversed(self.procs):
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                try:
                    proc.kill()
                except OSError:
                    pass
        self.procs = []

    # ---- setup --------------------------------------------------------------

    def setup(self):
        os.makedirs(self.dir)
        os.chown(self.dir, self.uid, self.gid)
        for d in ('capture', 'logs', 'home', 'certs', 'onion', 'xdg'):
            self.user_mkdir(d, mode=0o700 if d in ('home', 'xdg', 'onion')
                            else 0o755)
        self.manifest['t0'] = _now()
        self.manifest['started'] = _iso(self.manifest['t0'])
        self.manifest['host'] = {'os': platform.platform(),
                                 'python': platform.python_version()}
        self.manifest['browser'] = self.browser_version()
        self.user_write('resolv.conf', RESOLV)
        self.user_write('nsswitch.conf', NSSWITCH)
        self.sandbox = netns.Sandbox(self.run_id, self.uid, self.gid, self.dir,
                                     self.log)
        self.manifest['net'] = self.sandbox.setup(LOOPBACK_CANARY)
        self.log('namespace %s, gateway %s' % (self.sandbox.ns,
                                               self.manifest['net']['gw_ipv4']))
        self.start_captures()
        self.service('dns', 'dns', '--listen', '127.0.0.1', '--port', '53',
                     '--log', self.path('logs', 'dns_canary.jsonl'))
        self.service('loopback', 'loopback', '--port', str(LOOPBACK_CANARY),
                     '--log', self.path('logs', 'loopback_canary.jsonl'))
        self.assign_tests()
        if self.opts.target == 'moltnet':
            self.start_onion()
        else:
            self.onion = {'a': support.fake_onion(), 'b': None}
        self.make_certs_and_servers()
        if self.use_display_xvfb():
            self.start_xvfb()

    def browser_version(self):
        try:
            out = subprocess.run([self.opts.browser, '--version'],
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, timeout=20,
                                 universal_newlines=True,
                                 preexec_fn=_drop(self.uid, self.gid)).stdout
        except (OSError, subprocess.TimeoutExpired):
            out = ''
        out = out.strip()
        parts = out.rsplit(' ', 1)
        return {'path': self.opts.browser, 'product': parts[0] if parts else
                '', 'version': parts[1] if len(parts) > 1 else ''}

    def assign_tests(self):
        selected = set(self.opts.tests or [t.id for t in catalog.ALL_TESTS])
        for i, test in enumerate(catalog.ALL_TESTS):
            entry = {'id': test.id, 'target': test.target,
                     'nonce': support.nonce(), 'origin': test.origin,
                     'ipv4': '198.51.100.%d' % (10 + i),
                     'ipv6': '2001:db8::%x' % (0x10 + i),
                     'forbidden_hosts': list(test.forbidden_hosts)}
            if test.id not in selected:
                entry['skipped'] = 'not selected'
            self.tests[test.id] = entry
            self.manifest['tests'].append(entry)

    def start_onion(self):
        tor = shutil.which('tor')
        data = self.path('onion')
        socks = support.free_port()
        services = [(os.path.join(data, 'a'), [(80, 28080), (443, 28443),
                                               (8443, 28444)]),
                    (os.path.join(data, 'b'), [(80, 28080), (443, 28443),
                                               (8443, 28444)])]
        self.user_write('onion/torrc', support.onion_torrc(data, socks,
                                                           services))
        self.start('onion-tor', [tor, '-f', self.path('onion', 'torrc')],
                   ready=False, in_ns=False)
        deadline = _now() + 120
        hosts = {}
        while _now() < deadline and len(hosts) < 2:
            for name in ('a', 'b'):
                try:
                    with open(os.path.join(data, name, 'hostname')) as f:
                        hosts[name] = f.read().strip()
                except (IOError, OSError):
                    pass
            time.sleep(1)
        if len(hosts) < 2:
            raise RuntimeError('the kit\'s Tor did not create its onion '
                               'services (see onion/notice.log)')
        self.onion = hosts
        self.onion['socks'] = socks
        self.manifest['onion'] = {'a': hosts['a'], 'b': hosts['b']}
        self.log('onion services: %s, %s' % (hosts['a'], hosts['b']))

    def bases(self):
        if self.opts.target == 'moltnet':
            a, b = self.onion['a'], self.onion['b']
            return {'BASE_HTTP': 'http://' + a, 'BASE_HTTPS': 'https://' + a,
                    'BASE_B_HTTPS': 'https://' + b,
                    'BASE_AIA': 'https://%s:8443' % a,
                    'BASE_B_AIA': 'https://%s:8443' % b,
                    'BASE_LOCAL': 'http://127.0.0.1:%d' % LOCAL_HTTP,
                    'ONION_A': a, 'ONION_B': b}
        return {'BASE_HTTP': 'http://127.0.0.1:%d' % LOCAL_HTTP,
                'BASE_HTTPS': 'https://127.0.0.1:%d' % LOCAL_HTTPS,
                'BASE_B_HTTPS': 'https://localhost:%d' % LOCAL_HTTPS,
                'BASE_AIA': 'https://127.0.0.1:%d' % LOCAL_AIA,
                'BASE_B_AIA': 'https://localhost:%d' % LOCAL_AIA,
                'BASE_LOCAL': 'http://127.0.0.1:%d' % LOCAL_HTTP,
                'ONION_A': self.onion['a'], 'ONION_B': self.onion['a']}

    def make_certs_and_servers(self):
        bases = self.bases()
        names = ['localhost']
        if self.opts.target == 'moltnet':
            names = [self.onion['a'], self.onion['b']] + names
        aia_nonce = self.tests['cert_fetch']['nonce']
        aia = {'ca_issuers': ['%s/canary/aia-%s.crt' % (bases['BASE_HTTP'],
                                                        aia_nonce),
                              'http://aia-%s.%s/int.crt' % (aia_nonce,
                                                            CANARY_DOMAIN)],
               'ocsp': 'http://ocsp-%s.%s/' % (aia_nonce, CANARY_DOMAIN),
               'crl': 'http://crl-%s.%s/c.crl' % (aia_nonce, CANARY_DOMAIN)}
        self.spki = support.make_certs(self.path('certs'), names,
                                       ['127.0.0.1'], aia)
        for f in os.listdir(self.path('certs')):
            os.chown(self.path('certs', f), self.uid, self.gid)
        values = dict(bases)
        values.update({'LPORT': LOOPBACK_CANARY,
                       'GW': self.manifest['net']['gw_ipv4'],
                       'GW6': self.manifest['net']['gw_ipv6'],
                       'NSIP': self.manifest['net']['ns_ipv4']})
        config = {'pages_dir': PAGES, 'canary': CANARY_DOMAIN,
                  'values': values,
                  'tests': {t['nonce']: {'id': t['id'], 'ipv4': t['ipv4'],
                                         'ipv6': t['ipv6']}
                            for t in self.manifest['tests']}}
        self.user_write('pages.json', json.dumps(config, indent=1))
        cert, key = self.path('certs', 'server.crt'), self.path('certs',
                                                                'server.key')
        acert, akey = self.path('certs', 'aia.crt'), self.path('certs',
                                                               'aia.key')
        # Loopback inside the namespace: regular-window pages (and every
        # page in a control run).
        self.service('local-pages', 'pages', '--config',
                     self.path('pages.json'), '--log',
                     self.path('logs', 'local_server.jsonl'),
                     '--http', '127.0.0.1:%d' % LOCAL_HTTP,
                     '--https', '127.0.0.1:%d:%s:%s' % (LOCAL_HTTPS, cert,
                                                        key),
                     '--https', '127.0.0.1:%d:%s:%s' % (LOCAL_AIA, acert,
                                                        akey))
        if self.opts.target != 'moltnet':
            self.manifest['logs']['server'] = 'logs/local_server.jsonl'
            return
        # Behind the onion services, on the host's loopback (unreachable
        # from the namespace except through Tor).
        self.service('onion-pages', 'pages', '--config',
                     self.path('pages.json'), '--log',
                     self.path('logs', 'server.jsonl'),
                     '--http', '127.0.0.1:28080',
                     '--https', '127.0.0.1:28443:%s:%s' % (cert, key),
                     '--https', '127.0.0.1:28444:%s:%s' % (acert, akey),
                     in_ns=False)
        ok, why = support.wait_onion_reachable(
            self.onion['socks'], self.onion['a'], self.opts.onion_timeout,
            self.log)
        if not ok:
            raise RuntimeError('the kit\'s onion service is not reachable '
                               'through Tor: %s' % why)
        self.log('onion service reachable')

    def use_display_xvfb(self):
        if self.opts.display == 'xvfb':
            return True
        if self.opts.display == 'auto':
            return bool(shutil.which('Xvfb'))
        return False

    def start_xvfb(self):
        for n in range(91, 140):
            if not os.path.exists('/tmp/.X11-unix/X%d' % n) and \
                    not os.path.exists('/tmp/.X%d-lock' % n):
                break
        w, h = self.opts.screen
        self.start('xvfb', ['Xvfb', ':%d' % n, '-screen', '0',
                            '%dx%dx24' % (w, h), '-nolisten', 'tcp',
                            '-noreset'], ready=False)
        deadline = _now() + 15
        while _now() < deadline and not os.path.exists(
                '/tmp/.X11-unix/X%d' % n):
            time.sleep(0.2)
        self.display = ':%d' % n
        self.manifest['display'] = {'kind': 'xvfb', 'screen': [w, h]}
        self.log('Xvfb on %s (%dx%d)' % (self.display, w, h))

    # ---- the browser --------------------------------------------------------

    def seed_profile(self):
        """Regular-profile settings MoltNet must not show (§4 step 15)."""
        if not self.opts.seed_settings:
            return
        prefs_dir = self.user_mkdir('profile', 'Default', mode=0o700)
        os.chown(self.path('profile'), self.uid, self.gid)
        os.chmod(self.path('profile'), 0o700)
        blocked = ['http://localhost:%d,*' % LOCAL_HTTP]
        if self.opts.target == 'moltnet':
            blocked.append('https://%s:443,*' % self.onion['a'])
        prefs = {
            'enable_do_not_track': True,
            'intl': {'accept_languages': 'de-DE,de'},
            'webkit': {'webprefs': {
                'default_font_size': 24,
                'fonts': {'standard': {'Zyyy': 'DejaVu Serif'},
                          'sansserif': {'Zyyy': 'DejaVu Serif'}}}},
            'profile': {
                'cookie_controls_mode': 0,
                'content_settings': {'exceptions': {
                    'javascript': {p: {'last_modified': '13370000000000000',
                                       'setting': 2} for p in blocked},
                    'geolocation': {p: {'last_modified': '13370000000000000',
                                        'setting': 2} for p in blocked}}}},
            'browser': {'has_seen_welcome_page': True},
        }
        path = os.path.join(prefs_dir, 'Preferences')
        if not os.path.exists(path):
            with open(path, 'w') as f:
                json.dump(prefs, f)
            os.chown(path, self.uid, self.gid)

    def browser_env(self):
        env = self.kit_env()
        env.update({'USER': self.user, 'LOGNAME': self.user,
                    'XDG_RUNTIME_DIR': self.path('xdg'),
                    'LANG': self.opts.locale or os.environ.get('LANG') or
                    'C.UTF-8'})
        if self.opts.locale:
            env['LC_ALL'] = self.opts.locale
        if self.opts.timezone:
            env['TZ'] = self.opts.timezone
        if self.display:
            env['DISPLAY'] = self.display
        elif self.opts.display == 'current':
            for k in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY'):
                if os.environ.get(k):
                    env[k] = os.environ[k]
        return env

    def launch_browser(self, label, extra_flags=(), dsf=None):
        args = [self.opts.browser,
                '--user-data-dir=' + self.path('profile'),
                '--remote-debugging-pipe', '--no-first-run',
                '--no-default-browser-check', '--password-store=basic',
                '--disable-search-engine-choice-screen',
                '--ignore-certificate-errors-spki-list=' + self.spki]
        if self.opts.target == 'moltnet':
            wrapper = self.sandbox.write_tor_wrapper(
                self.real_tor(), self.path('logs', 'tor-argv.txt'))
            args.append('--moltnet-tor-binary=' + wrapper)
        if self.opts.extension:
            args.append('--enable-unsafe-extension-debugging')
        if self.opts.display == 'headless':
            args.append('--headless=new')
        if dsf and dsf != 1:
            args.append('--force-device-scale-factor=%s' % dsf)
        if self.opts.control_tor:
            args.append('--proxy-server=socks5://127.0.0.1:%d' % CONTROL_SOCKS)
        args += list(extra_flags) + list(self.opts.browser_args)
        args.append('about:blank')
        cmd_r, cmd_w = os.pipe()
        resp_r, resp_w = os.pipe()
        child_in = _high_fd(cmd_r)
        child_out = _high_fd(resp_w)
        logf = open(self.path('logs', 'browser-%s.out' % label), 'w')
        os.chown(logf.name, self.uid, self.gid)
        proc = self.sandbox.launch(
            args, 'browser', self.browser_env(), stdout=logf, stderr=logf,
            resolv=self.path('resolv.conf'),
            nsswitch=self.path('nsswitch.conf'),
            pipe_fds=(child_in, child_out))
        for fd in (cmd_r, resp_w, child_in, child_out):
            try:
                os.close(fd)
            except OSError:
                pass
        client = cdplib.Cdp(cmd_w, resp_r)
        self.browser = (proc, client, label)
        self.log('browser launched (%s): %s' % (label, ' '.join(args[1:])))
        deadline = _now() + 60
        while _now() < deadline:
            try:
                pages = [t for t in client.targets() if t['type'] == 'page']
                if pages:
                    self.default_ctx = pages[0].get('browserContextId')
                    sid = client.attach(pages[0]['targetId'])
                    self.regular = (pages[0]['targetId'], sid)
                    client.call('Target.setDiscoverTargets',
                                {'discover': True})
                    return client
            except cdplib.CdpClosed:
                break
            except cdplib.CdpError:
                pass
            time.sleep(0.5)
        raise RuntimeError('the browser did not start (see logs/browser-%s'
                           '.out)' % label)

    def close_browser(self):
        if not self.browser:
            return
        proc, client, label = self.browser
        try:
            client.call('Browser.close', timeout=10)
        except cdplib.CdpError:
            pass
        deadline = _now() + 15
        while _now() < deadline and self.sandbox.pids('browser'):
            time.sleep(0.3)
        self.sandbox.kill_cgroup('browser')
        if self.opts.target == 'moltnet':
            self.sandbox.kill_cgroup('tor')
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        self.browser = None
        self.moltnet = None
        self.molt_net_tab = None

    @property
    def cdp(self):
        return self.browser[1]

    # ---- MoltNet ------------------------------------------------------------

    def open_molt_net_ui(self):
        if self.molt_net_tab:
            return self.molt_net_tab
        try:
            tid = self.cdp.call('Target.createTarget',
                                {'url': 'chrome://molt-net/'})['targetId']
            sid = self.cdp.attach(tid)
        except cdplib.CdpClosed:
            raise
        except cdplib.CdpError:
            # Some builds refuse to create a target at an internal URL;
            # navigate a blank one there instead.
            tid = self.cdp.call('Target.createTarget',
                                {'url': 'about:blank'})['targetId']
            sid = self.cdp.attach(tid)
            self.cdp.navigate(sid, 'chrome://molt-net/', timeout=20)
        deadline = _now() + 20
        while _now() < deadline:
            try:
                if self.cdp.evaluate(sid, 'typeof sendWithPromise') == \
                        'function':
                    self.molt_net_tab = (tid, sid)
                    return self.molt_net_tab
            except cdplib.CdpError:
                pass
            time.sleep(0.5)
        raise RuntimeError('chrome://molt-net/ did not load (is this '
                           'MoltBrowser?)')

    def molt_net_call(self, message, timeout=60):
        _, sid = self.open_molt_net_ui()
        return self.cdp.evaluate(
            sid, 'sendWithPromise(%s)' % json.dumps(message), timeout=timeout)

    def moltnet_pages(self):
        return [t for t in self.cdp.targets() if t['type'] == 'page' and
                t.get('browserContextId') and
                t.get('browserContextId') != self.default_ctx]

    def open_moltnet(self, phase_name='open_moltnet'):
        phase = self.phase_begin(phase_name)
        start = _now()
        before = set(t['targetId'] for t in self.cdp.targets())
        try:
            result = self.molt_net_call('moltnet.openWindow',
                                        timeout=self.opts.open_timeout)
        except (cdplib.CdpError, RuntimeError) as e:
            result = {'error': 'harness: %s' % e}
        phase['data']['result'] = result
        found = None
        deadline = _now() + (3 if (result or {}).get('error') else 30)
        while _now() < deadline and not found:
            for t in self.moltnet_pages():
                if t['targetId'] not in before:
                    found = t
                    break
            if not found:
                time.sleep(0.5)
        if not found:
            # For the report: what did appear (a MoltNet tab in the default
            # context would mean DevTools reports contexts differently).
            phase['data']['new_targets'] = [
                {'type': t['type'], 'url': t.get('url', '')[:100],
                 'context': t.get('browserContextId')}
                for t in self.cdp.targets() if t['targetId'] not in before]
        if found:
            sid = self.cdp.attach(found['targetId'])
            self.moltnet = (found['targetId'], sid,
                            found.get('browserContextId'))
            phase['data']['context'] = found.get('browserContextId')
            phase['data']['url'] = found.get('url')
        phase['data']['seconds'] = round(_now() - start, 1)
        self.phase_end(phase)
        return bool(found) and not (result or {}).get('error')

    # ---- phases -------------------------------------------------------------

    def phase_begin(self, name, target='moltnet'):
        phase = {'name': name, 'target': target, 'start': _now(), 'data': {}}
        self.manifest['phases'].append(phase)
        self.log('phase %s' % name)
        return phase

    def phase_end(self, phase, error=None):
        phase['end'] = _now()
        if error:
            phase['error'] = error
            self.log('phase %s: %s' % (phase['name'], error))

    def want_phase(self, name):
        return self.opts.phases is None or name in self.opts.phases

    def skip_phase(self, name, why):
        self.manifest['phases'].append({'name': name, 'skipped': why})

    def target_session(self, test):
        if test.target == 'regular' or self.opts.target == 'regular':
            return self.regular[1]
        return self.moltnet[1] if self.moltnet else None

    def test_url(self, test, entry, query=''):
        bases = self.bases()
        base = {'http': bases['BASE_HTTP'], 'https': bases['BASE_HTTPS'],
                'https_b': bases['BASE_B_HTTPS'], 'aia': bases['BASE_AIA'],
                'aia_b': bases['BASE_B_AIA'],
                'local': bases['BASE_LOCAL']}[test.origin]
        return '%s/t/%s/%s%s' % (base, entry['nonce'], test.page, query)

    def poll_results(self, sid, timeout):
        deadline = _now() + timeout
        last = None
        while _now() < deadline:
            try:
                last = self.cdp.evaluate(
                    sid, 'JSON.stringify(window.__moltleak || null)',
                    timeout=10)
                state = json.loads(last) if last else None
                if state and state.get('done'):
                    return state.get('results'), None
            except (cdplib.CdpError, ValueError) as e:
                if isinstance(e, cdplib.CdpClosed):
                    return None, str(e)
            time.sleep(0.5)
        try:
            state = json.loads(last) if last else None
        except ValueError:
            state = None
        return (state or {}).get('results'), 'the page did not finish in %ds' \
            % timeout

    def run_actions(self, sid, actions):
        log = []
        for kind, arg in actions:
            try:
                if kind == 'sleep':
                    time.sleep(arg)
                elif kind in ('eval', 'gesture_eval'):
                    self.cdp.evaluate(sid, arg, timeout=30,
                                      user_gesture=kind == 'gesture_eval')
                elif kind == 'hover':
                    box = self.cdp.evaluate(
                        sid, '(() => { const r = document.querySelector(%s)'
                        '.getBoundingClientRect(); return [r.x + r.width / 2,'
                        ' r.y + r.height / 2]; })()' % json.dumps(arg))
                    for i in range(3):
                        self.cdp.call('Input.dispatchMouseEvent', {
                            'type': 'mouseMoved', 'x': box[0] + i,
                            'y': box[1]}, session=sid)
                        time.sleep(0.2)
                log.append('%s %s: ok' % (kind, arg))
            except (cdplib.CdpError, TypeError, IndexError) as e:
                log.append('%s %s: %s' % (kind, arg, e))
        return log

    def run_test(self, test):
        entry = self.tests[test.id]
        if entry.get('skipped'):
            return
        sid = self.target_session(test)
        if sid is None:
            entry['skipped'] = 'no MoltNet window'
            return
        if test.id == 'regular_onion_navigation':
            url = 'http://%s/' % self.onion['a']
        else:
            url = self.test_url(test, entry)
        entry['url'] = url
        entry['target'] = 'regular' if (test.target == 'regular' or
                                        self.opts.target == 'regular') \
            else 'moltnet'
        self.log('test %s: %s' % (test.id, url))
        entry['start'] = _now()
        try:
            err = self.cdp.navigate(sid, url, timeout=test.timeout)
            if err:
                entry['load_error'] = err
            if test.id == 'regular_onion_navigation':
                time.sleep(1.5)
                entry['results'] = {
                    'title': self.cdp.evaluate(sid, 'document.title'),
                    'url': self.cdp.evaluate(sid, 'location.href')}
            elif test.expect_load:
                results, err = self.poll_results(sid, test.timeout)
                entry['results'] = results
                if err:
                    entry['error'] = err
                if test.actions:
                    entry['actions'] = self.run_actions(sid, test.actions)
                    more, _ = self.poll_results(sid, 5)
                    if more:
                        entry['results'] = more
        except cdplib.CdpClosed as e:
            entry['error'] = 'browser gone: %s' % e
            raise
        except cdplib.CdpError as e:
            entry['error'] = str(e)
        entry['end'] = _now()
        time.sleep(test.wait)
        entry['settle_end'] = _now()

    def moltnet_tests(self):
        for test in catalog.TESTS:
            if test.id == 'extension':
                continue
            self.run_test(test)

    # ---- fingerprints -------------------------------------------------------

    def fingerprint(self, sid, target, dsf, label_prefix='', geometry=True):
        entry = self.tests.get('onion_https') or {}
        if target == 'regular':
            base = 'https://127.0.0.1:%d' % LOCAL_HTTPS
        else:
            base = self.bases()['BASE_HTTPS']
        url = '%s/t/%s/fp.html' % (base, entry.get('nonce', 'fingerprint0'))
        err = self.cdp.navigate(sid, url, timeout=90)
        results, perr = self.poll_results(sid, 90)
        if results is None:
            self.error('fingerprint page (%s, dsf %s): %s' % (
                target, dsf, err or perr))
            try:
                # Loaded, but none of its scripts ran: JavaScript is blocked
                # there in the regular profile (seed_profile), and MoltNet
                # took that over.
                no_js = self.cdp.evaluate(
                    sid, 'document.title === "moltleak: fingerprint" && '
                    'typeof ML === "undefined"')
            except cdplib.CdpError:
                no_js = False
            if no_js and target == 'moltnet' and self.opts.seed_settings:
                self.manifest['fingerprints'].append({
                    'label': label_prefix + 'initial', 'phase': 'main',
                    'dsf': dsf, 'window': None, 'zoom_requested': 100,
                    'kind': 'full', 'target': target,
                    'note': 'the page loaded but its scripts never ran',
                    'values': {'js.enabled_on_blocked_site': False}})
            return
        values = dict(results)
        values.update(self.geometry(sid))
        rec = {'label': label_prefix + 'initial', 'phase': 'main', 'dsf': dsf,
               'window': None, 'zoom_requested': 100, 'zoom_actual':
               self.page_zoom(sid), 'kind': 'full', 'target': target,
               'values': values}
        self.manifest['fingerprints'].append(rec)
        if not geometry:
            return
        tid = self.moltnet[0] if target == 'moltnet' else self.regular[0]
        try:
            win = self.cdp.call('Browser.getWindowForTarget',
                                {'targetId': tid})
        except cdplib.CdpError as e:
            self.error('window sizes: %s' % e)
            return
        sizes = self.opts.window_sizes if dsf == 1 else \
            self.opts.window_sizes[:2]
        for w, h in sizes:
            try:
                self.cdp.call('Browser.setWindowBounds', {
                    'windowId': win['windowId'],
                    'bounds': {'windowState': 'normal'}})
                self.cdp.call('Browser.setWindowBounds', {
                    'windowId': win['windowId'],
                    'bounds': {'left': 0, 'top': 0, 'width': w,
                               'height': h}})
            except cdplib.CdpError as e:
                self.error('setWindowBounds %dx%d: %s' % (w, h, e))
                continue
            values = self.geometry(sid, settle=True)
            bounds = self.cdp.call('Browser.getWindowBounds', {
                'windowId': win['windowId']}).get('bounds')
            self.manifest['fingerprints'].append({
                'label': label_prefix + 'window', 'phase': 'main', 'dsf': dsf,
                'window': [w, h], 'window_actual': bounds,
                'zoom_requested': 100, 'zoom_actual': self.page_zoom(sid),
                'kind': 'geometry', 'target': target, 'values': values})
        if dsf != 1 or not self.opts.zooms:
            return
        w, h = self.opts.window_sizes[0]
        try:
            self.cdp.call('Browser.setWindowBounds', {
                'windowId': win['windowId'],
                'bounds': {'left': 0, 'top': 0, 'width': w, 'height': h}})
        except cdplib.CdpError:
            pass
        base_zoom = self.page_zoom(sid)
        for level in self.opts.zooms:
            if level not in ZOOM_STEPS:
                continue
            key, count = ZOOM_STEPS[level]
            self.press(sid, '0')
            for _ in range(count):
                self.press(sid, key)
                time.sleep(0.15)
            values = self.geometry(sid, settle=True)
            actual = self.page_zoom(sid)
            rec = {'label': label_prefix + 'zoom', 'phase': 'main',
                   'dsf': dsf, 'window': [w, h], 'zoom_requested': level,
                   'zoom_actual': actual if actual != base_zoom else None,
                   'kind': 'geometry', 'target': target, 'values': values}
            if actual == base_zoom:
                rec['note'] = ('Ctrl+%s through DevTools did not change the '
                               'zoom; check this level by hand' % key)
            self.manifest['fingerprints'].append(rec)
        self.press(sid, '0')

    def press(self, sid, key):
        code, dom_code, char = _KEYS[key]
        for kind in ('rawKeyDown', 'keyUp'):
            try:
                self.cdp.call('Input.dispatchKeyEvent', {
                    'type': kind, 'modifiers': 2, 'windowsVirtualKeyCode':
                    code, 'nativeVirtualKeyCode': code, 'code': dom_code,
                    'key': char}, session=sid)
            except cdplib.CdpError:
                return

    def page_zoom(self, sid):
        try:
            m = self.cdp.call('Page.getLayoutMetrics', session=sid)
        except cdplib.CdpError:
            return None
        vv = m.get('cssVisualViewport') or m.get('visualViewport') or {}
        return vv.get('zoom')

    def geometry(self, sid, settle=False):
        if settle:
            last, same = None, 0
            deadline = _now() + 6
            while _now() < deadline and same < 3:
                time.sleep(0.25)
                try:
                    cur = self.cdp.evaluate(
                        sid, '[innerWidth, innerHeight, screen.width, '
                        'screen.height].join()')
                except cdplib.CdpError:
                    cur = None
                same = same + 1 if cur == last else 0
                last = cur
        try:
            self.cdp.call('Input.dispatchMouseEvent', {
                'type': 'mouseMoved', 'x': 101, 'y': 57}, session=sid)
            time.sleep(0.2)
            return self.cdp.evaluate(sid, 'mlGeometry()') or {}
        except cdplib.CdpError as e:
            return {'geometry_error': str(e)}

    # ---- the main session ---------------------------------------------------

    def settings_control(self):
        phase = self.phase_begin('settings_seed', target='regular')
        sid = self.regular[1]
        try:
            self.cdp.navigate(sid, 'http://127.0.0.1:%d/t/seedcheck00/seed.html'
                              % LOCAL_HTTP, timeout=30)
            res, _ = self.poll_results(sid, 15)
            reg = dict(res or {})
            self.cdp.navigate(sid, 'http://localhost:%d/t/seedcheck00/seed.html'
                              % LOCAL_HTTP, timeout=30)
            time.sleep(1.5)
            try:
                ran = self.cdp.evaluate(
                    sid, 'document.documentElement.getAttribute("data-ran")')
            except cdplib.CdpError:
                ran = None
            reg['js_ran_on_blocked'] = ran == '1'
            phase['data']['regular'] = reg
            self.phase_end(phase)
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def extension_phase(self):
        test = catalog.by_id('extension')
        entry = self.tests['extension']
        if entry.get('skipped'):
            return
        if not self.moltnet:
            entry['skipped'] = 'needs a MoltNet window'
            return
        phase = self.phase_begin('extension')
        data = phase['data']
        ext_dir = support.write_extension(
            self.path('ext'), entry['nonce'], CANARY_DOMAIN,
            [self.onion['a'], self.onion['b'] or self.onion['a']])
        for root, dirs, files in os.walk(ext_dir):
            for n in dirs + files:
                os.chown(os.path.join(root, n), self.uid, self.gid)
        os.chown(ext_dir, self.uid, self.gid)
        try:
            ext_id = self.cdp.call('Extensions.loadUnpacked',
                                   {'path': ext_dir})['id']
            data['how'] = 'Extensions.loadUnpacked'
        except cdplib.CdpError as e:
            data['loaded'] = False
            data['load_error'] = str(e)
            entry['skipped'] = 'the test extension could not be loaded'
            self.phase_end(phase)
            return
        data['loaded'] = True
        data['id'] = ext_id
        tid = self.cdp.call('Target.createTarget', {
            'url': 'chrome://extensions/?id=' + ext_id})['targetId']
        try:
            sid = self.cdp.attach(tid)
            time.sleep(1.5)
            self.cdp.evaluate(sid, 'chrome.developerPrivate.'
                              'updateExtensionConfiguration({extensionId: %s,'
                              ' incognitoAccess: true})' % json.dumps(ext_id))
            info = self.cdp.evaluate(sid, 'chrome.developerPrivate.'
                                     'getExtensionInfo(%s).then(i => '
                                     'i.incognitoAccess)' % json.dumps(ext_id))
            data['incognito'] = bool((info or {}).get('isActive'))
        except cdplib.CdpError as e:
            data['incognito'] = False
            data['incognito_error'] = str(e)
        finally:
            try:
                self.cdp.call('Target.closeTarget', {'targetId': tid})
            except cdplib.CdpError:
                pass
        # Control: the extension marks a regular page.
        sid = self.regular[1]
        try:
            self.cdp.navigate(sid, 'http://127.0.0.1:%d/t/%s/ext.html?ext=%s'
                              % (LOCAL_HTTP, entry['nonce'], ext_id))
            res, _ = self.poll_results(sid, 15)
            data['regular_marked'] = bool((res or {}).get('marked'))
        except cdplib.CdpError as e:
            data['regular_error'] = str(e)
        self.phase_end(phase)
        if self.moltnet:
            entry['url_query'] = '?ext=' + ext_id
            sid = self.moltnet[1]
            entry['url'] = self.test_url(test, entry, '?ext=' + ext_id)
            entry['target'] = 'moltnet'
            entry['start'] = _now()
            try:
                self.cdp.navigate(sid, entry['url'], timeout=test.timeout)
                entry['results'], err = self.poll_results(sid, test.timeout)
                if err:
                    entry['error'] = err
            except cdplib.CdpError as e:
                entry['error'] = str(e)
            entry['end'] = _now()
            time.sleep(test.wait)
            entry['settle_end'] = _now()

    def identity_url(self, base, action):
        return '%s/t/identityxx/identity.html?%s' % (base, action)

    def read_identity(self, sid, base, action):
        self.cdp.navigate(sid, self.identity_url(base, action), timeout=90)
        res, err = self.poll_results(sid, 60)
        return res or {}

    def identity_phase(self):
        phase = self.phase_begin('identity')
        data = phase['data']
        bases = self.bases()
        try:
            self.cdp.call('Storage.setCookies', {'cookies': [{
                'name': 'mlk_regular', 'value': '1',
                'domain': self.onion['a'], 'path': '/'}]})
            sid = self.moltnet[1]
            res = self.read_identity(sid, bases['BASE_HTTP'], 'set=1')
            data['regular_cookie_in_moltnet'] = 'mlk_regular' in (
                res.get('cookie') or '')
            data['cookie_set'] = res.get('cookie')
            data['canvas_a'] = res.get('canvas')
            res_b = self.read_identity(sid, bases['BASE_B_HTTPS'], 'read=1')
            data['canvas_b'] = res_b.get('canvas')
            old_ctx = self.moltnet[2]
            self.cdp.call('Target.closeTarget', {'targetId': self.moltnet[0]})
            time.sleep(4)
            self.moltnet = None
            if not self.open_moltnet('reopen_moltnet'):
                raise RuntimeError('MoltNet did not reopen')
            res = self.read_identity(self.moltnet[1], bases['BASE_HTTP'],
                                     'read=1')
            data['cookie_after_close'] = 'mlk_moltnet' in (
                res.get('cookie') or '')
            data['same_context_after_close'] = old_ctx == self.moltnet[2]
            self.phase_end(phase)
        except (cdplib.CdpError, RuntimeError) as e:
            self.phase_end(phase, str(e))

    def exit_ip(self, sid):
        if self.opts.offline:
            return None
        try:
            self.cdp.navigate(sid, 'https://check.torproject.org/api/ip',
                              timeout=60)
            text = self.cdp.evaluate(sid, 'document.body ? '
                                     'document.body.innerText : ""')
            return json.loads(text)
        except (cdplib.CdpError, ValueError):
            return None

    def verify_exit_phase(self):
        phase = self.phase_begin('verify_exit')
        if self.opts.offline:
            phase['skipped'] = '--offline'
            self.phase_end(phase)
            return
        try:
            phase['data']['result'] = self.molt_net_call('moltnet.verifyExit',
                                                         timeout=90)
            phase['data']['page'] = self.exit_ip(self.moltnet[1])
            self.phase_end(phase)
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def circuit_phase(self):
        phase = self.phase_begin('circuit')
        try:
            res = self.molt_net_call('moltnet.getCircuit', timeout=60) or {}
            hops = res.get('hops') or []
            data_dir = support.torrc_value(self.tor_torrc() or '',
                                           'DataDirectory')
            known = support.consensus_identities(data_dir) if data_dir \
                else set()
            for hop in hops:
                fp = (hop.get('fingerprint') or '').lstrip('$').upper()
                hop['in_consensus'] = (fp in known) if known else None
            phase['data'] = {'hops': hops, 'consensus_relays': len(known)}
            self.phase_end(phase)
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def tor_torrc(self):
        try:
            with open(self.path('logs', 'tor-argv.txt')) as f:
                argv = f.read().split('\n')
            return argv[argv.index('-f') + 1]
        except (IOError, OSError, ValueError, IndexError):
            return None

    # A page that asks "Leave site?" when it is closed. With a user
    # activation (userGesture) Chromium would show the dialog; New Identity
    # must close the window anyway.
    HOLD_PAGE_JS = ("window.addEventListener('beforeunload', (e) => {"
                    "e.preventDefault(); e.returnValue = ''; }); true")

    def wait_new_moltnet_page(self, before, seconds=60):
        deadline = _now() + seconds
        while _now() < deadline:
            for t in self.moltnet_pages():
                if t['targetId'] not in before:
                    return t
            time.sleep(0.5)
        return None

    def new_identity_phase(self):
        phase = self.phase_begin('new_identity')
        data = phase['data']
        bases = self.bases()
        try:
            sid = self.moltnet[1]
            res = self.read_identity(sid, bases['BASE_HTTP'], 'set=1')
            data['canvas_before'] = res.get('canvas')
            data['exit_before'] = (self.exit_ip(sid) or {}).get('IP')
            try:
                data['hold_page'] = bool(self.cdp.evaluate(
                    sid, self.HOLD_PAGE_JS, user_gesture=True))
            except cdplib.CdpError as e:
                data['hold_page'] = 'error: %s' % e
            old = self.moltnet
            data['old_context'] = old[2]
            before = set(t['targetId'] for t in self.moltnet_pages())
            data['result'] = self.molt_net_call('moltnet.newIdentity',
                                                timeout=self.opts.open_timeout)
            first_done = _now()
            found = self.wait_new_moltnet_page(before)
            if found:
                self.moltnet = (found['targetId'],
                                self.cdp.attach(found['targetId']),
                                found.get('browserContextId'))
                data['new_context'] = found.get('browserContextId')
                # Nothing of the old identity may still be open.
                data['old_pages_left'] = len([
                    t for t in self.moltnet_pages()
                    if t.get('browserContextId') == old[2]])
                res = self.read_identity(self.moltnet[1], bases['BASE_HTTP'],
                                         'read=1')
                data['cookie_after'] = 'mlk_moltnet' in (res.get('cookie') or
                                                         '')
                data['canvas_after'] = res.get('canvas')
                data['exit_after'] = (self.exit_ip(self.moltnet[1]) or
                                      {}).get('IP')
                # A second New Identity right away: Tor applies NEWNYM at
                # most every 10 s, so its window may only open once Tor has
                # switched, about 10 s after the first one.
                before = set(t['targetId'] for t in self.moltnet_pages())
                data['second_result'] = self.molt_net_call(
                    'moltnet.newIdentity', timeout=self.opts.open_timeout)
                data['second_after_first_s'] = round(_now() - first_done, 1)
                found = self.wait_new_moltnet_page(before)
                if found:
                    self.moltnet = (found['targetId'],
                                    self.cdp.attach(found['targetId']),
                                    found.get('browserContextId'))
                    data['second_context'] = found.get('browserContextId')
            self.phase_end(phase)
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def fail_closed_phase(self):
        phase = self.phase_begin('fail_closed')
        try:
            killed = self.sandbox.kill_cgroup('tor')
            phase['data']['killed'] = killed
            time.sleep(2)
            sid = self.moltnet[1]
            url = '%s/t/failclosed0/basic.html' % self.bases()['BASE_HTTP']
            err = self.cdp.navigate(sid, url, timeout=45)
            phase['data']['nav_error'] = err
            loaded = False
            try:
                loaded = bool(self.cdp.evaluate(
                    sid, '!!(window.__moltleak && window.__moltleak.done)'))
            except cdplib.CdpError:
                pass
            phase['data']['loaded'] = loaded
            self.phase_end(phase)
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def main_session(self):
        self.seed_profile()
        self.launch_browser('main')
        if self.opts.seed_settings:
            self.settings_control()
        if self.opts.target == 'moltnet':
            if not self.open_moltnet():
                self.error('MoltNet did not open; the MoltNet tests are '
                           'skipped (see the open_moltnet phase)')
                for entry in self.manifest['tests']:
                    if entry['target'] == 'moltnet' and \
                            not entry.get('skipped'):
                        entry['skipped'] = 'MoltNet did not open'
            else:
                self.verify_exit_phase()
                self.circuit_phase()
        if self.opts.target == 'regular' or self.moltnet:
            self.moltnet_tests()
            sid = self.moltnet[1] if self.moltnet else self.regular[1]
            target = 'moltnet' if self.moltnet else 'regular'
            self.fingerprint(sid, target, 1.0)
        # control fingerprint in the regular window
        self.fingerprint(self.regular[1], 'regular', 1.0, geometry=False)
        if self.opts.extension:
            self.extension_phase()
        for test in catalog.REGULAR_TESTS:
            self.run_test(test)
        if self.moltnet:
            if self.want_phase('identity'):
                self.identity_phase()
            if self.want_phase('new_identity') and self.moltnet:
                self.new_identity_phase()
            if self.want_phase('fail_closed') and self.moltnet:
                self.fail_closed_phase()
        self.log('final wait %ds (late reports)' % self.opts.final_wait)
        time.sleep(self.opts.final_wait)
        self.close_browser()

    def restart_session(self):
        phase = self.phase_begin('restart')
        try:
            self.launch_browser('restart')
            time.sleep(8)
            phase['data']['moltnet_pages'] = len(self.moltnet_pages())
            self.phase_end(phase)
        except (RuntimeError, cdplib.CdpError) as e:
            self.phase_end(phase, str(e))
        self.close_browser()

    def override_session(self):
        self.manifest['logs']['proxy'] = 'logs/proxy.jsonl'
        self.service('proxy', 'proxy', '--listen', '127.0.0.1', '--port',
                     str(PROXY_PORT), '--log', self.path('logs',
                                                         'proxy.jsonl'))
        phase = self.phase_begin('override', target='regular')
        flag = '--proxy-server=http://127.0.0.1:%d' % PROXY_PORT
        phase['data']['flag'] = flag
        try:
            self.launch_browser('override', [flag])
            try:
                phase['data']['result'] = self.molt_net_call(
                    'moltnet.openWindow', timeout=self.opts.open_timeout)
            except (cdplib.CdpError, RuntimeError) as e:
                phase['data']['result'] = {'error': 'harness: %s' % e}
            time.sleep(3)
            phase['data']['moltnet_pages'] = len(self.moltnet_pages())
            test = catalog.by_id('regular_onion_subresources')
            entry = self.tests[test.id]
            sid = self.regular[1]
            self.cdp.navigate(sid, self.test_url(test, entry), timeout=60)
            res, _ = self.poll_results(sid, 60)
            phase['data']['subresources'] = res
            time.sleep(4)
            self.phase_end(phase)
        except (RuntimeError, cdplib.CdpError) as e:
            self.phase_end(phase, str(e))
        self.close_browser()

    def dsf_sessions(self):
        for dsf in self.opts.dsfs:
            if dsf == 1:
                continue
            phase = self.phase_begin('dsf_%s' % dsf)
            try:
                self.launch_browser('dsf%s' % dsf, dsf=dsf)
                if self.opts.target == 'moltnet' and not self.open_moltnet(
                        'open_moltnet_dsf_%s' % dsf):
                    raise RuntimeError('MoltNet did not open')
                sid = self.moltnet[1] if self.moltnet else self.regular[1]
                self.fingerprint(sid, 'moltnet' if self.moltnet else
                                 'regular', dsf, label_prefix='dsf-')
                self.phase_end(phase)
            except (RuntimeError, cdplib.CdpError) as e:
                self.phase_end(phase, str(e))
            self.close_browser()

    def control_tor(self):
        data = self.user_mkdir('control-tor', mode=0o700)
        self.user_write('control-tor/torrc', 'DataDirectory %s\nSocksPort '
                        '127.0.0.1:%d\nLog notice file %s/notice.log\n' % (
                            data, CONTROL_SOCKS, data))
        wrapper = self.sandbox.write_tor_wrapper(
            self.real_tor(), self.path('logs', 'tor-argv.txt'))
        self.start('control-tor', [wrapper, '-f', self.path('control-tor',
                                                            'torrc')],
                   cgroup='kit', ready=False)
        time.sleep(3)

    # ---- everything ---------------------------------------------------------

    def run(self):
        self.preflight()
        try:
            self.setup()
            if self.opts.control_tor:
                self.control_tor()
            self.main_session()
            # In a control run (--target regular) only when asked for: they
            # exercise the kit's own code, MoltNet won't open there.
            if self.opts.target == 'moltnet' or self.opts.phases:
                if self.want_phase('restart'):
                    self.restart_session()
                if self.want_phase('override'):
                    self.override_session()
            if self.want_phase('dsf'):
                self.dsf_sessions()
        except KeyboardInterrupt:
            self.error('interrupted')
        except (RuntimeError, OSError, cdplib.CdpError) as e:
            self.error(str(e))
        finally:
            try:
                self.close_browser()
            except Exception as e:  # pylint: disable=broad-except
                self.error('closing the browser: %s' % e)
            if self.sandbox:
                try:
                    self.user_write('logs/nft-counters.txt',
                                    self.sandbox.counters())
                except (OSError, RuntimeError):
                    pass
            self.stop_all()
            if self.sandbox:
                for err in self.sandbox.teardown():
                    self.error('teardown: ' + err)
            self.manifest['finished'] = _iso(_now())
            if os.path.isdir(self.dir):
                self.user_write('manifest.json', json.dumps(
                    self.manifest, indent=1, sort_keys=True, default=str))
                _chown_tree(self.dir, self.uid, self.gid)
        return self.dir


def _drain(stream, logf):
    try:
        for line in iter(stream.readline, b''):
            logf.write(line.decode('utf-8', 'replace'))
            logf.flush()
    except (OSError, ValueError):
        pass


def _high_fd(fd):
    import fcntl
    new = fcntl.fcntl(fd, fcntl.F_DUPFD, 10)
    return new


def _drop(uid, gid):
    def fn():
        os.setgid(gid)
        os.setuid(uid)
    return fn


def _chown_tree(top, uid, gid):
    for root, dirs, files in os.walk(top):
        for n in dirs + files:
            try:
                os.lchown(os.path.join(root, n), uid, gid)
            except OSError:
                pass
