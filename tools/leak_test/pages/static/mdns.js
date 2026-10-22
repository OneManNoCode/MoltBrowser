// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// .local names (multicast DNS on the local network).
'use strict';
(async () => {
  const name = 'moltleak-' + ML.cfg.nonce + '.local';
  ML.set('fetch_local', await ML.probe('http://' + name + '/'));
  ML.set('fetch_uuid', await ML.probe('http://' + crypto.randomUUID() +
      '.local/'));
  const img = new Image();
  img.src = 'http://img-' + name + '/x.png';
  await ML.sleep(1500);
  ML.finish();
})();
