// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// WebTransport is HTTP/3 over UDP: it must refuse to go anywhere from a
// proxied (MoltNet) context, without looking the name up.
'use strict';
async function wt(url) {
  if (typeof WebTransport === 'undefined') {
    return {ok: false, error: 'absent'};
  }
  try {
    const t = new WebTransport(url);
    await ML.timeout(t.ready, 8000);
    t.close();
    return {ok: true};
  } catch (e) {
    return {ok: false, error: ML.errName(e)};
  }
}
(async () => {
  ML.set('wt_canary', await wt('https://' + ML.host('wt') + ':4433/'));
  ML.set('wt_ip', await wt('https://' + ML.cfg.cip4 + ':4433/'));
  ML.finish();
})();
