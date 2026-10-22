// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// dns-prefetch / preconnect hints and speculation rules are in the markup;
// the harness then hovers both links. Script-added hints too.
'use strict';
for (const [rel, kind] of [['dns-prefetch', 'jsdnsprefetch'],
  ['preconnect', 'jspreconnect']]) {
  const l = document.createElement('link');
  l.rel = rel;
  l.href = 'https://' + ML.host(kind) + '/';
  document.head.appendChild(l);
}
setTimeout(() => {
  ML.set('loaded', true);
  ML.finish();
}, 2000);
