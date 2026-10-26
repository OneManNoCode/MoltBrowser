// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// localStorage writes: in MoltNet a site's writes come out of a write
// allowance (1000 at once, 100 a second after that); a write past it fails
// and the page's value reverts when the storage service answers. A regular
// window keeps every write. (MOLTNET_COMPLIANCE.md §2, "DOM storage
// totals".)
'use strict';
(async () => {
  const kSent = 3000;
  const kBurst = 1000;
  const kPerSecond = 100;
  const prefix = 'ml-' + ML.cfg.nonce + '-';
  try {
    const t0 = performance.now();
    for (let i = 0; i < kSent; ++i) {
      localStorage.setItem(prefix + i, 'v');
    }
    const seconds = (performance.now() - t0) / 1000;
    // Refused writes revert as their answers arrive: wait until the count
    // holds still.
    const count = () => {
      let n = 0;
      for (let i = 0; i < kSent; ++i) {
        if (localStorage.getItem(prefix + i) !== null) {
          ++n;
        }
      }
      return n;
    };
    let kept = -1;
    for (let tries = 0; tries < 24; ++tries) {
      await ML.sleep(250);
      const now = count();
      if (now === kept) {
        break;
      }
      kept = now;
    }
    ML.set('writes', {sent: kSent, kept, seconds: Math.round(seconds * 1000) /
        1000});
    // Within the allowance: the burst, plus what refilled while the loop
    // ran, plus a margin for writes the site made in earlier tests.
    ML.set('within_rate',
        kept <= kBurst + Math.ceil(kPerSecond * seconds) + 50);
    ML.set('burst_fits', kept >= kBurst - 100);
    for (let i = 0; i < kSent; ++i) {
      localStorage.removeItem(prefix + i);
    }
  } catch (e) {
    ML.set('writes', ML.errInfo(e));
  }
  ML.finish();
})();
