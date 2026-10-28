# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The run directory, as the harness (root) writes into it.

A run directory belongs to the user who runs the kit, and so does most of
what is in it: the browser, Tor and the kit's servers run as that user and
write there. That user's processes can therefore put a symbolic link, a
hard link or a FIFO where the harness is about to write, or swap a
directory for a link, and a write, chmod or chown made by path would reach
some other file with root's rights (a link from logs/browser-mic.out to
/etc/shadow, a profile/Default that leads to /root).

RunDir makes every such operation from a descriptor of the run directory,
opened when the harness made it and checked to be root's own before it is
handed to the user. Each component of a path under it is opened without
following links; the last one must be a directory, or a regular file with
a single link (checked before it is emptied); modes and owners are set on
the open descriptor. A path that doesn't qualify fails with OSError and is
left as it is. Files the harness makes as root first (the servers'
certificates, the test extension) are made in the sandbox's root-owned
private directory and copied in (copy_in()).
"""

import errno
import os
import stat

_DIR_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
# O_NONBLOCK: opening a FIFO someone put there must not block (it then
# fails the regular-file check).
_FILE_FLAGS = os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC


def _parts(rel):
    """The components of `rel`, a relative path inside the run directory."""
    parts = [p for p in rel.split('/') if p not in ('', '.')]
    if not parts or '..' in parts or rel.startswith('/'):
        raise OSError(errno.EINVAL, 'not a path inside the run directory',
                      rel)
    return parts


def _write_all(fd, data):
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]


def check_out_dir(st, path, uid, gid):
    """Refuses the --out directory whose stat is `st` unless it belongs to
    root, the harness or the user (`uid`) and no other account can write in
    it (see RunDir.create())."""
    if st.st_uid not in (0, os.geteuid(), uid):
        raise OSError(errno.EPERM,
                      'the --out directory belongs to another account', path)
    mode = stat.S_IMODE(st.st_mode)
    if mode & stat.S_IWOTH and not mode & stat.S_ISVTX:
        raise OSError(errno.EPERM,
                      'anyone can write in the --out directory and it is not '
                      'sticky', path)
    if mode & stat.S_IWGRP and st.st_gid not in (0, gid):
        raise OSError(errno.EPERM,
                      'another group can write in the --out directory', path)


class RunDir(object):
    """File operations under a run directory, from a descriptor of it."""

    def __init__(self, fd, uid, gid):
        self.fd = fd
        self.uid = uid
        self.gid = gid

    @classmethod
    def create(cls, path, uid, gid):
        """Makes the run directory `path` (it must not exist; its parent, the
        --out directory, may), opens it, checks that it is the directory just
        made, and hands it to the user (`uid`, `gid`).

        The --out directory is opened without following a link in its last
        component, and must belong to root, the harness or the user and be
        writable by no other account (a world-writable one must be sticky,
        as /tmp is; a group-writable one must be root's or the user's own
        group's). Otherwise a link put where --out was, or a directory
        another account controls, would have root make the run directory
        and hand it to the user wherever that leads. The run directory is
        made and opened from the --out directory's descriptor."""
        parent, name = os.path.split(os.path.abspath(path))
        os.makedirs(parent, exist_ok=True)
        pfd = os.open(parent, _DIR_FLAGS)
        try:
            check_out_dir(os.fstat(pfd), parent, uid, gid)
            os.mkdir(name, 0o755, dir_fd=pfd)
            fd = os.open(name, _DIR_FLAGS, dir_fd=pfd)
        finally:
            os.close(pfd)
        try:
            st = os.fstat(fd)
            if st.st_uid != os.geteuid():
                raise OSError(errno.EPERM, 'the run directory was replaced',
                              path)
            os.fchmod(fd, 0o755)
            os.fchown(fd, uid, gid)
        except BaseException:
            os.close(fd)
            raise
        return cls(fd, uid, gid)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    # ---- directories --------------------------------------------------------

    def _open_dir(self, parts, create=False):
        """A descriptor of the directory `parts` under the run directory,
        each component opened without following links; with `create`, the
        missing ones made (0o755, the user's)."""
        fd = os.dup(self.fd)
        try:
            for part in parts:
                made = False
                if create:
                    try:
                        os.mkdir(part, 0o755, dir_fd=fd)
                        made = True
                    except FileExistsError:
                        pass
                sub = os.open(part, _DIR_FLAGS, dir_fd=fd)
                os.close(fd)
                fd = sub
                if made:
                    os.fchmod(fd, 0o755)
                    os.fchown(fd, self.uid, self.gid)
        except BaseException:
            os.close(fd)
            raise
        return fd

    def mkdir(self, rel, mode=0o755):
        """Makes the directory `rel` (and its missing parents) if it isn't
        there, and gives it mode `mode` and the user as owner."""
        fd = self._open_dir(_parts(rel), create=True)
        try:
            os.fchmod(fd, mode)
            os.fchown(fd, self.uid, self.gid)
        finally:
            os.close(fd)

    def exists(self, rel):
        """Whether `rel` names an entry (a link counts; it isn't followed)."""
        parts = _parts(rel)
        try:
            parent = self._open_dir(parts[:-1])
        except OSError:
            return False
        try:
            os.stat(parts[-1], dir_fd=parent, follow_symlinks=False)
            return True
        except OSError:
            return False
        finally:
            os.close(parent)

    # ---- files --------------------------------------------------------------

    @staticmethod
    def _open_at(parent, name, flags, rel):
        """`name` in the directory `parent`, opened with `flags` without
        following a link; refused unless it is a regular file with a single
        link."""
        fd = os.open(name, flags | _FILE_FLAGS, 0o600, dir_fd=parent)
        try:
            st = os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1:
                raise OSError(errno.EPERM, 'not a regular file with one link',
                              rel)
            # A regular file: blocking again, as a process given it as its
            # output expects.
            os.set_blocking(fd, True)
        except BaseException:
            os.close(fd)
            raise
        return fd

    def _write_at(self, parent, name, data, mode, owned, rel):
        if isinstance(data, str):
            data = data.encode('utf-8')
        fd = self._open_at(parent, name, os.O_WRONLY | os.O_CREAT, rel)
        try:
            os.ftruncate(fd, 0)
            _write_all(fd, data)
            os.fchmod(fd, mode)
            if owned:
                os.fchown(fd, self.uid, self.gid)
        finally:
            os.close(fd)

    def write(self, rel, data, mode=0o644, owned=True):
        """Writes `data` (bytes, or text as UTF-8) to the file `rel`, made or
        emptied first, with mode `mode`; owned by the user if `owned`."""
        parts = _parts(rel)
        parent = self._open_dir(parts[:-1])
        try:
            self._write_at(parent, parts[-1], data, mode, owned, rel)
        finally:
            os.close(parent)

    def replace(self, rel, data, mode=0o644, owned=True):
        """Writes `data` to a new file next to `rel` and renames it over
        `rel` (whatever is there is replaced, a link included, never
        followed)."""
        parts = _parts(rel)
        tmp = parts[-1] + '.moltleak'
        parent = self._open_dir(parts[:-1])
        try:
            self._write_at(parent, tmp, data, mode, owned, rel)
            os.rename(tmp, parts[-1], src_dir_fd=parent, dst_dir_fd=parent)
        finally:
            os.close(parent)

    def open_text(self, rel, append=False, owned=True):
        """A text file object writing to `rel` (made, or emptied unless
        `append`), owned by the user if `owned`: a log the harness or a
        process it starts writes."""
        parts = _parts(rel)
        parent = self._open_dir(parts[:-1])
        try:
            fd = self._open_at(parent, parts[-1], os.O_WRONLY | os.O_CREAT |
                               (os.O_APPEND if append else 0), rel)
        finally:
            os.close(parent)
        try:
            if not append:
                os.ftruncate(fd, 0)
            os.fchmod(fd, 0o644)
            if owned:
                os.fchown(fd, self.uid, self.gid)
            return os.fdopen(fd, 'a' if append else 'w')
        except BaseException:
            os.close(fd)
            raise

    def read(self, rel):
        """The bytes of the regular file `rel`, or None if there is none, or
        it isn't one."""
        parts = _parts(rel)
        try:
            parent = self._open_dir(parts[:-1])
        except OSError:
            return None
        try:
            fd = self._open_at(parent, parts[-1], os.O_RDONLY, rel)
        except OSError:
            return None
        finally:
            os.close(parent)
        try:
            chunks = []
            while True:
                chunk = os.read(fd, 1 << 16)
                if not chunk:
                    return b''.join(chunks)
                chunks.append(chunk)
        finally:
            os.close(fd)

    def copy_in(self, src_dir, rel_dir):
        """Copies the regular files directly in `src_dir` (a directory the
        user can't change: the sandbox's private one) into the directory
        `rel_dir`, keeping their modes, owned by the user."""
        self.mkdir(rel_dir, 0o755)
        for name in sorted(os.listdir(src_dir)):
            src = os.path.join(src_dir, name)
            st = os.lstat(src)
            if not stat.S_ISREG(st.st_mode):
                continue
            with open(src, 'rb') as f:
                data = f.read()
            self.write(rel_dir + '/' + name, data, stat.S_IMODE(st.st_mode))

    def chown_tree(self):
        """Gives the run directory and everything in it to the user, without
        following links (os.fwalk works from descriptors)."""
        os.fchown(self.fd, self.uid, self.gid)
        for _, dirs, files, dir_fd in os.fwalk('.', dir_fd=self.fd,
                                               follow_symlinks=False):
            for name in dirs + files:
                try:
                    os.chown(name, self.uid, self.gid, dir_fd=dir_fd,
                             follow_symlinks=False)
                except OSError:
                    pass
