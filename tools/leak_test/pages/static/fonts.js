// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Web fonts, favicons, the manifest and images from canary names and IP
// literals are in the markup; also load the fonts from script.
'use strict';
(async () => {
  for (const f of ['MLRemote', 'MLSame', 'MLIp']) {
    await ML.step('font_' + f, async () => {
      const faces = await document.fonts.load('16px ' + f);
      return faces.length;
    }, 6000);
  }
  ML.finish();
})();
