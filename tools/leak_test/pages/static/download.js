// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// One download (several would hit the multiple-downloads prompt). In a
// MoltNet window no Safe Browsing check may be sent outside Tor.
'use strict';
setTimeout(() => {
  document.getElementById('dl').click();
  ML.set('clicked', true);
  setTimeout(() => ML.finish(), 3000);
}, 500);
