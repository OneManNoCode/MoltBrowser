// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// ?set=1 sets a MoltNet cookie; both read the cookies and a canvas readback
// (farbled per site and per session, so it changes with New Identity).
'use strict';
(async () => {
  if (location.search.includes('set=1')) {
    document.cookie = 'mlk_moltnet=' + ML.cfg.nonce + '; max-age=3600; path=/';
    try {
      localStorage.setItem('mlk', '1');
    } catch (e) {
      // storage may be unavailable
    }
  }
  ML.set('cookie', document.cookie);
  ML.set('canvas', await canvasHash());
  ML.finish();
})();
async function canvasHash() {
  try {
    const c = document.createElement('canvas');
    c.width = 220;
    c.height = 40;
    const g = c.getContext('2d');
    g.fillStyle = '#f60';
    g.fillRect(10, 5, 100, 30);
    g.fillStyle = 'rgba(30, 120, 200, 0.7)';
    g.font = '16px sans-serif';
    g.fillText('moltleak \u{1F9C5} 0123', 4, 25);
    const data = new TextEncoder().encode(c.toDataURL());
    const h = await crypto.subtle.digest('SHA-256', data);
    return Array.from(new Uint8Array(h)).slice(0, 12)
        .map((b) => b.toString(16).padStart(2, '0')).join('');
  } catch (e) {
    return 'error ' + ML.errName(e);
  }
}
