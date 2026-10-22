// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Shared worker of the leak kit's workers test (templated per test).
'use strict';
async function probes() {
  const out = {};
  for (const [k, url] of [
    ['canary', 'https://shared-{{NONCE}}.{{CANARY}}/'],
    ['same', '/canary/shared-{{NONCE}}'],
    ['ip', 'http://{{CIP4}}:8080/shared']]) {
    try {
      const r = await fetch(url, {mode: 'no-cors'});
      out[k] = 'ok ' + r.type;
    } catch (err) {
      out[k] = 'error ' + err.name;
    }
  }
  try {
    new WebSocket('wss://sharedws-{{NONCE}}.{{CANARY}}/');
    out.ws = 'created';
  } catch (err) {
    out.ws = 'error ' + err.name;
  }
  return out;
}
onconnect = (e) => {
  const port = e.ports[0];
  probes().then((r) => port.postMessage(r));
};
