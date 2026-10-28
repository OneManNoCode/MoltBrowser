#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for leakkit/rundir.py: what the harness writes into a run directory
its user owns never reaches a file outside it, whatever the user's
processes put there (links, hard links, FIFOs, a directory swapped for a
link), and the harness's own writes go through it.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import errno
import json
import os
import shutil
import stat
import tempfile
import unittest

from leakkit import harness
from leakkit import rundir
from leakkit import support

# As root the tests hand files to another user, as the harness does; as
# anyone else, to themselves (the owner checks then only see the mode).
if os.geteuid() == 0:
    UID, GID = 54321, 54321
else:
    UID, GID = os.getuid(), os.getgid()


class RunDirTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # A file and a directory outside the run directory, which nothing
        # may change.
        self.outside = os.path.join(self.tmp, 'outside')
        os.mkdir(self.outside, 0o700)
        self.secret = os.path.join(self.outside, 'secret')
        with open(self.secret, 'w') as f:
            f.write('root only\n')
        os.chmod(self.secret, 0o600)
        self.before = self.state()
        self.path = os.path.join(self.tmp, 'out', 'run-1')
        self.run = rundir.RunDir.create(self.path, UID, GID)
        self.addCleanup(self.run.close)

    def state(self):
        out = {}
        for name in sorted(os.listdir(self.outside)):
            p = os.path.join(self.outside, name)
            st = os.lstat(p)
            with open(p, 'rb') as f:
                out[name] = (f.read(), stat.S_IMODE(st.st_mode), st.st_uid,
                             st.st_gid, st.st_nlink)
        st = os.lstat(self.outside)
        out['.'] = (stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid)
        return out

    def assertOutsideUntouched(self):
        after = self.state()
        # A hard link the test made counts one more link on the file.
        for name, value in after.items():
            if name != '.' and name in self.before:
                after[name] = value[:4] + (self.before[name][4],)
        self.assertEqual(after, self.before)

    def p(self, *parts):
        return os.path.join(self.path, *parts)

    # ---- the run directory ----------------------------------------------

    def test_create_hands_the_directory_to_the_user(self):
        st = os.stat(self.path)
        self.assertEqual((st.st_uid, st.st_gid), (UID, GID))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o755)
        with self.assertRaises(FileExistsError):
            rundir.RunDir.create(self.path, UID, GID)

    def test_create_refuses_a_linked_out_directory(self):
        real = os.path.join(self.tmp, 'real')
        os.mkdir(real, 0o755)
        linked = os.path.join(self.tmp, 'linked')
        os.symlink(real, linked)
        with self.assertRaises(OSError):
            rundir.RunDir.create(os.path.join(linked, 'run-2'), UID, GID)
        self.assertEqual(os.listdir(real), [])

    def test_create_refuses_an_out_directory_anyone_can_write(self):
        open_dir = os.path.join(self.tmp, 'open')
        os.mkdir(open_dir)
        os.chmod(open_dir, 0o777)
        with self.assertRaises(OSError) as cm:
            rundir.RunDir.create(os.path.join(open_dir, 'run-2'), UID, GID)
        self.assertEqual(cm.exception.errno, errno.EPERM)
        self.assertEqual(os.listdir(open_dir), [])
        # A sticky one, as /tmp is, is fine.
        os.chmod(open_dir, 0o1777)
        run = rundir.RunDir.create(os.path.join(open_dir, 'run-3'), UID, GID)
        run.close()
        self.assertEqual(os.listdir(open_dir), ['run-3'])

    @unittest.skipUnless(os.geteuid() == 0, 'needs root to give a directory '
                         'to another account')
    def test_create_refuses_another_accounts_out_directory(self):
        theirs = os.path.join(self.tmp, 'theirs')
        os.mkdir(theirs, 0o755)
        os.chown(theirs, UID + 1, GID + 1)
        with self.assertRaises(OSError) as cm:
            rundir.RunDir.create(os.path.join(theirs, 'run-2'), UID, GID)
        self.assertEqual(cm.exception.errno, errno.EPERM)
        self.assertEqual(os.listdir(theirs), [])

    def test_check_out_dir(self):
        me = os.geteuid()
        # The user's group is a private one (UserPrivateGroups below).
        upg = UserPrivateGroups()
        ok = [st(0, 0, 0o755), st(me, 0, 0o755), st(UID, GID, 0o755),
              st(UID, GID, 0o775), st(0, 0, 0o1777), st(0, 0, 0o775)]
        for s in ok:
            rundir.check_out_dir(s, 'x', UID, GID, upg)
        bad = [st(UID + 7, GID, 0o755), st(UID, GID, 0o777),
               st(UID, GID + 7, 0o775)]
        for s in bad:
            if s.st_uid == me:
                continue
            with self.assertRaises(OSError):
                rundir.check_out_dir(s, 'x', UID, GID, upg)

    def test_create_refuses_a_group_writable_out_directory_of_a_shared_group(
            self):
        # Every user's primary group is `users`: another account in it could
        # rename the run directory and put another in its place.
        shared = os.path.join(self.tmp, 'shared')
        os.mkdir(shared)
        os.chmod(shared, 0o775)
        gid = os.stat(shared).st_gid
        with self.assertRaises(OSError) as cm:
            rundir.RunDir.create(os.path.join(shared, 'run-2'), UID, gid,
                                 SharedUsersGroup(gid))
        self.assertEqual(cm.exception.errno, errno.EPERM)
        self.assertIn('alice', str(cm.exception))
        self.assertIn('chmod g-w', str(cm.exception))
        self.assertEqual(os.listdir(shared), [])
        # Writable by its owner only, or sticky, it is fine.
        for mode in (0o755, 0o1775):
            os.chmod(shared, mode)
            run = rundir.RunDir.create(os.path.join(shared, 'run-%o' % mode),
                                       UID, gid, SharedUsersGroup(gid))
            run.close()
        self.assertEqual(sorted(os.listdir(shared)), ['run-1775', 'run-755'])

    def test_create_takes_a_group_writable_out_directory_of_a_private_group(
            self):
        private = os.path.join(self.tmp, 'private')
        os.mkdir(private)
        os.chmod(private, 0o775)
        gid = os.stat(private).st_gid
        run = rundir.RunDir.create(os.path.join(private, 'run-2'), UID, gid,
                                   UserPrivateGroups(gid))
        run.close()
        self.assertEqual(os.listdir(private), ['run-2'])

    def test_writes_follow_the_directory_not_its_path(self):
        # The user renames the run directory and puts a link to another
        # directory in its place: writes still land in the real one.
        moved = os.path.join(self.tmp, 'out', 'moved')
        os.rename(self.path, moved)
        os.symlink(self.outside, self.path)
        self.run.write('manifest.json', '{}')
        self.assertTrue(os.path.isfile(os.path.join(moved, 'manifest.json')))
        self.assertOutsideUntouched()

    # ---- files ---------------------------------------------------------

    def test_write_makes_a_regular_file(self):
        self.run.mkdir('logs')
        self.run.write('logs/a.txt', 'hello', mode=0o640)
        st = os.lstat(self.p('logs', 'a.txt'))
        self.assertTrue(stat.S_ISREG(st.st_mode))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o640)
        self.assertEqual((st.st_uid, st.st_gid), (UID, GID))
        with open(self.p('logs', 'a.txt')) as f:
            self.assertEqual(f.read(), 'hello')
        # Written again: emptied first.
        self.run.write('logs/a.txt', b'x')
        with open(self.p('logs', 'a.txt')) as f:
            self.assertEqual(f.read(), 'x')

    def test_write_refuses_a_link(self):
        os.symlink(self.secret, self.p('mic-input.wav'))
        with self.assertRaises(OSError):
            self.run.write('mic-input.wav', b'RIFF')
        self.assertOutsideUntouched()
        # A link to nothing: nothing is made where it points either.
        missing = os.path.join(self.outside, 'made')
        os.symlink(missing, self.p('manifest.json'))
        with self.assertRaises(OSError):
            self.run.write('manifest.json', '{}')
        self.assertFalse(os.path.lexists(missing))

    def test_write_refuses_a_linked_directory(self):
        os.symlink(self.outside, self.p('logs'))
        with self.assertRaises(OSError):
            self.run.write('logs/secret', 'x')
        with self.assertRaises(OSError):
            self.run.open_text('logs/browser-mic.out')
        with self.assertRaises(OSError):
            self.run.mkdir('logs/sub')
        self.assertOutsideUntouched()

    def test_write_refuses_a_hard_link_without_emptying_it(self):
        try:
            os.link(self.secret, self.p('nft-counters.txt'))
        except OSError:
            self.skipTest('hard links to the file are not allowed here')
        with self.assertRaises(OSError):
            self.run.write('nft-counters.txt', 'x')
        with self.assertRaises(OSError):
            self.run.open_text('nft-counters.txt')
        self.assertOutsideUntouched()

    def test_write_refuses_a_fifo_without_blocking(self):
        os.mkfifo(self.p('harness.log'))
        with self.assertRaises(OSError):
            self.run.open_text('harness.log', append=True)
        with self.assertRaises(OSError):
            self.run.write('harness.log', 'x')
        self.assertIsNone(self.run.read('harness.log'))

    def test_paths_stay_inside(self):
        # The run directory is tmp/out/run-1: ../../outside is the file.
        for rel in ('../../outside/secret', '/etc/passwd', '', 'a/../../x'):
            with self.assertRaises(OSError):
                self.run.write(rel, 'x')
        self.assertOutsideUntouched()

    def test_open_text(self):
        self.run.mkdir('logs')
        with self.run.open_text('logs/browser-mic.out') as f:
            # A process's output: blocking, as any file it is given.
            self.assertTrue(os.get_blocking(f.fileno()))
            f.write('one\n')
        with self.run.open_text('logs/browser-mic.out', append=True,
                                owned=False) as f:
            f.write('two\n')
        with open(self.p('logs', 'browser-mic.out')) as f:
            self.assertEqual(f.read(), 'one\ntwo\n')
        st = os.lstat(self.p('logs', 'browser-mic.out'))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o644)
        self.assertEqual(st.st_uid, UID)
        os.symlink(self.secret, self.p('logs', 'tcpdump-all.out'))
        with self.assertRaises(OSError):
            self.run.open_text('logs/tcpdump-all.out', owned=False)
        self.assertOutsideUntouched()

    def test_read(self):
        self.run.write('pages.json', '{"a": 1}')
        self.assertEqual(self.run.read('pages.json'), b'{"a": 1}')
        os.symlink(self.secret, self.p('tor-argv.txt'))
        self.assertIsNone(self.run.read('tor-argv.txt'))
        self.assertIsNone(self.run.read('missing'))

    def test_replace_replaces_a_link_instead_of_following_it(self):
        self.run.mkdir('profile/Default', 0o700)
        os.symlink(self.secret, self.p('profile', 'Default', 'Preferences'))
        self.run.replace('profile/Default/Preferences', '{}', mode=0o600)
        st = os.lstat(self.p('profile', 'Default', 'Preferences'))
        self.assertTrue(stat.S_ISREG(st.st_mode))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o600)
        self.assertOutsideUntouched()

    # ---- directories ---------------------------------------------------

    def test_mkdir(self):
        self.run.mkdir('profile/Default', 0o700)
        for rel, mode in (('profile', 0o755), ('profile/Default', 0o700)):
            st = os.lstat(self.p(rel))
            self.assertTrue(stat.S_ISDIR(st.st_mode))
            self.assertEqual(stat.S_IMODE(st.st_mode), mode)
            self.assertEqual((st.st_uid, st.st_gid), (UID, GID))
        # Again, on what is there: no error, the mode set again.
        self.run.mkdir('profile', 0o700)
        self.assertEqual(stat.S_IMODE(os.lstat(self.p('profile')).st_mode),
                         0o700)

    def test_mkdir_refuses_a_link(self):
        self.run.mkdir('profile', 0o700)
        os.symlink(self.outside, self.p('profile', 'Default'))
        with self.assertRaises(OSError):
            self.run.mkdir('profile/Default', 0o777)
        self.assertOutsideUntouched()
        self.assertFalse(self.run.exists('profile/Default/Preferences'))
        self.assertTrue(self.run.exists('profile/Default'))

    def test_copy_in(self):
        src = os.path.join(self.tmp, 'staging')
        os.mkdir(src, 0o700)
        with open(os.path.join(src, 'server.key'), 'w') as f:
            f.write('key')
        os.chmod(os.path.join(src, 'server.key'), 0o600)
        os.symlink(self.secret, os.path.join(src, 'link'))
        self.run.copy_in(src, 'certs')
        st = os.lstat(self.p('certs', 'server.key'))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o600)
        self.assertEqual(st.st_uid, UID)
        self.assertFalse(os.path.lexists(self.p('certs', 'link')))

    def test_chown_tree_follows_no_link(self):
        self.run.mkdir('logs')
        self.run.write('logs/x.out', 'x', owned=False)
        os.symlink(self.outside, self.p('logs', 'to-dir'))
        os.symlink(self.secret, self.p('to-file'))
        self.run.chown_tree()
        st = os.lstat(self.p('logs', 'x.out'))
        self.assertEqual((st.st_uid, st.st_gid), (UID, GID))
        # The links themselves are left as they are.
        self.assertTrue(os.path.islink(self.p('to-file')))
        self.assertOutsideUntouched()

    @unittest.skipUnless(os.geteuid() == 0, 'needs root to link a file of '
                         'root\'s')
    def test_chown_tree_leaves_a_hard_link(self):
        # A process of the user links a file of root's into the run
        # directory (where fs.protected_hardlinks allows it): the run's end
        # must not give that file to the user.
        os.link(self.secret, self.p('shadow'))
        self.run.mkdir('logs')
        self.run.write('logs/x.out', 'x', owned=False)
        self.run.chown_tree()
        self.assertEqual(os.lstat(self.secret).st_uid, 0)
        self.assertEqual(os.lstat(self.p('logs', 'x.out')).st_uid, UID)
        self.assertOutsideUntouched()

    def test_create_refuses_a_link_on_the_way(self):
        # A directory of the user's on the way to --out swapped for a link
        # to a directory of root's (as ~/MoltBrowser for /etc).
        rootonly = os.path.join(self.tmp, 'rootonly')
        os.mkdir(rootonly, 0o755)
        users = os.path.join(self.tmp, 'users')
        os.mkdir(users, 0o755)
        os.chown(users, UID, GID)
        os.symlink(rootonly, os.path.join(users, 'MoltBrowser'))
        with self.assertRaises(OSError) as cm:
            rundir.RunDir.create(os.path.join(
                users, 'MoltBrowser', 'moltleak-runs', 'run-2'), UID, GID)
        self.assertEqual(cm.exception.errno, errno.ELOOP)
        self.assertEqual(os.listdir(rootonly), [])

    @unittest.skipUnless(os.geteuid() == 0, 'needs a directory of root\'s '
                         'that only root can change')
    def test_create_follows_a_link_only_root_could_put_there(self):
        # A link in a directory of root's that no one else can write (a
        # /home that leads to var/home) is followed, a relative one too.
        real = os.path.join(self.tmp, 'var', 'home')
        os.makedirs(real, 0o755)
        top = os.path.join(self.tmp, 'top')
        os.mkdir(top, 0o755)
        os.symlink('../var/home', os.path.join(top, 'home'))
        run = rundir.RunDir.create(os.path.join(top, 'home', 'u', 'run-2'),
                                   UID, GID)
        run.close()
        st = os.lstat(os.path.join(real, 'u', 'run-2'))
        self.assertEqual(st.st_uid, UID)
        # Not once another account can write there.
        os.chmod(top, 0o775)
        os.chown(top, 0, GID + 1)
        with self.assertRaises(OSError) as cm:
            rundir.RunDir.create(os.path.join(top, 'home', 'u', 'run-3'),
                                 UID, GID)
        self.assertEqual(cm.exception.errno, errno.ELOOP)
        self.assertEqual(os.listdir(os.path.join(real, 'u')), ['run-2'])

    def test_open_out_dir_makes_what_is_missing(self):
        path = os.path.join(self.tmp, 'a', 'b', 'c')
        fd = rundir.open_out_dir(path)
        try:
            self.assertEqual(os.fstat(fd).st_ino, os.stat(path).st_ino)
        finally:
            os.close(fd)
        with self.assertRaises(OSError):
            rundir.open_out_dir('relative/path')
        with open(os.path.join(self.tmp, 'a', 'file'), 'w') as f:
            f.write('x')
        with self.assertRaises(OSError) as cm:
            rundir.open_out_dir(os.path.join(self.tmp, 'a', 'file', 'd'))
        self.assertEqual(cm.exception.errno, errno.ENOTDIR)


