// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Third-party frame: can it keep an unpartitioned cookie?
'use strict';
document.cookie = 'mlk_3p=1; SameSite=None; Secure; path=/';
parent.postMessage({cookie: document.cookie.includes('mlk_3p=1')}, '*');
