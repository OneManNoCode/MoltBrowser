# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The test network: a namespace where only Tor's traffic may leave.

    host namespace                          moltleak-<id> namespace
    --------------                          -----------------------
    <uplink> <- masquerade <- mlk<id>h ==== mlk<id>n  10.231.X.2/30
                                                       fd6d:6c6b:X::2/64
                              nftables (inet moltleak), output hook:
                                1. sockets of the run's "tor" cgroup:
                                   accept (first packet -> nflog 2)
                                2. DNS/mDNS/LLMNR/DoT from anything else:
                                   nflog 1; only the kit's resolver on
                                   127.0.0.1:53 answers, the rest is dropped
                                3. the loopback canary port (on loopback):
                                   nflog 1
                                4. other loopback traffic (browser <-> Tor's
                                   SOCKS port, the kit's local servers): accept
                                5. replies of accepted flows: accept
                                6. IGMP/MLD reports: nflog 1 (they show a
                                   socket joined e.g. the mDNS group)
                                7. everything else: nflog 1, drop

Tor is told apart from the browser by cgroup (nftables `socket cgroupv2`),
not by address: MoltBrowser starts Tor itself, as the same user, and the
kit only points --moltnet-tor-binary at a root-owned wrapper that moves
itself into the "tor" cgroup and execs the real tor. Its pluggable
transports are its children and stay in that cgroup.

Everything here needs root and is undone by teardown(), also after a crash
of the run (each step is recorded in the order it was made).
"""

import errno
import os
import shutil
import signal
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCHER = os.path.join(HERE, 'launch_in_ns.sh')

NFLOG_BLOCKED = 1
NFLOG_TOR = 2


def cgroup2_mount(mountinfo='/proc/self/mountinfo'):
    """Mount point of the cgroup v2 hierarchy, or None."""
    try:
        with open(mountinfo, 'r') as f:
            for line in f:
                fields = line.split()
                if ' - ' not in line:
                    continue
                post = line.split(' - ', 1)[1].split()
                if post and post[0] == 'cgroup2':
                    return fields[4]
    except (IOError, OSError):
        return None
    return None


def netns_ruleset(cgroup_rel, cgroup_level, loopback_port):
    """The namespace's nftables ruleset. `cgroup_rel` is the run's "tor"
    cgroup relative to /sys/fs/cgroup (nft resolves it there)."""
    if "'" in cgroup_rel or '"' in cgroup_rel:
        raise ValueError('bad cgroup path')
    return '''table inet moltleak {
  chain output {
    type filter hook output priority filter; policy drop;
    socket cgroupv2 level %(level)d "%(cg)s" ct state new log group %(tor)d prefix "tor" counter accept
    socket cgroupv2 level %(level)d "%(cg)s" counter accept
    ip daddr 127.0.0.1 meta l4proto { tcp, udp } th dport 53 log group %(blk)d prefix "dns-kit" counter accept
    meta l4proto { tcp, udp } th dport { 53, 853, 5353, 5355 } log group %(blk)d prefix "dns" counter drop
    oif "lo" tcp dport %(lport)d ct state new log group %(blk)d prefix "lo-canary" counter accept
    oif "lo" counter accept
    ct state established,related counter accept
    ip protocol igmp log group %(blk)d prefix "igmp" counter accept
    icmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert } counter accept
    icmpv6 type { mld-listener-report, mld2-listener-report, mld-listener-done } log group %(blk)d prefix "mld" counter accept
    log group %(blk)d prefix "drop" counter drop
  }
}
''' % {'level': cgroup_level, 'cg': cgroup_rel, 'tor': NFLOG_TOR,
       'blk': NFLOG_BLOCKED, 'lport': int(loopback_port)}


def host_ruleset(table, veth_host, subnet):
    return '''table ip %(t)s {
  chain forward {
    type filter hook forward priority filter; policy accept;
    iifname "%(v)s" counter accept
    oifname "%(v)s" ct state established,related counter accept
    oifname "%(v)s" counter drop
  }
  chain postrouting {
    type nat hook postrouting priority srcnat; policy accept;
    ip saddr %(s)s oifname != "%(v)s" counter masquerade
  }
}
''' % {'t': table, 'v': veth_host, 's': subnet}


def tor_wrapper_script(procs_path, real_tor, argv_log):
    for p in (procs_path, real_tor, argv_log):
        if "'" in p or '\n' in p:
            raise ValueError('unsafe path: %r' % p)
    return '''#!/bin/sh
# Written by tools/leak_test for one run and removed afterwards.
# MoltBrowser runs this as its Tor (--moltnet-tor-binary): it joins the
# run's "tor" cgroup, whose traffic is the only traffic the test namespace
# lets out, records its arguments, and becomes the real tor.
echo $$ > '%s' || exit 70
printf '%%s\\n' "$@" > '%s' 2>/dev/null
exec '%s' "$@"
''' % (procs_path, argv_log, real_tor)


def pick_index(run_id, used_subnets):
    """A 10.231.X.0/30 subnet not already in use."""
    start = int(run_id[:4], 16) % 250 + 1
    for i in range(250):
        x = (start + i) % 250 + 1
        if '10.231.%d.' % x not in used_subnets:
            return x
    raise RuntimeError('no free 10.231.X.0/30 subnet')


class Sandbox(object):
    def __init__(self, run_id, uid, gid, run_dir, log):
        self.run_id = run_id
        self.uid = uid
        self.gid = gid
        self.run_dir = run_dir
        self.log = log
        self.ns = 'moltleak-' + run_id
        self.veth_host = 'mlk%sh' % run_id
        self.veth_ns = 'mlk%sn' % run_id
        self.host_table = 'moltleak_' + run_id
        self.private_dir = '/run/moltleak-' + run_id
        self.cg_mount = cgroup2_mount()
        self.cg_root = None
        self.undo = []
        self.net = {}
        self.ip_forward_was = None

    # ---- helpers ------------------------------------------------------------

    def sh(self, *argv, **kw):
        check = kw.pop('check', True)
        inp = kw.pop('input', None)
        proc = subprocess.run(list(argv), input=inp, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              universal_newlines=True)
        if check and proc.returncode != 0:
            raise RuntimeError('%s failed: %s' % (' '.join(argv),
                                                  proc.stderr.strip()))
        return proc.stdout

    def in_ns(self, *argv, **kw):
        return self.sh('nsenter', '--net=/run/netns/' + self.ns, '--', *argv,
                       **kw)

    def cgroup(self, name):
        return os.path.join(self.cg_root, name)

    def procs(self, name):
        return os.path.join(self.cg_root, name, 'cgroup.procs')

    # ---- setup --------------------------------------------------------------

    def setup(self, loopback_port):
        if not self.cg_mount:
            raise RuntimeError('cgroup v2 is not mounted; the kit tells Tor '
                               'apart by cgroup (see docs/LEAK_TESTING.md)')
        if not self.cg_mount.startswith('/sys/fs/cgroup'):
            raise RuntimeError('cgroup v2 is mounted at %s; nftables looks '
                               'under /sys/fs/cgroup' % self.cg_mount)
        used = self.sh('ip', '-4', 'addr') + self.sh('ip', '-4', 'route')
        x = pick_index(self.run_id, used)
        self.net = {
            'index': x,
            'subnet': '10.231.%d.0/30' % x,
            'gw_ipv4': '10.231.%d.1' % x, 'ns_ipv4': '10.231.%d.2' % x,
            'gw_ipv6': 'fd6d:6c6b:%x::1' % x, 'ns_ipv6': 'fd6d:6c6b:%x::2' % x,
            'veth_host': self.veth_host, 'veth_ns': self.veth_ns,
            'netns': self.ns,
        }
        # cgroups
        self.cg_root = os.path.join(self.cg_mount, 'moltleak-' + self.run_id)
        os.mkdir(self.cg_root)
        self.undo.append(('cgroup', None))
        for name in ('browser', 'tor', 'kit'):
            os.mkdir(self.cgroup(name))
        for path in [self.cg_root, os.path.join(self.cg_root, 'cgroup.procs')
                     ] + [self.cgroup(n) for n in ('browser', 'tor', 'kit')] \
                + [self.procs(n) for n in ('browser', 'tor', 'kit')]:
            os.chown(path, self.uid, self.gid)
        # namespace and link
        self.sh('ip', 'netns', 'add', self.ns)
        self.undo.append(('netns', None))
        self.sh('ip', 'link', 'add', self.veth_host, 'type', 'veth', 'peer',
                'name', self.veth_ns)
        self.undo.append(('link', None))
        self.sh('ip', 'link', 'set', self.veth_ns, 'netns', self.ns)
        self.sh('ip', 'addr', 'add', self.net['gw_ipv4'] + '/30', 'dev',
                self.veth_host)
        self.sh('ip', '-6', 'addr', 'add', self.net['gw_ipv6'] + '/64', 'dev',
                self.veth_host, 'nodad', check=False)
        self.sh('ip', 'link', 'set', self.veth_host, 'up')
        for knob, value in (('net/ipv4/ip_unprivileged_port_start', '0'),
                            ('net/ipv6/conf/all/accept_ra', '0'),
                            ('net/ipv6/conf/default/accept_ra', '0')):
            self.in_ns('sh', '-c', 'echo %s > /proc/sys/%s' % (value, knob),
                       check=False)
        self.in_ns('ip', 'link', 'set', 'lo', 'up')
        self.in_ns('ip', 'addr', 'add', self.net['ns_ipv4'] + '/30', 'dev',
                   self.veth_ns)
        self.in_ns('ip', '-6', 'addr', 'add', self.net['ns_ipv6'] + '/64',
                   'dev', self.veth_ns, 'nodad', check=False)
        self.in_ns('ip', 'link', 'set', self.veth_ns, 'up')
        self.in_ns('ip', 'route', 'add', 'default', 'via', self.net['gw_ipv4'])
        self.in_ns('ip', '-6', 'route', 'add', 'default', 'via',
                   self.net['gw_ipv6'], check=False)
        self.net['ns_ifindex'] = self.in_ns(
            'cat', '/sys/class/net/%s/ifindex' % self.veth_ns,
            check=False).strip()
        # firewall inside the namespace
        rel = os.path.relpath(self.cgroup('tor'), '/sys/fs/cgroup')
        level = len(os.path.relpath(self.cgroup('tor'),
                                    self.cg_mount).split(os.sep))
        self.in_ns('nft', '-f', '-', input=netns_ruleset(rel, level,
                                                         loopback_port))
        # forwarding + NAT on the host for Tor's traffic
        with open('/proc/sys/net/ipv4/ip_forward', 'r') as f:
            self.ip_forward_was = f.read().strip()
        if self.ip_forward_was != '1':
            with open('/proc/sys/net/ipv4/ip_forward', 'w') as f:
                f.write('1')
            self.undo.append(('ip_forward', self.ip_forward_was))
        self.sh('nft', '-f', '-', input=host_ruleset(
            self.host_table, self.veth_host, self.net['subnet']))
        self.undo.append(('host_table', None))
        self._iptables_compat()
        # root-owned directory for the Tor wrapper (MoltBrowser refuses a
        # --moltnet-tor-binary in a directory others can write, like /tmp)
        os.mkdir(self.private_dir, 0o755)
        os.chmod(self.private_dir, 0o755)
        self.undo.append(('private_dir', None))
        return self.net

    def _iptables_compat(self):
        """Docker and ufw set the iptables FORWARD policy to DROP, which
        would drop Tor's forwarded packets whatever our table says."""
        if not shutil.which('iptables'):
            return
        out = self.sh('iptables', '-S', 'FORWARD', check=False)
        if '-P FORWARD DROP' not in out and 'DOCKER-USER' not in out:
            return
        chain = 'DOCKER-USER' if '-N DOCKER-USER' in self.sh(
            'iptables', '-S', check=False) else 'FORWARD'
        rules = [['-i', self.veth_host, '-j', 'ACCEPT'],
                 ['-o', self.veth_host, '-m', 'conntrack', '--ctstate',
                  'RELATED,ESTABLISHED', '-j', 'ACCEPT']]
        for rule in rules:
            self.sh('iptables', '-I', chain, '1', *rule)
            self.undo.append(('iptables', [chain] + rule))

    def write_tor_wrapper(self, real_tor, argv_log):
        path = os.path.join(self.private_dir, 'tor-wrapper')
        with open(path, 'w') as f:
            f.write(tor_wrapper_script(self.procs('tor'), real_tor, argv_log))
        os.chmod(path, 0o755)
        return path

    # ---- processes ----------------------------------------------------------

    def launch(self, argv, cgroup, env, stdout=None, stderr=None,
               resolv=None, nsswitch=None, pipe_fds=None):
        """Starts `argv` as the invoking user in the namespace, in `cgroup`,
        with exactly `env`. pipe_fds=(in, out) become fds 3 and 4."""
        cmd = ['nsenter', '--net=/run/netns/' + self.ns, '--',
               '/bin/bash', LAUNCHER, self.procs(cgroup), resolv or '-',
               nsswitch or '-', str(self.uid), str(self.gid)]
        pass_fds = ()
        if pipe_fds:
            cmd += [str(pipe_fds[0]), str(pipe_fds[1])]
            pass_fds = tuple(pipe_fds)
        else:
            cmd += ['-', '-']
        cmd += ['--', 'env', '-i'] + ['%s=%s' % kv for kv in sorted(
            env.items())] + list(argv)
        return subprocess.Popen(cmd, stdout=stdout, stderr=stderr,
                                stdin=subprocess.DEVNULL, pass_fds=pass_fds,
                                close_fds=True, start_new_session=True)

    def pids(self, cgroup):
        try:
            with open(self.procs(cgroup), 'r') as f:
                return [int(x) for x in f.read().split()]
        except (IOError, OSError, ValueError):
            return []

    def kill_cgroup(self, cgroup, sig=signal.SIGKILL):
        killed = []
        kill_file = os.path.join(self.cgroup(cgroup), 'cgroup.kill')
        if sig == signal.SIGKILL and os.path.exists(kill_file):
            killed = self.pids(cgroup)
            try:
                with open(kill_file, 'w') as f:
                    f.write('1')
                return killed
            except (IOError, OSError):
                pass
        for pid in self.pids(cgroup):
            try:
                os.kill(pid, sig)
                killed.append(pid)
            except OSError as e:
                if e.errno != errno.ESRCH:
                    raise
        return killed

    # ---- teardown -----------------------------------------------------------

    def teardown(self):
        errors = []
        if self.cg_root and os.path.isdir(self.cg_root):
            for name in ('browser', 'tor', 'kit'):
                try:
                    self.kill_cgroup(name)
                except OSError as e:
                    errors.append('kill %s: %s' % (name, e))
            deadline = time.time() + 10
            while time.time() < deadline and any(
                    self.pids(n) for n in ('browser', 'tor', 'kit')):
                time.sleep(0.2)
        for kind, arg in reversed(self.undo):
            try:
                if kind == 'cgroup':
                    for name in ('browser', 'tor', 'kit'):
                        if os.path.isdir(self.cgroup(name)):
                            os.rmdir(self.cgroup(name))
                    os.rmdir(self.cg_root)
                elif kind == 'netns':
                    self.sh('ip', 'netns', 'del', self.ns, check=False)
                elif kind == 'link':
                    self.sh('ip', 'link', 'del', self.veth_host, check=False)
                elif kind == 'ip_forward':
                    with open('/proc/sys/net/ipv4/ip_forward', 'w') as f:
                        f.write(arg)
                elif kind == 'host_table':
                    self.sh('nft', 'delete', 'table', 'ip', self.host_table,
                            check=False)
                elif kind == 'iptables':
                    self.sh('iptables', '-D', *arg, check=False)
                elif kind == 'private_dir':
                    shutil.rmtree(self.private_dir, ignore_errors=True)
            except (OSError, RuntimeError) as e:
                errors.append('%s: %s' % (kind, e))
        self.undo = []
        return errors

    def counters(self):
        return self.in_ns('nft', 'list', 'table', 'inet', 'moltleak',
                          check=False)