def st(uid, gid, mode):
    """A directory's stat with `uid`, `gid` and `mode`."""
    return os.stat_result((stat.S_IFDIR | mode, 0, 0, 2, uid, gid, 0, 0, 0,
                           0))


class Accounts(object):
    """A stand-in for the system's group and account databases
    (rundir.AccountDatabase): `groups` maps a gid to (name, members), and
    `accounts` lists (name, uid, primary gid)."""

    def __init__(self, groups, accounts):
        self.groups = groups
        self.listed = accounts

    def group_name(self, gid):
        return self.groups[gid][0] if gid in self.groups else None

    def group_members(self, gid):
        return list(self.groups[gid][1]) if gid in self.groups else None

    def uid_of(self, name):
        for account, uid, _ in self.listed:
            if account == name:
                return uid
        return None

    def accounts(self):
        return list(self.listed)


def UserPrivateGroups(gid=None):
    """A system with user private groups (Debian, Ubuntu, Fedora, Arch and
    most others): every account's primary group is its own, named after it,
    with no other member. The user's is `gid` (GID by default)."""
    gid = GID if gid is None else gid
    return Accounts(
        {0: ('root', []), gid: ('moltuser', []), 1001: ('alice', []),
         1002: ('bob', []), 100: ('users', ['alice', 'bob'])},
        [('root', 0, 0), ('moltuser', UID, gid), ('alice', 1001, 1001),
         ('bob', 1002, 1002), ('daemon', 1, 1)])


