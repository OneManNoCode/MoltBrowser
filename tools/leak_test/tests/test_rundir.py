#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for leakkit/rundir.py: what the harness writes into a run directory
its user owns never reaches a file outside it, whatever the user's
processes put there (links, hard links, FIFOs, a directory swapped for a
link), and the harness's own writes go through it.

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import json
import os
import shutil
import stat
import tempfile
import unittest

from leakkit import harness
from leakkit import rundir

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
        self.assertEqual(os.lstat(self.p('to-file')).st_uid, UID)
        self.assertOutsideUntouched()


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


if __name__ == '__main__':
    unittest.main()
