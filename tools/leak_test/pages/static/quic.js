// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// The https onion answers with Alt-Svc: h3; fetch from it again (Chromium
// would try HTTP/3 on a direct connection) and from a canary name. No UDP
// may leave.
'use strict';
(async () => {
  const n = ML.cfg.nonce;
  ML.set('first', await ML.probe('/canary/quic1-' + n, {}));
  await ML.sleep(1500);
  ML.set('second', await ML.probe('/canary/quic2-' + n, {}));
  ML.set('canary', await ML.probe('https://' + ML.host('quic') + '/'));
  ML.finish();
})();