def SharedUsersGroup(gid=None):
    """A system where every user's primary group is one shared group
    (`users`, as on systems without user private groups): `gid` (GID by
    default)."""
    gid = GID if gid is None else gid
    return Accounts(
        {0: ('root', []), gid: ('users', [])},
        [('root', 0, 0), ('moltuser', UID, gid), ('alice', 1001, gid),
         ('bob', 1002, gid)])


class OtherAccountsInGroupTest(unittest.TestCase):
    """rundir.other_accounts_in_group() and the --out check it gives, on
    both kinds of system."""

    def test_user_private_groups(self):
        upg = UserPrivateGroups()
        self.assertEqual(rundir.other_accounts_in_group(GID, UID, upg), [])
        rundir.check_out_dir(st(UID, GID, 0o775), 'x', UID, GID, upg)
        # Root's group has root alone.
        self.assertEqual(rundir.other_accounts_in_group(0, UID, upg), [])
        rundir.check_out_dir(st(0, 0, 0o775), 'x', UID, GID, upg)

    def test_a_shared_primary_group(self):
        shared = SharedUsersGroup()
        self.assertEqual(rundir.other_accounts_in_group(GID, UID, shared),
                         ['alice', 'bob'])
        with self.assertRaises(OSError) as cm:
            rundir.check_out_dir(st(UID, GID, 0o775), '/home/out', UID, GID,
                                 shared)
        self.assertEqual(cm.exception.errno, errno.EPERM)
        message = str(cm.exception)
        for part in ('users (gid %d)' % GID, 'alice, bob', 'chmod g-w',
                     'another --out', '/home/out'):
            self.assertIn(part, message)
        # Not group-writable, or sticky as /tmp is: no one else can rename
        # the run directory.
        rundir.check_out_dir(st(UID, GID, 0o755), 'x', UID, GID, shared)
        rundir.check_out_dir(st(UID, GID, 0o1775), 'x', UID, GID, shared)
        rundir.check_out_dir(st(0, 0, 0o1777), 'x', UID, GID, shared)

    def test_another_member_of_a_private_group(self):
        # A user private group that lists another account as a member.
        accounts = UserPrivateGroups()
        accounts.groups[GID] = ('moltuser', ['moltuser', 'bob'])
        self.assertEqual(rundir.other_accounts_in_group(GID, UID, accounts),
                         ['bob'])
        with self.assertRaises(OSError):
            rundir.check_out_dir(st(UID, GID, 0o775), 'x', UID, GID,
                                 accounts)
        # A member no account has (removed, or not yet made) counts too.
        accounts.groups[GID] = ('moltuser', ['ghost'])
        self.assertEqual(rundir.other_accounts_in_group(GID, UID, accounts),
                         ['ghost'])

    def test_root_and_the_user_dont_count(self):
        accounts = UserPrivateGroups()
        accounts.groups[GID] = ('moltuser', ['root', 'moltuser'])
        accounts.listed.append(('toor', 0, GID))
        accounts.listed.append(('moltuser2', UID, GID))
        self.assertEqual(rundir.other_accounts_in_group(GID, UID, accounts),
                         [])

    def test_root_group_with_system_accounts(self):
        # Root's group as the primary group of other accounts (sync,
        # shutdown, halt and operator have gid 0 on Fedora and RHEL).
        accounts = UserPrivateGroups()
        accounts.listed += [('sync', 5, 0), ('operator', 11, 0)]
        self.assertEqual(rundir.other_accounts_in_group(0, UID, accounts),
                         ['sync', 'operator'])
        with self.assertRaises(OSError):
            rundir.check_out_dir(st(0, 0, 0o775), 'x', UID, GID, accounts)
        rundir.check_out_dir(st(0, 0, 0o1777), 'x', UID, GID, accounts)
        rundir.check_out_dir(st(0, 0, 0o755), 'x', UID, GID, accounts)

    def test_a_group_the_database_doesnt_list(self):
        accounts = Accounts({}, [('root', 0, 0), ('moltuser', UID, GID)])
        self.assertTrue(rundir.other_accounts_in_group(GID, UID, accounts))
        with self.assertRaises(OSError) as cm:
            rundir.check_out_dir(st(UID, GID, 0o775), 'x', UID, GID,
                                 accounts)
        self.assertIn('gid %d' % GID, str(cm.exception))

    def test_many_others_are_summarised(self):
        accounts = SharedUsersGroup()
        accounts.listed += [('u%d' % i, 2000 + i, GID) for i in range(10)]
        with self.assertRaises(OSError) as cm:
            rundir.check_out_dir(st(UID, GID, 0o775), 'x', UID, GID,
                                 accounts)
        self.assertIn('and 7 more', str(cm.exception))

    def test_the_systems_own_databases(self):
        # The real databases answer without error, whatever this system
        # has; root is never another account.
        others = rundir.other_accounts_in_group(0, os.getuid())
        self.assertNotIn('root', others)


