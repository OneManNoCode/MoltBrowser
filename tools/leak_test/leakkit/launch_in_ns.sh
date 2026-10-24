#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Starts one of the leak kit's processes in the test network namespace.
# The harness runs this as root through nsenter --net=/run/netns/<ns>; it
# first re-runs itself in a private mount namespace (unshare --mount), so
# the mounts below only exist for this process and its children. It:
#   * moves itself into a cgroup of the run (browser, tor or kit), which the
#     namespace's firewall uses to tell Tor's traffic from everything else;
#   * points name resolution at the kit's resolver: /etc/resolv.conf says
#     "nameserver 127.0.0.1" and /etc/nsswitch.conf "hosts: files dns", so
#     every lookup (Chromium's own resolver and getaddrinfo) is sent inside
#     the namespace, where it is logged, instead of to systemd-resolved or
#     avahi over a Unix socket, which would ask from outside the namespace;
#   * optionally (--policy-dir) mounts the run's own directory of managed
#     policies over /etc/chromium/policies/managed, where Linux builds
#     without Chrome branding (MoltBrowser among them) read them, so the
#     kit can set a policy while the browser runs; the host's own policies
#     there, if any, are hidden from this launch only;
#   * optionally moves the DevTools pipe to fds 3 and 4
#     (--remote-debugging-pipe);
#   * drops to the invoking user and runs the command.
#
# usage: launch_in_ns.sh [--policy-dir DIR] PROCS RESOLV NSSWITCH UID GID \
#            IN_FD OUT_FD -- CMD...
#   DIR        an existing directory to mount over the managed-policy
#              directory, which must exist (the harness makes it)
#   PROCS      the cgroup.procs file to join
#   RESOLV     file to mount over /etc/resolv.conf, or -
#   NSSWITCH   file to mount over /etc/nsswitch.conf, or -
#   IN_FD/OUT_FD  fds that become 3 and 4, or -
set -euo pipefail

MANAGED_POLICY_DIR=/etc/chromium/policies/managed

private=0
policy_dir=-
while [[ $# -gt 0 ]]; do
  case $1 in
    --private-mnt) private=1; shift ;;
    --policy-dir)
      [[ $# -ge 2 ]] || { echo "--policy-dir needs a directory" >&2; exit 64; }
      policy_dir=$2
      shift 2
      ;;
    *) break ;;
  esac
done
if [[ $# -lt 8 ]]; then
  echo "usage: $0 [--policy-dir DIR] PROCS RESOLV NSSWITCH UID GID IN_FD OUT_FD -- CMD..." >&2
  exit 64
fi
procs=$1 resolv=$2 nsswitch=$3 uid=$4 gid=$5 in_fd=$6 out_fd=$7
shift 7
if [[ ${1:-} == -- ]]; then
  shift
fi
[[ $uid =~ ^[0-9]+$ && $gid =~ ^[0-9]+$ ]] || { echo "bad uid/gid" >&2; exit 64; }

if [[ $in_fd != - ]]; then
  [[ $in_fd =~ ^[0-9]+$ && $out_fd =~ ^[0-9]+$ ]] || { echo "bad fds" >&2; exit 64; }
  (( in_fd > 4 && out_fd > 4 )) || { echo "pipe fds must be above 4" >&2; exit 64; }
fi
if [[ $policy_dir != - ]]; then
  [[ $policy_dir == /* && -d $policy_dir ]] || { echo "bad policy dir" >&2; exit 64; }
fi

# The bind mounts below must never land in the caller's mount namespace:
# re-run in a private one first.
if [[ $private != 1 ]]; then
  exec unshare --mount --propagation private -- /bin/bash "$0" \
    --private-mnt --policy-dir "$policy_dir" "$procs" "$resolv" \
    "$nsswitch" "$uid" "$gid" "$in_fd" "$out_fd" -- "$@"
fi

echo "$$" > "$procs"

if [[ $resolv != - && -e /etc/resolv.conf ]]; then
  mount --bind "$resolv" "$(readlink -f /etc/resolv.conf)"
fi
if [[ $nsswitch != - && -e /etc/nsswitch.conf ]]; then
  mount --bind "$nsswitch" "$(readlink -f /etc/nsswitch.conf)"
fi
if [[ $policy_dir != - ]]; then
  [[ -d $MANAGED_POLICY_DIR && ! -L $MANAGED_POLICY_DIR ]] || {
    echo "$MANAGED_POLICY_DIR is missing" >&2; exit 70; }
  mount --bind "$policy_dir" "$MANAGED_POLICY_DIR"
fi

if [[ $in_fd != - ]]; then
  eval "exec 3<&${in_fd} 4>&${out_fd} ${in_fd}<&- ${out_fd}>&-"
fi

exec setpriv --reuid="$uid" --regid="$gid" --init-groups -- "$@"
