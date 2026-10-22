// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Is the test extension (id in ?ext=) visible here? Its content script
// marks <html>; its web-accessible resource; chrome.runtime.
'use strict';
(async () => {
  await ML.sleep(1500);
  const id = new URLSearchParams(location.search).get('ext') || '';
  ML.set('marked', document.documentElement.hasAttribute(
      'data-moltleak-ext'));
  ML.set('runtime', !!(self.chrome && chrome.runtime && chrome.runtime.id));
  ML.set('war', /^[a-p]{32}$/.test(id) ?
      await ML.probe('chrome-extension://' + id + '/war.txt', {}) :
      {ok: false, error: 'no id'});
  ML.finish();
})();
