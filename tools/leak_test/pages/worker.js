// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Dedicated worker of the leak kit's workers test (templated per test).
'use strict';
async function probes() {
  const out = {};
  for (const [k, url] of [
    ['canary', 'https://worker-{{NONCE}}.{{CANARY}}/'],
    ['same', '/canary/worker-{{NONCE}}'],
    ['ip', 'http://{{CIP4}}:8080/worker']]) {
    try {
      const r = await fetch(url, {mode: 'no-cors'});
      out[k] = 'ok ' + r.type;
    } catch (err) {
      out[k] = 'error ' + err.name;
    }
  }
  try {
    new WebSocket('wss://workerws-{{NONCE}}.{{CANARY}}/');
    out.ws = 'created';
  } catch (err) {
    out.ws = 'error ' + err.name;
  }
  return out;
}
probes().then((r) => postMessage(r));