class HarnessWritesTest(unittest.TestCase):
    """The harness's own writes into the run directory (the microphone
    launch's WAV, the profile's Preferences, the logs) go through RunDir."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.outside = os.path.join(self.tmp, 'outside')
        os.mkdir(self.outside, 0o700)
        self.secret = os.path.join(self.outside, 'shadow')
        with open(self.secret, 'w') as f:
            f.write('root:x\n')
        os.chmod(self.secret, 0o600)
        run = object.__new__(harness.Run)
        run.dir = os.path.join(self.tmp, 'run')
        run.uid, run.gid = UID, GID
        run.rundir = None
        run.make_run_dir()
        self.addCleanup(run.rundir.close)
        self.run = run

    def secret_state(self):
        st = os.lstat(self.secret)
        with open(self.secret) as f:
            return f.read(), stat.S_IMODE(st.st_mode), st.st_uid

    def test_user_write_and_mkdir(self):
        before = self.secret_state()
        os.symlink(self.secret, self.run.path('mic-input.wav'))
        with self.assertRaises(OSError):
            self.run.user_write('mic-input.wav', b'RIFF')
        os.symlink(self.outside, self.run.path('logs'))
        with self.assertRaises(OSError):
            self.run.user_mkdir('logs', mode=0o777)
        self.assertEqual(self.secret_state(), before)
        self.assertEqual(stat.S_IMODE(os.lstat(self.outside).st_mode), 0o700)

    def test_set_profile_prefs_through_a_link(self):
        before = self.secret_state()
        self.run.user_mkdir('profile', mode=0o700)
        os.symlink(self.outside, self.run.path('profile', 'Default'))
        with self.assertRaises(OSError):
            self.run.set_profile_prefs({'a': 1})
        self.assertEqual(self.secret_state(), before)
        self.assertEqual(sorted(os.listdir(self.outside)), ['shadow'])

    def test_set_profile_prefs_temporary_file_is_a_link(self):
        before = self.secret_state()
        self.run.user_mkdir('profile', 'Default', mode=0o700)
        os.symlink(self.secret, self.run.path('profile', 'Default',
                                              'Preferences.moltleak'))
        with self.assertRaises(OSError):
            self.run.set_profile_prefs({'a': 1})
        self.assertEqual(self.secret_state(), before)

    def test_set_profile_prefs(self):
        self.run.user_mkdir('profile', 'Default', mode=0o700)
        self.run.rundir.write('profile/Default/Preferences',
                              json.dumps({'a': {'b': 1}, 'c': 2}))
        self.run.set_profile_prefs({'a': {'d': 3}})
        with open(self.run.path('profile', 'Default', 'Preferences')) as f:
            self.assertEqual(json.load(f), {'a': {'b': 1, 'd': 3}, 'c': 2})

    def test_log(self):
        self.run.log('hello')
        with open(self.run.path('harness.log')) as f:
            self.assertTrue(f.read().endswith('hello\n'))
        os.unlink(self.run.path('harness.log'))
        os.symlink(self.secret, self.run.path('harness.log'))
        before = self.secret_state()
        self.run.log('not there')   # refused quietly, as a failed log write
        self.assertEqual(self.secret_state(), before)


class HarnessReadsTest(unittest.TestCase):
    """What the harness reads in the run directory (the kit's onion service
    addresses, MoltNet's torrc and Tor's consensus) it reads through
    RunDir too: read by path, a link a process of the user put there would
    have root read a file only root may read, and what it read lands in the
    manifest and the logs, which the user reads."""

    ONION = 'a' * 56 + '.onion'
    CONSENSUS = 'r name AAECAwQFBgcICQoLDA0ODxAREhM= x\n'
    RELAY = '000102030405060708090A0B0C0D0E0F10111213'

    def setUp(self):
        HarnessWritesTest.setUp(self)
        # Files only root may read, whose contents the harness would take
        # if it read them.
        self.root_files = {}
        for name, text in (('hostname', self.ONION + '\n'),
                           ('torrc', 'ConnLimit 4242\n'),
                           ('cached-consensus', self.CONSENSUS)):
            path = os.path.join(self.outside, name)
            with open(path, 'w') as f:
                f.write(text)
            os.chmod(path, 0o600)
            self.root_files[name] = path

    def test_onion_hostname_is_read_through_the_descriptor(self):
        self.run.user_mkdir('onion', mode=0o700)
        self.run.user_mkdir('onion', 'a', mode=0o700)
        hostname = self.run.path('onion', 'a', 'hostname')
        self.assertIsNone(self.run.onion_hostname('a'))
        os.symlink(self.root_files['hostname'], hostname)
        self.assertIsNone(self.run.onion_hostname('a'))
        os.unlink(hostname)
        os.link(self.root_files['hostname'], hostname)
        self.assertIsNone(self.run.onion_hostname('a'))
        os.unlink(hostname)
        self.run.rundir.write('onion/a/hostname', 'root:x\n')
        self.assertIsNone(self.run.onion_hostname('a'))   # not an address
        self.run.rundir.write('onion/a/hostname', self.ONION + '\n')
        self.assertEqual(self.run.onion_hostname('a'), self.ONION)

    def test_read_user_path_stays_in_the_run_directory(self):
        self.run.rundir.write('torrc', 'DataDirectory %s\nConnLimit 9\n' %
                              self.run.path('tor'))
        torrc = self.run.path('torrc')
        self.assertEqual(support.torrc_value(torrc, 'ConnLimit',
                                             read=self.run.read_user_path),
                         '9')
        outside_torrc = self.root_files['torrc']
        for outside in (outside_torrc, self.run.path('..', 'outside',
                                                     'torrc'),
                        self.run.dir, '', None):
            self.assertIsNone(self.run.read_user_path(outside), outside)
        # A torrc the user's processes named (the Tor wrapper's log is
        # theirs) outside the run directory, or a link to one, isn't read.
        os.symlink(outside_torrc, self.run.path('linked-torrc'))
        for path in (outside_torrc, self.run.path('linked-torrc')):
            self.assertIsNone(support.torrc_value(
                path, 'ConnLimit', read=self.run.read_user_path), path)
        # Nor a consensus that is a link to one.
        self.run.user_mkdir('tor')
        consensus = self.run.path('tor', 'cached-consensus')
        os.symlink(self.root_files['cached-consensus'], consensus)
        self.assertEqual(support.consensus_identities(
            self.run.path('tor'), read=self.run.read_user_path), set())
        os.unlink(consensus)
        self.run.rundir.write('tor/cached-consensus', self.CONSENSUS)
        self.assertEqual(support.consensus_identities(
            self.run.path('tor'), read=self.run.read_user_path),
            {self.RELAY})


if __name__ == '__main__':
    unittest.main()
