# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The regular-window traffic capture (docs/LEAK_TESTING.md, "Regular-window
traffic capture"; `leaktest.sh traffic`).

Starts the browser with a fresh profile in the kit's network namespace and
walks it through a first run, an idle period, a scripted browse of the
kit's local pages, a restart and MoltBrowser's update check. Nothing
leaves the namespace: the kit's resolver gives every name an address of
its own in the sinkhole range, and the sinkhole on the namespace's
loopback records each connection (server name, port, HTTP request line)
and answers 503. bganalysis.py compares what was recorded with the
allow-list generated from docs/NETWORK_TRAFFIC.md (hostlist.py).

Runs as root, like `leaktest.sh run`; the browser and every server run as
the invoking user. Reuses the harness's namespace, captures, launcher and
DevTools pipe (harness.Run); only the servers and the steps differ.
"""

import json
import os
import subprocess
import sys
import time

from . import KIT_VERSION
from . import cdp as cdplib
from . import harness
from . import hostlist
from . import netns
from . import sinkhole
from . import support

SCHEMA = 'moltleak-traffic/1'
LOCAL_HTTP, LOCAL_HTTPS, LOCAL_EXPIRED = 18080, 18443, 18446
SINKHOLE_PORTS = (80, 443)

# (step, seconds it is held open after its action, ...): the default plan.
DEFAULT_TIMES = {'first_run': 45, 'idle': 600, 'restart': 120,
                 'update_check': 45, 'final': 20}


class TrafficOptions(harness.Options):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.target = 'traffic'
        self.extension = False
        self.seed_settings = False
        self.idle = kw.get('idle', DEFAULT_TIMES['idle'])
        self.first_run_wait = kw.get('first_run_wait',
                                     DEFAULT_TIMES['first_run'])
        self.restart_idle = kw.get('restart_idle', DEFAULT_TIMES['restart'])
        self.update_wait = kw.get('update_wait',
                                  DEFAULT_TIMES['update_check'])
        self.final_wait = kw.get('final_wait', DEFAULT_TIMES['final'])
        self.hosts = kw.get('hosts') or hostlist.JSON_PATH
        self.skip_browse = kw.get('skip_browse', False)


def make_expired_cert(out_dir):
    """expired.crt/key: a self-signed certificate for 127.0.0.1 and
    localhost that expired in 2020, with a key of its own (so that
    --ignore-certificate-errors-spki-list doesn't cover it): the browser
    shows a certificate-date error, as a real expired site would."""
    def p(name):
        return os.path.join(out_dir, name)
    with open(p('expired.cnf'), 'w') as f:
        f.write('[ca]\ndefault_ca = d\n[d]\ndir = %s\ndatabase = '
                '$dir/expired.index\nnew_certs_dir = $dir\nserial = '
                '$dir/expired.serial\ndefault_md = sha256\npolicy = p\n'
                'unique_subject = no\n[p]\ncommonName = supplied\n[ext]\n'
                'basicConstraints = critical,CA:FALSE\nsubjectAltName = '
                'IP:127.0.0.1,DNS:localhost\n' % out_dir)
    open(p('expired.index'), 'w').close()
    with open(p('expired.serial'), 'w') as f:
        f.write('01\n')
    support._openssl('req', '-newkey', 'ec', '-pkeyopt',
                     'ec_paramgen_curve:prime256v1', '-nodes', '-keyout',
                     p('expired.key'), '-out', p('expired.csr'), '-subj',
                     '/CN=moltleak expired')
    support._openssl('ca', '-batch', '-config', p('expired.cnf'),
                     '-selfsign', '-keyfile', p('expired.key'), '-in',
                     p('expired.csr'), '-out', p('expired.crt'),
                     '-startdate', '20200101000000Z', '-enddate',
                     '20200102000000Z', '-extensions', 'ext', '-notext')
    return p('expired.crt'), p('expired.key')


class TrafficRun(harness.Run):
    """One capture: its own namespace, resolver, sinkhole and pages."""

    def __init__(self, opts):
        super().__init__(opts)
        stamp = os.path.basename(self.dir).rsplit('-', 1)[0]
        self.dir = os.path.join(os.path.abspath(opts.out), '%s-traffic-%s' % (
            stamp, self.run_id))
        self.nonce = support.nonce()
        self.manifest = {
            'schema': SCHEMA, 'kit_version': KIT_VERSION,
            'run_id': self.run_id, 'target': 'traffic',
            'options': opts.as_dict(), 'phases': [], 'launches': [],
            'errors': [], 'tests': [],
            'captures': {'all': 'capture/all.pcap',
                         'blocked': 'capture/blocked.pcap',
                         'tor': 'capture/tor.pcap'},
            'logs': {'dns': 'logs/fakedns.jsonl',
                     'sinkhole': 'logs/sinkhole.jsonl',
                     'local_server': 'logs/local_server.jsonl'},
            'sinkhole': {'net': sinkhole.SINKHOLE_NET,
                         'ports': list(SINKHOLE_PORTS)},
            'local_ports': [LOCAL_HTTP, LOCAL_HTTPS, LOCAL_EXPIRED],
            'canary': {'domain': harness.CANARY_DOMAIN},
        }

    # ---- preflight and setup ------------------------------------------------

    def preflight(self):
        why = hostlist.check_files(json_path=self.opts.hosts) \
            if os.path.abspath(self.opts.hosts) == hostlist.JSON_PATH else None
        if why:
            raise SystemExit('the allow-list does not match the doc: ' + why)
        try:
            hl = hostlist.HostList.load(self.opts.hosts)
        except (IOError, OSError, ValueError) as e:
            raise SystemExit('cannot read the allow-list %s: %s' % (
                self.opts.hosts, e))
        self.manifest['hosts'] = {'path': os.path.abspath(self.opts.hosts),
                                  'entries': len(hl.entries)}
        super().preflight()

    def setup(self):
        os.makedirs(self.dir)
        os.chown(self.dir, self.uid, self.gid)
        for d in ('capture', 'logs', 'home', 'certs', 'xdg'):
            self.user_mkdir(d, mode=0o700 if d in ('home', 'xdg') else 0o755)
        self.manifest['t0'] = time.time()
        self.manifest['started'] = harness._iso(self.manifest['t0'])
        self.manifest['host'] = {'os': os.uname().sysname + ' ' +
                                 os.uname().release,
                                 'python': sys.version.split()[0]}
        self.manifest['browser'] = self.browser_version()
        self.user_write('resolv.conf', harness.RESOLV)
        self.user_write('nsswitch.conf', harness.NSSWITCH)
        self.sandbox = netns.Sandbox(self.run_id, self.uid, self.gid, self.dir,
                                     self.log)
        self.manifest['net'] = self.sandbox.setup(
            harness.LOOPBACK_CANARY, sinkhole=sinkhole.SINKHOLE_NET)
        self.log('namespace %s, sinkhole %s' % (self.sandbox.ns,
                                               sinkhole.SINKHOLE_NET))
        self.start_captures()
        certs = self.path('certs')
        self.spki = support.make_certs(certs, ['localhost'], ['127.0.0.1'], {
            'ca_issuers': ['http://aia.%s/i.crt' % harness.CANARY_DOMAIN],
            'ocsp': 'http://ocsp.%s/' % harness.CANARY_DOMAIN,
            'crl': 'http://crl.%s/c.crl' % harness.CANARY_DOMAIN})
        ecert, ekey = make_expired_cert(certs)
        for f in os.listdir(certs):
            os.chown(os.path.join(certs, f), self.uid, self.gid)
        cert, key = self.path('certs', 'server.crt'), self.path('certs',
                                                                'server.key')
        self.service('fakedns', 'fakedns', '--listen', '127.0.0.1', '--port',
                     '53', '--log', self.path('logs', 'fakedns.jsonl'))
        self.service('sinkhole', 'sinkhole', '--ports',
                     ','.join(str(p) for p in SINKHOLE_PORTS), '--cert', cert,
                     '--key', key, '--log', self.path('logs',
                                                      'sinkhole.jsonl'))
        values = {'BASE_HTTP': 'http://127.0.0.1:%d' % LOCAL_HTTP,
                  'BASE_HTTPS': 'https://127.0.0.1:%d' % LOCAL_HTTPS,
                  'BASE_B_HTTPS': 'https://localhost:%d' % LOCAL_HTTPS,
                  'BASE_LOCAL': 'http://127.0.0.1:%d' % LOCAL_HTTP,
                  'ONION_A': '', 'ONION_B': '', 'LPORT': '',
                  'GW': self.manifest['net']['gw_ipv4'],
                  'GW6': self.manifest['net']['gw_ipv6'],
                  'NSIP': self.manifest['net']['ns_ipv4']}
        self.user_write('pages.json', json.dumps({
            'pages_dir': harness.PAGES, 'canary': harness.CANARY_DOMAIN,
            'values': values, 'tests': {}}, indent=1))
        self.service('local-pages', 'pages', '--config',
                     self.path('pages.json'), '--log',
                     self.path('logs', 'local_server.jsonl'),
                     '--http', '127.0.0.1:%d' % LOCAL_HTTP,
                     '--https', '127.0.0.1:%d:%s:%s' % (LOCAL_HTTPS, cert,
                                                        key),
                     '--https', '127.0.0.1:%d:%s:%s' % (LOCAL_EXPIRED, ecert,
                                                        ekey))
        if self.use_display_xvfb():
            self.start_xvfb()

    # ---- steps --------------------------------------------------------------

    def step(self, name):
        return self.phase_begin(name, target='regular')

    def launch(self, label, first_run=False):
        """Starts the browser as a user would (harness natural launch): the
        first launch is the profile's first run."""
        self.manifest['launches'].append({'label': label, 'ts': time.time(),
                                          'first_run': first_run})
        client = self.launch_browser(label, natural=True)
        self.manifest['launches'][-1]['up'] = time.time()
        return client

    def page_url(self, page, port=LOCAL_HTTP, scheme='http'):
        return '%s://127.0.0.1:%d/t/%s/%s' % (scheme, port, self.nonce, page)

    def tabs(self):
        try:
            return [{'url': (t.get('url') or '')[:120],
                     'title': (t.get('title') or '')[:80]}
                    for t in self.cdp.targets() if t['type'] == 'page']
        except cdplib.CdpError:
            return []

    def insert_text(self, sid, selector, text):
        self.cdp.evaluate(sid, 'document.querySelector(%s).focus()' %
                          json.dumps(selector), user_gesture=True)
        self.cdp.call('Input.insertText', {'text': text}, session=sid)

    def browse_step(self, name, action, settle):
        phase = self.step(name)
        try:
            phase['data'].update(action(self.regular[1]) or {})
            time.sleep(settle)
            phase['data']['title'] = self.cdp.evaluate(
                self.regular[1], 'document.title', timeout=10)
            self.phase_end(phase)
        except cdplib.CdpClosed:
            self.phase_end(phase, 'the browser closed')
            raise
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def _home(self, sid):
        return {'nav_error': self.cdp.navigate(sid, 'chrome://newtab/',
                                               timeout=30)}

    def _form(self, sid):
        data = {'nav_error': self.cdp.navigate(sid, self.page_url(
            'bg_form.html'), timeout=30)}
        time.sleep(2)
        self.insert_text(sid, '#name', 'Molt Leaktest')
        self.insert_text(sid, '#street', '1 Example Street')
        self.insert_text(sid, '#city', 'Springfield')
        self.insert_text(sid, '#zip', '12345')
        self.insert_text(sid, '#user', 'molt.leaktest@example.invalid')
        self.insert_text(sid, '#pass', 'not-a-real-password-26')
        time.sleep(1)
        self.cdp.evaluate(sid, "document.querySelector('#signin')"
                          ".requestSubmit()", user_gesture=True)
        return data

    def _translate(self, sid):
        return {'nav_error': self.cdp.navigate(sid, self.page_url(
            'bg_fr.html'), timeout=30)}

    def _spellcheck(self, sid):
        data = {'nav_error': self.cdp.navigate(sid, self.page_url(
            'bg_spell.html'), timeout=30)}
        self.insert_text(sid, '#text', 'Thiss sentense is mispeled on '
                         'purpos, so that the speling checker wakes up. ')
        return data

    def _download(self, sid):
        # The browser's own download handling (into $HOME/Downloads, a
        # directory of the run): DevTools' Browser.setDownloadBehavior would
        # replace part of it, Safe Browsing's check perhaps included.
        return {'nav_error': self.cdp.navigate(sid, self.page_url(
            'download.html'), timeout=30)}

    def _dns_error(self, sid):
        return {'nav_error': self.cdp.navigate(
            sid, 'http://nx-%s.invalid/' % self.nonce, timeout=30)}

    def _cert_error(self, sid):
        return {'nav_error': self.cdp.navigate(sid, self.page_url(
            'basic.html', LOCAL_EXPIRED, 'https'), timeout=30)}

    BROWSE = (('home', '_home', 8), ('form', '_form', 10),
              ('translate', '_translate', 12), ('spellcheck', '_spellcheck',
                                                12),
              ('download', '_download', 15), ('dns_error', '_dns_error', 12),
              ('cert_error', '_cert_error', 15))

    def update_check(self):
        phase = self.step('update_check')
        try:
            try:
                tid = self.cdp.call('Target.createTarget', {
                    'url': 'chrome://molt-ai-update/'})['targetId']
                sid = self.cdp.attach(tid)
            except cdplib.CdpClosed:
                raise
            except cdplib.CdpError:
                # Some builds refuse to create a target at an internal URL
                # (harness.open_molt_net_ui does the same).
                tid = self.cdp.call('Target.createTarget',
                                    {'url': 'about:blank'})['targetId']
                sid = self.cdp.attach(tid)
                phase['data']['nav_error'] = self.cdp.navigate(
                    sid, 'chrome://molt-ai-update/', timeout=20)
            # The page asks for a check as it loads ("updateCheck").
            time.sleep(self.opts.update_wait)
            try:
                text = self.cdp.evaluate(sid, 'document.body ? '
                                         'document.body.innerText : ""',
                                         timeout=10)
            except cdplib.CdpError as e:
                text = 'error: %s' % e
            phase['data']['page_text'] = str(text or '')[:400]
            self.phase_end(phase)
        except cdplib.CdpClosed:
            self.phase_end(phase, 'the browser closed')
            raise
        except cdplib.CdpError as e:
            self.phase_end(phase, str(e))

    def session(self):
        phase = self.step('first_run')
        self.launch('first-run', first_run=True)
        time.sleep(self.opts.first_run_wait)
        phase['data']['tabs'] = self.tabs()
        self.phase_end(phase)

        phase = self.step('idle')
        time.sleep(self.opts.idle)
        phase['data']['tabs'] = self.tabs()
        self.phase_end(phase)

        if self.opts.skip_browse:
            for name, _, _ in self.BROWSE:
                self.skip_phase(name, '--skip-browse')
        else:
            for name, method, settle in self.BROWSE:
                self.browse_step(name, getattr(self, method), settle)

        phase = self.step('shutdown')
        self.close_browser()
        self.phase_end(phase)

        phase = self.step('restart')
        self.launch('restart')
        time.sleep(self.opts.restart_idle)
        phase['data']['tabs'] = self.tabs()
        self.phase_end(phase)

        self.update_check()

        phase = self.step('final')
        time.sleep(self.opts.final_wait)
        self.close_browser()
        self.phase_end(phase)

    def run(self):
        self.preflight()
        try:
            self.setup()
            self.session()
        except KeyboardInterrupt:
            self.error('interrupted')
        except (RuntimeError, OSError, cdplib.CdpError) as e:
            self.error(str(e))
        finally:
            for phase in self.manifest['phases']:
                if 'start' in phase and 'end' not in phase:
                    self.phase_end(phase, 'did not finish')
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
            self.manifest['finished'] = harness._iso(time.time())
            if os.path.isdir(self.dir):
                self.user_write('manifest.json', json.dumps(
                    self.manifest, indent=1, sort_keys=True, default=str))
                harness._chown_tree(self.dir, self.uid, self.gid)
        return self.dir


def report_command(run_dir, uid=None, gid=None):
    """argv that rebuilds the report as the invoking user."""
    cmd = [sys.executable, '-I', harness.ENTRY, 'report', run_dir]
    if uid is not None and os.geteuid() == 0:
        cmd = ['setpriv', '--reuid=%d' % uid, '--regid=%d' % gid,
               '--init-groups', '--'] + cmd
    return cmd


def run(opts):
    tr = TrafficRun(opts)
    run_dir = tr.run()
    print('run directory: %s' % run_dir)
    return subprocess.call(report_command(run_dir, tr.uid, tr.gid))
