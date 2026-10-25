// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the end of a MoltNet identity, as its HTTP network session's
// SOCKS connections see it.
//
// When a MoltNet identity ends, its network context stops what it has under
// way (services/network/molt_ended_identity_policy.h). Some of that would
// still write to Tor as it goes: a WebSocket closed sends its closing
// handshake, an HTTP/2 session drains the frames it had queued (an
// RST_STREAM for each stream cancelled, the rest of a frame it was
// writing), a TLS socket the record it had buffered. Every connection of a
// MoltNet context goes to Tor through a SOCKS5 socket, so that is where the
// end is enforced, under every protocol above it: once the session's
// MoltIdentityEnd is marked, its SOCKS5 sockets refuse every write
// (SOCKS5ClientSocket::Write(), ERR_ABORTED, which none of the layers above
// retries), and SOCKSConnectJob starts no new SOCKS handshake, which would
// make Tor open a stream. Reads go on: a socket still open may receive what
// the server sends until the layers above close it.
//
// One per MoltNet HttpNetworkSession (made when it has per-site socket
// pools), shared with each of its SOCKS5 sockets, which may outlive the
// session. Not thread-safe: the network service uses it on its one thread.

#ifndef NET_SOCKET_MOLT_IDENTITY_END_H_
#define NET_SOCKET_MOLT_IDENTITY_END_H_

#include "base/memory/ref_counted.h"

namespace net {

class MoltIdentityEnd : public base::RefCounted<MoltIdentityEnd> {
 public:
  MoltIdentityEnd() = default;
  MoltIdentityEnd(const MoltIdentityEnd&) = delete;
  MoltIdentityEnd& operator=(const MoltIdentityEnd&) = delete;

  // The identity has ended; there is no way back.
  void Mark() { ended_ = true; }
  bool ended() const { return ended_; }

 private:
  friend class base::RefCounted<MoltIdentityEnd>;
  ~MoltIdentityEnd() = default;

  bool ended_ = false;
};

// Whether `end` (null: a session that isn't MoltNet's) has been marked.
inline bool MoltIdentityHasEnded(const MoltIdentityEnd* end) {
  return end && end->ended();
}

}  // namespace net

#endif  // NET_SOCKET_MOLT_IDENTITY_END_H_
