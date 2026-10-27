// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The microphone's processed samples (FINGERPRINTING.md "MoltNet: the
// microphone's processing"). The harness launches the browser with
// Chromium's fake capture device playing a fixed WAV (leakkit/micinput.py:
// 2 s of silence, a burst, 3.5 s of two voices) and its fake permission UI,
// and calls mlMicProcessing() with a user gesture.
//
// getUserMedia() with echo cancellation, noise suppression and gain control
// starts a stream, which plays the file from its start through the audio
// processing; MediaStreamTrackProcessor hands the page the processed 10 ms
// buffers as AudioData (exact samples, not farbled). The page finds the first
// buffer whose peak exceeds 0.05 (the burst, after the processed silence) and
// hashes the left channel of the 200 buffers from there (2 s). It does this
// twice, each time with a new stream, and reports whether the two agree: the
// processing starts at the file's start each time, so on one machine they
// must, and between machines MoltNet's must too (leakkit/expectations.py).
'use strict';

async function captureOnce() {
  const stream = await navigator.mediaDevices.getUserMedia({audio: {
    echoCancellation: true, noiseSuppression: true, autoGainControl: true}});
  const track = stream.getAudioTracks()[0];
  const settings = track.getSettings();
  const reader = new MediaStreamTrackProcessor({track}).readable.getReader();
  const buffers = [];
  let onset = -1;
  const deadline = performance.now() + 12000;
  try {
    while (performance.now() < deadline) {
      const {value, done} = await ML.timeout(reader.read(), 3000);
      if (done) {
        break;
      }
      const left = new Float32Array(value.numberOfFrames);
      value.copyTo(left, {planeIndex: 0, format: 'f32-planar'});
      const ts = value.timestamp;
      value.close();
      if (onset < 0) {
        let peak = 0;
        for (const x of left) {
          peak = Math.max(peak, Math.abs(x));
        }
        if (peak <= 0.05) {
          continue;
        }
        onset = buffers.length;
      }
      buffers.push({left, ts});
      if (buffers.length >= 200) {
        break;
      }
    }
  } finally {
    reader.cancel();
    track.stop();
  }
  if (onset < 0) {
    return {error: 'silent', settings};
  }
  if (buffers.length < 200) {
    return {error: 'short', settings};
  }
  // A gap in the timestamps (a dropped buffer) makes the hash meaningless.
  for (let i = 1; i < buffers.length; ++i) {
    const step = buffers[i].ts - buffers[i - 1].ts;
    if (step > 15000) {
      return {error: 'gap', settings};
    }
  }
  let size = 0;
  for (const b of buffers) {
    size += b.left.length;
  }
  const all = new Float32Array(size);
  let at = 0;
  for (const b of buffers) {
    all.set(b.left, at);
    at += b.left.length;
  }
  const h = await crypto.subtle.digest('SHA-256', all.buffer);
  const hash = Array.from(new Uint8Array(h)).slice(0, 12)
      .map((b) => b.toString(16).padStart(2, '0')).join('');
  return {hash, frames: buffers[0].left.length, settings};
}

async function micProcessing() {
  if (!window.MediaStreamTrackProcessor) {
    return {'audio.mic_processed': 'absent',
      'audio.mic_processed_stable': 'absent'};
  }
  const first = await captureOnce();
  await ML.sleep(500);
  const second = await captureOnce();
  const s = first.settings || {};
  const value = first.error ? first.error :
      {hash: first.hash, frames: first.frames};
  return {
    'audio.mic_processed': value,
    'audio.mic_processed_stable': !first.error && !second.error &&
        first.hash === second.hash,
    'audio.mic_settings': {echoCancellation: s.echoCancellation,
      noiseSuppression: s.noiseSuppression,
      autoGainControl: s.autoGainControl},
    'audio.mic_sample_rate': s.sampleRate === undefined ? null :
        s.sampleRate,
  };
}

window.mlMicProcessing = () => micProcessing().catch((e) => ({
  'audio.mic_processed': ML.errInfo(e),
  'audio.mic_processed_stable': ML.errInfo(e)}));
