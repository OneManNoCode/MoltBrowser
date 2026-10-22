// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// MOLTNET_COMPLIANCE.md §4 step 18: STUN and TURN servers given by name and
// as IP literals, a peer candidate by name and a .local candidate. In a
// MoltNet window the constructor must throw, and none of these names may be
// looked up anywhere.
'use strict';
(async () => {
  const d = ML.cfg;
  const stun = ML.host('stun');
  const turn = ML.host('turn');
  await ML.step('pc', async () => {
    let pc;
    try {
      pc = new RTCPeerConnection({iceServers: [
        {urls: 'stun:' + stun + ':3478'},
        {urls: 'turn:' + turn + ':3478?transport=udp', username: 'u',
         credential: 'c'},
        {urls: 'turn:' + turn + ':3478?transport=tcp', username: 'u',
         credential: 'c'},
        {urls: 'stun:' + d.cip4 + ':3478'},
        {urls: 'turn:[' + d.cip6 + ']:3478', username: 'u', credential: 'c'},
      ]});
    } catch (e) {
      return ML.errInfo(e);
    }
    const types = [];
    pc.onicecandidate = (ev) => {
      if (ev.candidate) {
        const c = ev.candidate.candidate || '';
        types.push((c.match(/ typ (\w+)/) || [])[1] + (c.includes('.local') ?
            '/mdns' : ''));
      }
    };
    pc.createDataChannel('x');
    await pc.setLocalDescription(await pc.createOffer());
    await ML.sleep(4000);
    const peer = new RTCPeerConnection();
    await peer.setRemoteDescription(pc.localDescription);
    await peer.setLocalDescription(await peer.createAnswer());
    for (const host of [ML.host('peer'), crypto.randomUUID() + '.local',
      d.cip4]) {
      try {
        await peer.addIceCandidate({sdpMid: '0', candidate:
            'candidate:1 1 udp 2122260223 ' + host + ' 9 typ host'});
      } catch (e) {
        // recorded through the candidate types only
      }
    }
    await ML.sleep(2000);
    return {available: true, candidate_types: types};
  }, 15000);
  ML.set('codecs', (() => {
    try {
      return RTCRtpSender.getCapabilities('video').codecs.length;
    } catch (e) {
      return ML.errName(e);
    }
  })());
  ML.finish();
})();
