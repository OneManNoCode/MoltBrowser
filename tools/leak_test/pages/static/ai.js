// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Built-in AI APIs: availability, then create() with a user gesture (the
// harness calls mlCreate() with userGesture). No component or model
// download may start outside Tor.
'use strict';
const APIS = ['LanguageModel', 'Summarizer', 'Writer', 'Rewriter',
  'Translator', 'LanguageDetector', 'Proofreader'];
const OPTS = {Translator: {sourceLanguage: 'en', targetLanguage: 'de'}};
(async () => {
  const avail = {};
  for (const name of APIS) {
    const api = self[name];
    if (!api || typeof api.availability !== 'function') {
      avail[name] = 'absent';
      continue;
    }
    try {
      avail[name] = await ML.timeout(api.availability(OPTS[name] || {}),
          8000);
    } catch (e) {
      avail[name] = 'error ' + ML.errName(e);
    }
  }
  ML.set('availability', avail);
  window.mlCreate = async () => {
    const created = {};
    for (const name of APIS) {
      const api = self[name];
      if (!api || typeof api.create !== 'function') {
        continue;
      }
      try {
        const s = await ML.timeout(api.create(OPTS[name] || {}), 8000);
        created[name] = 'created';
        if (s && s.destroy) {
          s.destroy();
        }
      } catch (e) {
        created[name] = ML.errName(e);
      }
    }
    ML.set('create', created);
    window.__moltleak = {done: true, results: ML.results};
  };
  ML.finish();
})();
