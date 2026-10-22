// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// navigator.sendBeacon, keepalive fetch, a pagehide beacon, and <a ping>
// (the harness calls mlGo(), which follows the link).
'use strict';
const sameBeacon = '/canary/beacon-' + ML.cfg.nonce;
ML.set('beacon_canary', navigator.sendBeacon('https://' + ML.host('beacon') +
    '/b', 'x'));
ML.set('beacon_same', navigator.sendBeacon(sameBeacon, 'x'));
fetch('https://' + ML.host('keepalive') + '/k', {method: 'POST', body: 'x',
  keepalive: true, mode: 'no-cors'}).catch(() => {});
addEventListener('pagehide', () => {
  navigator.sendBeacon('https://' + ML.host('pagehide') + '/u', 'x');
});
window.mlGo = () => {
  document.getElementById('go').click();
};
ML.finish();
