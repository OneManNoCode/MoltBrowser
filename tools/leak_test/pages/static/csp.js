// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Violations that send CSP reports (report-uri: canary + same origin;
// report-to), a report-only connect-src violation; NEL and Report-To are
// response headers. Reporting-API reports are batched (up to a minute), so
// the harness keeps capturing until the end of the run.
'use strict';
(async () => {
  const img = document.createElement('img');
  img.src = 'https://' + ML.host('cspimg') + '/x.png';
  document.body.appendChild(img);
  try {
    // eslint-disable-next-line no-eval
    eval('1');
    ML.set('eval', 'allowed');
  } catch (e) {
    ML.set('eval', ML.errName(e));
  }
  ML.set('connect', await ML.probe('https://' + ML.host('cspconnect') + '/'));
  await ML.sleep(2000);
  ML.finish();
})();
