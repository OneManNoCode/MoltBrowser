// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Background Fetch from a service worker registration: one request to a
// name of this test (`bgf-<nonce>.<canary domain>`), made by the browser's
// download service. report.py's identity.background_fetch_circuits reads
// the SOCKS username it went to Tor with: it must be the page's site's.
// The name doesn't resolve, so the fetch fails after its SOCKS request.
'use strict';
(async () => {
  const base = '/t/' + ML.cfg.nonce + '/';
  await ML.step('bgfetch', async () => {
    if (!('serviceWorker' in navigator)) {
      return {registered: false, api: false};
    }
    const reg = await navigator.serviceWorker.register(base + 'sw.js',
        {scope: base});
    await ML.timeout(navigator.serviceWorker.ready, 20000);
    if (!reg.backgroundFetch) {
      return {registered: true, api: false};
    }
    const url = 'https://' + ML.host('bgf') + '/canary/bgf-' + ML.cfg.nonce;
    let f;
    try {
      f = await reg.backgroundFetch.fetch('bgf-' + ML.cfg.nonce, [url],
          {title: 'moltleak'});
    } catch (e) {
      return {registered: true, api: true, started: false,
        error: ML.errName(e)};
    }
    const done = await new Promise((resolve) => {
      const check = () => {
        if (f.result) {
          resolve(true);
        }
      };
      f.addEventListener('progress', check);
      check();
      setTimeout(() => resolve(false), 30000);
    });
    return {registered: true, api: true, started: true, done,
      result: f.result || '', failureReason: f.failureReason || ''};
  }, 50000);
  ML.finish();
})();
