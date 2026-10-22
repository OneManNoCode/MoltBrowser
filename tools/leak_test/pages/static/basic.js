// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// A page that loads: proves the route works (and, for http:// onions, that
// HTTPS-Only didn't interpose).
'use strict';
ML.set('loaded', true);
ML.set('secure_context', window.isSecureContext);
ML.set('protocol', location.protocol);
ML.finish();
