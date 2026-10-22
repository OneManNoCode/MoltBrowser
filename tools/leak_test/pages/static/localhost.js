// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Local services and the LAN: the loopback canary (127.0.0.1, ::1,
// localhost, this machine's own address), the namespace's gateway, private
// and link-local addresses. MoltNet sends them to Tor, which refuses.
'use strict';
(async () => {
  const d = ML.cfg;
  const p = d.lport;
  const targets = {
    loopback4: 'http://127.0.0.1:' + p + '/',
    loopback6: 'http://[::1]:' + p + '/',
    localhost: 'http://localhost:' + p + '/',
    own_address: 'http://' + d.nsip + ':' + p + '/',
    gateway: 'http://' + d.gw + ':' + p + '/',
    gateway6: 'http://[' + d.gw6 + ']:' + p + '/',
    lan_192: 'http://192.168.1.1/',
    lan_10: 'http://10.0.0.1/',
    link_local: 'http://169.254.169.254/latest/meta-data/',
  };
  const fetchResults = {};
  await Promise.all(Object.entries(targets).map(async ([k, url]) => {
    fetchResults[k] = await ML.probe(url, {mode: 'no-cors'}, 15000);
  }));
  ML.set('fetch', fetchResults);
  const img = new Image();
  img.src = 'http://127.0.0.1:' + p + '/img';
  try {
    new WebSocket('ws://127.0.0.1:' + p + '/ws');
  } catch (e) {
    // ignored: the canary records any connection
  }
  await ML.sleep(1500);
  ML.finish();
})();
