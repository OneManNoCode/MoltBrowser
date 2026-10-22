// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Control for the settings checks (regular window): the seeded regular-
// profile settings must be in effect here. On the JavaScript-blocked
// origin this script never runs, so data-ran stays unset.
'use strict';
document.documentElement.setAttribute('data-ran', '1');
ML.set('doNotTrack', navigator.doNotTrack);
ML.set('fontSize', getComputedStyle(document.body).fontSize);
ML.set('languages', navigator.languages);
ML.finish();
