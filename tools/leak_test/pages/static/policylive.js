// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The policy_live phase's page. Its forms are what Autofill would ask its
// server about. window.mlkCanaryLoop() fetches the page's canary name over
// and over: through Tor while MoltNet is open, and never from outside it,
// also while a proxy policy arrives (the kit's resolver would log such a
// lookup).
'use strict';
(() => {
  let sent = 0;
  window.mlkCanaryLoop = (ms) => {
    const url = 'http://' + ML.host('plc') + '/x';
    setInterval(() => {
      sent++;
      fetch(url + '?n=' + sent, {mode: 'no-cors', cache: 'no-store'})
          .catch(() => {});
    }, ms || 250);
    return url;
  };
  window.mlkCanarySent = () => sent;
  ML.set('forms', document.forms.length);
  ML.finish();
})();
