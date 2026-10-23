#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""MoltNet leak and uniformity test kit (docs/LEAK_TESTING.md).

  sudo tools/leak_test/leaktest.sh run --browser chromium/src/out/MoltBrowser-linux/chrome
      runs everything and writes <out>/<run>/report.md + report.json
  tools/leak_test/leaktest.sh report <run dir>
      (re)builds the report from a recorded run, no root needed
  tools/leak_test/leaktest.sh diff <run A>/report.json <run B>/report.json
      shows whether two runs (two machines, two OS locales) look the same
  tools/leak_test/leaktest.sh service ...
      the kit's own servers (started by `run`)

Run it with `python3 -I`; it only imports its own directory and the
standard library.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from leakkit import report as reportlib  # noqa: E402


def _size(text):
    w, _, h = text.lower().partition('x')
    return int(w), int(h)


def _list(text, conv=str):
    return [conv(x) for x in text.split(',') if x.strip()] if text else []


def cmd_run(args):
    from leakkit import harness
    opts = harness.Options(
        browser=os.path.abspath(args.browser), out=args.out,
        target=args.target, display=args.display, screen=_size(args.screen),
        window_sizes=[_size(s) for s in _list(args.window_sizes)] or None,
        zooms=_list(args.zooms, int) if args.zooms is not None else None,
        dsfs=_list(args.dsf, float) or None, locale=args.locale,
        timezone=args.timezone, tests=_list(args.tests) or None,
        phases=_list(args.phases) or None, browser_args=args.browser_arg,
        tor=args.tor, offline=args.offline, control_tor=args.control_tor,
        user=args.user, final_wait=args.final_wait,
        onion_timeout=args.onion_timeout,
        extension=not args.no_extension,
        seed_settings=not args.no_seed_settings)
    run = harness.Run(opts)
    run_dir = run.run()
    print('run directory: %s' % run_dir)
    # The report is built from the recorded files, as the invoking user.
    cmd = [sys.executable, '-I', os.path.abspath(__file__), 'report', run_dir]
    if os.geteuid() == 0:
        cmd = ['setpriv', '--reuid=%d' % run.uid, '--regid=%d' % run.gid,
               '--init-groups', '--'] + cmd
    return subprocess.call(cmd)


def cmd_report(args):
    rep = reportlib.write(args.run_dir)
    print('verdict: %s  (%s)' % (rep['verdict'], ', '.join(
        '%d %s' % (v, k) for k, v in sorted(rep['counts'].items()))))
    for c in rep['checks']:
        if c['status'] in ('FAIL', 'ERROR'):
            print('  %-5s %s' % (c['status'], c['title']))
    print('report: %s' % os.path.join(args.run_dir, 'report.md'))
    print('json:   %s' % os.path.join(args.run_dir, 'report.json'))
    return 0 if rep['verdict'] == 'PASS' else 1


def cmd_diff(args):
    verdict, text, data = reportlib.diff_reports(args.a, args.b)
    sys.stdout.write(text)
    if args.json:
        import json
        with open(args.json, 'w') as f:
            json.dump(data, f, indent=1, sort_keys=True)
    return 0 if verdict == 'PASS' else 1


def _hostport(text):
    host, _, port = text.rpartition(':')
    return host.strip('[]'), int(port)


