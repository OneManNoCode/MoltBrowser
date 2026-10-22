// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Shared helpers for the leak kit's test pages. A page reports through
// window.__moltleak = {done, results}, which the harness polls over the
// DevTools pipe. Every probe is caught: a failing API is a result, never
// an exception that stops the page.
'use strict';
(function() {
  const d = document.documentElement.dataset;
  const results = {};
  window.__moltleak = {done: false, results};
  const ML = {
    cfg: d,
    results,
    host(kind) {
      return kind + '-' + d.nonce + '.' + d.canary;
    },
    set(key, value) {
      results[key] = value;
    },
    errName(e) {
      return (e && e.name) ? e.name : String(e);
    },
    errInfo(e) {
      return {error: ML.errName(e), message: String((e && e.message) || '')
          .slice(0, 200)};
    },
    timeout(promise, ms) {
      return Promise.race([promise, new Promise((resolve, reject) =>
          setTimeout(() => reject(new Error('timeout')), ms))]);
    },
    async step(key, fn, ms) {
      try {
        results[key] = await ML.timeout(Promise.resolve().then(fn),
            ms || 8000);
      } catch (e) {
        results[key] = ML.errInfo(e);
      }
    },
    // fetch() that reports how it ended, never throws.
    async probe(url, init, ms) {
      try {
        const r = await ML.timeout(fetch(url, init || {mode: 'no-cors'}),
            ms || 8000);
        return {ok: true, type: r.type, status: r.status};
      } catch (e) {
        return {ok: false, error: ML.errName(e)};
      }
    },
    sleep(ms) {
      return new Promise((r) => setTimeout(r, ms));
    },
    finish() {
      window.__moltleak = {done: true, results};
      document.title = 'done';
    },
  };
  window.ML = ML;
})();
