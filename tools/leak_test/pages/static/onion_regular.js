// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// A regular window must not send anything to an onion host: images, fetch,
// a redirect to an onion, WebSocket, a dedicated and a shared worker.
'use strict';
(async () => {
  const onion = ML.cfg.onionA;
  const out = {};
  out.fetch = await ML.probe('http://' + onion + '/x', {mode: 'no-cors'},
      10000);
  out.img = await new Promise((resolve) => {
    const i = new Image();
    i.onload = () => resolve({ok: true});
    i.onerror = () => resolve({ok: false, error: 'error'});
    i.src = 'http://' + onion + '/x.png';
    setTimeout(() => resolve({ok: false, error: 'timeout'}), 10000);
  });
  out.redirect = await new Promise((resolve) => {
    const i = new Image();
    i.onload = () => resolve({ok: true});
    i.onerror = () => resolve({ok: false, error: 'error'});
    i.src = '/to-onion/' + ML.cfg.nonce;
    setTimeout(() => resolve({ok: false, error: 'timeout'}), 10000);
  });
  out.ws = await new Promise((resolve) => {
    try {
      const s = new WebSocket('ws://' + onion + '/');
      s.onopen = () => resolve({ok: true});
      s.onerror = () => resolve({ok: false, error: 'error'});
      setTimeout(() => resolve({ok: false, error: 'timeout'}), 10000);
    } catch (e) {
      resolve({ok: false, error: ML.errName(e)});
    }
  });
  out.worker = await new Promise((resolve) => {
    const src = 'fetch("http://' + onion + '/w", {mode: "no-cors"})' +
        '.then(() => postMessage({ok: true}), (e) => postMessage(' +
        '{ok: false, error: e.name}));';
    const w = new Worker(URL.createObjectURL(new Blob([src])));
    w.onmessage = (e) => resolve(e.data);
    setTimeout(() => resolve({ok: false, error: 'timeout'}), 10000);
  });
  out.shared_worker = await new Promise((resolve) => {
    try {
      const src = 'onconnect = (e) => { const p = e.ports[0]; fetch("http://' +
          onion + '/s", {mode: "no-cors"}).then(() => p.postMessage(' +
          '{ok: true}), (err) => p.postMessage({ok: false, error: ' +
          'err.name})); };';
      const w = new SharedWorker(URL.createObjectURL(new Blob([src])));
      w.port.onmessage = (e) => resolve(e.data);
      w.port.start();
    } catch (e) {
      resolve({ok: false, error: ML.errName(e)});
    }
    setTimeout(() => resolve({ok: false, error: 'timeout'}), 10000);
  });
  ML.set('requests', out);
  ML.finish();
})();
