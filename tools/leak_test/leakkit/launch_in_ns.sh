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
#   * optionally moves the DevTools pipe to fds 3 and 4
#     (--remote-debugging-pipe);
#   * drops to the invoking user and runs the command.
#
# usage: launch_in_ns.sh PROCS RESOLV NSSWITCH UID GID IN_FD OUT_FD -- CMD...
#   PROCS      the cgroup.procs file to join
#   RESOLV     file to mount over /etc/resolv.conf, or -
#   NSSWITCH   file to mount over /etc/nsswitch.conf, or -
#   IN_FD/OUT_FD  fds that become 3 and 4, or -
set -euo pipefail

private=0
if [[ ${1:-} == --private-mnt ]]; then
  private=1
  shift
fi
if [[ $# -lt 8 ]]; then
  echo "usage: $0 PROCS RESOLV NSSWITCH UID GID IN_FD OUT_FD -- CMD..." >&2
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

# The bind mounts below must never land in the caller's mount namespace:
# re-run in a private one first.
if [[ $private != 1 ]]; then
  exec unshare --mount --propagation private -- /bin/bash "$0" \
    --private-mnt "$procs" "$resolv" "$nsswitch" "$uid" "$gid" "$in_fd" \
    "$out_fd" -- "$@"
fi

echo "$$" > "$procs"

if [[ $resolv != - && -e /etc/resolv.conf ]]; then
  mount --bind "$resolv" "$(readlink -f /etc/resolv.conf)"
fi
if [[ $nsswitch != - && -e /etc/nsswitch.conf ]]; then
  mount --bind "$nsswitch" "$(readlink -f /etc/nsswitch.conf)"
fi

if [[ $in_fd != - ]]; then
  eval "exec 3<&${in_fd} 4>&${out_fd} ${in_fd}<&- ${out_fd}>&-"
fi

exec setpriv --reuid="$uid" --regid="$gid" --init-groups -- "$@"
