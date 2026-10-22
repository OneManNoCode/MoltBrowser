// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// A service worker, a dedicated and a shared worker each fetch a canary
// name, the page's own origin and open a WebSocket to a canary name.
'use strict';
function fromWorker(w, port) {
  return new Promise((resolve) => {
    (port || w).onmessage = (e) => resolve(e.data);
    setTimeout(() => resolve('timeout'), 20000);
  });
}
(async () => {
  const base = '/t/' + ML.cfg.nonce + '/';
  await ML.step('sw', async () => {
    const reg = await navigator.serviceWorker.register(base + 'sw.js',
        {scope: base});
    const sw = reg.installing || reg.waiting || reg.active;
    const got = await new Promise((resolve) => {
      navigator.serviceWorker.onmessage = (e) => resolve(e.data);
      sw.postMessage('go');
      setTimeout(() => resolve('timeout'), 20000);
    });
    return {registered: true, result: got};
  }, 30000);
  await ML.step('worker', () => fromWorker(new Worker(base + 'worker.js')),
      25000);
  await ML.step('shared', () => {
    const s = new SharedWorker(base + 'shared_worker.js');
    s.port.start();
    return fromWorker(s, s.port);
  }, 25000);
  ML.finish();
})();
