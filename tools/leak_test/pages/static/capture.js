// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// Screen, window and tab capture in a MoltNet window: refused as a user's
// Cancel in the picker (NotAllowedError "Permission denied by user") at
// once, with no picker, and the display-capture permission reads "denied"
// in the page and in a worker. The harness calls mlCapture() with a user
// gesture. A picker that opened would leave the call waiting: it then ends
// as a timeout after 8 s.
'use strict';
(async () => {
  const md = navigator.mediaDevices;
  ML.set('api', {
    getDisplayMedia: !!(md && md.getDisplayMedia),
    displaySurface: !!(md && md.getSupportedConstraints().displaySurface),
  });
  await ML.step('perm', () => navigator.permissions.query(
      {name: 'display-capture'}).then((s) => s.state));
  await ML.step('worker_perm', () => new Promise((resolve) => {
    const src = 'navigator.permissions.query({name: "display-capture"})' +
        '.then((s) => postMessage(s.state), ' +
        '(e) => postMessage("error " + e.name));';
    const w = new Worker(URL.createObjectURL(new Blob([src])));
    w.onmessage = (e) => {
      resolve(e.data);
      w.terminate();
    };
    w.onerror = () => resolve('error');
  }));
  const attempt = async (key, call) => {
    const t0 = performance.now();
    try {
      const stream = await ML.timeout(call(), 8000);
      stream.getTracks().forEach((t) => t.stop());
      ML.set(key, {resolved: true, ms: Math.round(performance.now() - t0)});
    } catch (e) {
      ML.set(key, Object.assign(ML.errInfo(e),
          {ms: Math.round(performance.now() - t0)}));
    }
  };
  window.mlCapture = async () => {
    if (!md || !md.getDisplayMedia) {
      ML.set('display_media', {error: 'absent'});
    } else {
      await attempt('display_media', () => md.getDisplayMedia(
          {video: true, audio: true}));
      await attempt('this_tab', () => md.getDisplayMedia(
          {video: true, preferCurrentTab: true}));
    }
    // The legacy chromeMediaSource form, recorded only.
    await attempt('gum_screen', () => md.getUserMedia(
        {video: {mandatory: {chromeMediaSource: 'screen'}}}));
    window.__moltleak = {done: true, results: ML.results};
  };
  ML.finish();
})();
