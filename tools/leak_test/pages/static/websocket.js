// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// WebSockets: to a canary name (ws and wss), an IP literal, and the page's
// own onion (which must work, through Tor).
'use strict';
function ws(url, ms) {
  return new Promise((resolve) => {
    let s;
    try {
      s = new WebSocket(url);
    } catch (e) {
      resolve(ML.errInfo(e));
      return;
    }
    const t = setTimeout(() => {
      resolve('timeout');
      s.close();
    }, ms);
    s.onopen = () => {
      clearTimeout(t);
      resolve('open');
      s.close();
    };
    s.onerror = () => {
      clearTimeout(t);
      resolve('error');
    };
  });
}
(async () => {
  const d = ML.cfg;
  ML.set('ws_canary', await ws('wss://' + ML.host('ws') + '/', 8000));
  ML.set('ws_canary_plain', await ws('ws://' + ML.host('wsplain') + '/',
      8000));
  ML.set('ws_ip', await ws('ws://' + d.cip4 + ':8080/', 8000));
  ML.set('ws_same', await ws(location.origin.replace(/^http/, 'ws') +
      '/ws/' + d.nonce, 30000));
  ML.finish();
})();
