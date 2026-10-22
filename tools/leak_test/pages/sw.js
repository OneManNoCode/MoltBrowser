// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Service worker of the leak kit's workers test (templated per test).
'use strict';
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('message', async (e) => {
  const out = {};
  for (const [k, url] of [
    ['canary', 'https://sw-{{NONCE}}.{{CANARY}}/'],
    ['same', '/canary/sw-{{NONCE}}'],
    ['ip', 'http://{{CIP4}}:8080/sw']]) {
    try {
      const r = await fetch(url, {mode: 'no-cors'});
      out[k] = 'ok ' + r.type;
    } catch (err) {
      out[k] = 'error ' + err.name;
    }
  }
  e.source.postMessage(out);
});
