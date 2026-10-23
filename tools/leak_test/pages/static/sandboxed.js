// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Served with `Content-Security-Policy: sandbox allow-scripts`, so this
// document's origin is opaque. Its requests after it loaded go to Tor with
// the SOCKS username of an opaque top-level site; two such pages on two
// sites must still get different usernames (per-site circuits).
'use strict';
(async () => {
  ML.set('opaque_origin', self.origin === 'null');
  await ML.step('img', () => new Promise((resolve) => {
    const img = new Image();
    img.onload = () => resolve('load');
    img.onerror = () => resolve('error');
    img.src = '/canary/sbx-' + ML.cfg.nonce + '.png';
  }));
  ML.set('fetch', await ML.probe('/canary/sbxf-' + ML.cfg.nonce));
  ML.finish();
})();
