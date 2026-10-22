// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// The page's markup does the work (resource hints, speculation rules); give
// the browser a moment to act on them.
'use strict';
setTimeout(() => {
  ML.set('loaded', true);
  ML.finish();
}, 2500);