def cmd_service(args):
    from leakkit import services
    if args.kind == 'pages':
        listens = []
        for spec in args.http or []:
            host, port = _hostport(spec)
            listens.append(('http', host, port, None, None))
        for spec in args.https or []:
            host, port, cert, key = spec.split(':', 3)
            listens.append(('https', host, int(port), cert, key))
        services.run_pages(args.config, args.log, listens)
    elif args.kind == 'dns':
        services.run_dns(args.listen, args.port, args.log)
    elif args.kind == 'loopback':
        services.run_loopback(args.port, args.log)
    elif args.kind == 'proxy':
        services.run_proxy(args.listen, args.port, args.log)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = p.add_subparsers(dest='cmd')

    r = sub.add_parser('run', help='run the suite against a built browser '
                       '(root)')
    r.add_argument('--browser', required=True,
                   help='the browser executable (e.g. out/MoltBrowser-linux/chrome '
                   'or /opt/moltbrowser/moltbrowser)')
    r.add_argument('--out', default=None,
                   help='where run directories go (default ./moltleak-runs)')
    r.add_argument('--target', choices=('moltnet', 'regular'),
                   default='moltnet',
                   help='moltnet (default): open a MoltNet window and test '
                   'it; regular: test a plain window (harness check, or a '
                   'control with any Chromium)')
    r.add_argument('--display', choices=('auto', 'xvfb', 'current',
                                         'headless'), default='auto')
    r.add_argument('--screen', default='1920x1080',
                   help='Xvfb screen size (default 1920x1080)')
    r.add_argument('--window-sizes', default=None,
                   help='comma-separated WxH (default 1920x1080,1366x768,'
                   '1280x800,1024x768,800x600)')
    r.add_argument('--zooms', default=None,
                   help='zoom levels in %% (default 50,90,125,200; empty to '
                   'skip)')
    r.add_argument('--dsf', default=None,
                   help='device scale factors, one launch each (default '
                   '1,2)')
    r.add_argument('--locale', default=None,
                   help='LANG/LC_ALL for the browser, e.g. de_DE.UTF-8')
    r.add_argument('--timezone', default=None,
                   help='TZ for the browser, e.g. Asia/Tokyo')
    r.add_argument('--tests', default=None,
                   help='comma-separated test ids (default all)')
    r.add_argument('--phases', default=None,
                   help='comma-separated phases: identity,new_identity,'
                   'fail_closed,restart,override,partitioning_override,dsf '
                   '(default all)')
    r.add_argument('--browser-arg', action='append', default=[],
                   help='extra browser flag (repeatable)')
    r.add_argument('--tor', default=None,
                   help='the tor MoltNet should run (default: the bundled '
                   'one, else /usr/bin/tor or /usr/sbin/tor)')
    r.add_argument('--offline', action='store_true',
                   help='skip the checks that need the public internet '
                   'through a Tor exit (check.torproject.org)')
    r.add_argument('--control-tor', action='store_true',
                   help='with --target regular: run a system tor in the '
                   'Tor cgroup and point the browser at it with '
                   '--proxy-server (harness self-test)')
    r.add_argument('--user', default=None,
                   help='who runs the browser (default: $SUDO_USER)')
    r.add_argument('--final-wait', type=int, default=20,
                   help='seconds to keep capturing before the browser '
                   'closes (late reports)')
    r.add_argument('--onion-timeout', type=int, default=300,
                   help='seconds to wait for the kit\'s onion service to be '
                   'reachable through Tor')
    r.add_argument('--no-extension', action='store_true')
    r.add_argument('--no-seed-settings', action='store_true')
    r.set_defaults(fn=cmd_run)

    rep = sub.add_parser('report', help='build report.md/report.json from a '
                         'run directory')
    rep.add_argument('run_dir')
    rep.set_defaults(fn=cmd_report)

    d = sub.add_parser('diff', help='compare the MoltNet fingerprints of two '
                       'runs')
    d.add_argument('a')
    d.add_argument('b')
    d.add_argument('--json', default=None, help='also write the diff as JSON')
    d.set_defaults(fn=cmd_diff)

    s = sub.add_parser('service', help='(internal) the kit\'s servers, '
                       'started by run')
    s.add_argument('kind', choices=('pages', 'dns', 'loopback', 'proxy'))
    s.add_argument('--config')
    s.add_argument('--log')
    s.add_argument('--listen', default='127.0.0.1')
    s.add_argument('--port', type=int, default=0)
    s.add_argument('--http', action='append')
    s.add_argument('--https', action='append')
    s.set_defaults(fn=cmd_service)

    args = p.parse_args(argv)
    if not getattr(args, 'fn', None):
        p.print_help()
        return 2
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
