// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Geolocation: denied at once, no prompt, no location provider.
'use strict';
const t0 = performance.now();
// A prompt (as in a regular window) never answers by itself.
const pending = setTimeout(() => {
  ML.set('geo', {code: 'pending', ms: Math.round(performance.now() - t0)});
  ML.finish();
}, 6000);
navigator.geolocation.getCurrentPosition(() => {
  clearTimeout(pending);
  ML.set('geo', {code: 0, ms: Math.round(performance.now() - t0)});
  ML.finish();
}, (e) => {
  clearTimeout(pending);
  ML.set('geo', {code: e.code, message: e.message,
    ms: Math.round(performance.now() - t0)});
  ML.finish();
}, {timeout: 10000});
