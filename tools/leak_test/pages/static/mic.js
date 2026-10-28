// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The microphone's processed samples (FINGERPRINTING.md "MoltNet: the
// microphone's processing"). The harness launches the browser with
// Chromium's fake capture device playing a fixed WAV (leakkit/micinput.py:
// 2 s of silence, a burst, 3.5 s of two voices) and its fake permission UI,
// and calls mlMicProcessing(), then mlMicPlayback(), each with a user
// gesture.
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
// With the permission it also reports the first track's format
// (getSettings()' sampleRate, sampleSize, channelCount and latency) and
// getCapabilities()' ranges, the ranges enumerateDevices()' InputDeviceInfo
// gives for the microphones, and whether every device label is generic and
// every device a group of its own: in MoltNet the same on every device and
// OS.
//
// The other cases, each with a new stream:
//   * system echo cancellation (echoCancellation "all"), which MoltNet
//     answers with WebRTC's on every OS: the same samples as above;
//   * while the page plays two tones (a 48 kHz and a 44.1 kHz context, which
//     the audio service resamples and mixes for the output device):
//     echoCancellation "remote-only", processed in the renderer, whose echo
//     canceller hears only WebRTC's playout (none in MoltNet): the same
//     samples as above again; and the default echo cancellation, which hears
//     the tones as its reference: recorded only, since the echo canceller
//     adapts to the timing between playout and capture, which changes from
//     run to run on any machine.
'use strict';

const kProcessing = {noiseSuppression: true, autoGainControl: true};

function sorted(list) {
  if (!Array.isArray(list)) {
    return null;
  }
  return list.map((x) => String(x)).sort();
}

// The processing capabilities of a microphone track, in a fixed order.
function processingCapabilities(track) {
  const c = track.getCapabilities ? track.getCapabilities() : {};
  return {
    echoCancellation: sorted(c.echoCancellation),
    autoGainControl: sorted(c.autoGainControl),
    noiseSuppression: sorted(c.noiseSuppression),
    voiceIsolation: sorted(c.voiceIsolation),
  };
}

function processingSettings(settings) {
  const s = settings || {};
  return {echoCancellation: s.echoCancellation,
    noiseSuppression: s.noiseSuppression,
    autoGainControl: s.autoGainControl};
}

// The format a microphone track reports (FINGERPRINTING.md "MoltNet:
// cameras, microphones and outputs": in MoltNet one format for every
// microphone, whatever the device's).
function formatSettings(settings) {
  const s = settings || {};
  const value = (x) => (x === undefined ? null : x);
  return {sampleRate: value(s.sampleRate), sampleSize: value(s.sampleSize),
    channelCount: value(s.channelCount), latency: value(s.latency)};
}

// The format ranges of getCapabilities(), a track's or an InputDeviceInfo's,
// as [min, max].
function formatRanges(capabilities) {
  const c = capabilities || {};
  const range = (r) => (r && typeof r === 'object' ? [r.min, r.max] : null);
  return {sampleRate: range(c.sampleRate), sampleSize: range(c.sampleSize),
    channelCount: range(c.channelCount), latency: range(c.latency)};
}

// MoltNet's label for a device: "Default", or the kind and the ID's first
// four characters.
const kDeviceKinds = {audioinput: 'Microphone', videoinput: 'Camera',
  audiooutput: 'Speaker'};
function genericLabel(kind, id) {
  if (id === 'default') {
    return 'Default';
  }
  if (id === 'communications') {
    return 'Communications';
  }
  return kDeviceKinds[kind] + ' ' + id.slice(0, 4);
}

// After the permission: whether every label enumerateDevices() shows, and the
// microphone track's, is generic, whether every device is a group of its
// own, whether Windows' communications device is listed, and the distinct
// format ranges of the microphones' InputDeviceInfo.getCapabilities().
async function deviceNames(track) {
  const devices = await navigator.mediaDevices.enumerateDevices();
  let labels = true;
  let groups = true;
  let communications = false;
  const ranges = new Set();
  for (const device of devices) {
    communications = communications || device.deviceId === 'communications';
    if (!device.label) {
      continue;
    }
    labels = labels && device.label === genericLabel(device.kind,
        device.deviceId);
    groups = groups && device.groupId === device.deviceId;
    if (device.kind === 'audioinput' && device.getCapabilities) {
      ranges.add(JSON.stringify(formatRanges(device.getCapabilities())));
    }
  }
  const id = String(track.deviceId || '');
  return {
    names: {labels, groups, communications,
      track: track.label === genericLabel('audioinput', id) &&
          track.groupId === id && track.capabilitiesGroupId === id},
    ranges: [...ranges].sort().map((r) => JSON.parse(r)),
  };
}

async function captureOnce(echoCancellation) {
  const stream = await navigator.mediaDevices.getUserMedia({audio: Object.assign(
      {echoCancellation}, kProcessing)});
  const track = stream.getAudioTracks()[0];
  const settings = track.getSettings();
  const capabilities = processingCapabilities(track);
  const allCapabilities = track.getCapabilities ? track.getCapabilities() : {};
  const device = {label: track.label, deviceId: settings.deviceId,
    groupId: settings.groupId, capabilitiesGroupId: allCapabilities.groupId,
    format: formatSettings(settings),
    ranges: formatRanges(allCapabilities)};
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
    return {error: 'silent', settings, capabilities, device};
  }
  if (buffers.length < 200) {
    return {error: 'short', settings, capabilities, device};
  }
  // A gap in the timestamps (a dropped buffer) makes the hash meaningless.
  for (let i = 1; i < buffers.length; ++i) {
    const step = buffers[i].ts - buffers[i - 1].ts;
    if (step > 15000) {
      return {error: 'gap', settings, capabilities, device};
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
  return {hash, frames: buffers[0].left.length, settings, capabilities,
    device};
}

// A capture that reports how getUserMedia() or the reading failed instead of
// throwing, so that one case failing leaves the others' values.
async function tryCapture(echoCancellation) {
  try {
    return await captureOnce(echoCancellation);
  } catch (e) {
    return {failed: ML.errInfo(e)};
  }
}

// What a capture reports as a value: its hash, or why there is none.
function hashOf(capture) {
  if (capture.failed) {
    return capture.failed;
  }
  return capture.error ? capture.error :
      {hash: capture.hash, frames: capture.frames};
}

async function micProcessing() {
  if (!window.MediaStreamTrackProcessor) {
    return {'audio.mic_processed': 'absent',
      'audio.mic_processed_stable': 'absent'};
  }
  const first = await captureOnce(true);
  await ML.sleep(500);
  const second = await captureOnce(true);
  await ML.sleep(500);
  const systemAec = await tryCapture('all');
  let names;
  try {
    names = await deviceNames(first.device);
  } catch (e) {
    names = {names: ML.errInfo(e), ranges: ML.errInfo(e)};
  }
  return {
    'audio.mic_processed': hashOf(first),
    'audio.mic_processed_stable': !first.error && !second.error &&
        first.hash === second.hash,
    'audio.mic_settings': processingSettings(first.settings),
    'audio.mic_capabilities': first.capabilities,
    'audio.mic_format': first.device.format,
    'audio.mic_format_ranges': first.device.ranges,
    'audio.mic_device_ranges': names.ranges,
    'audio.mic_names': names.names,
    'audio.mic_system_aec': hashOf(systemAec),
    'audio.mic_system_aec_settings': processingSettings(systemAec.settings),
  };
}

// A looped second of three harmonics of `inc`'s pitch, from integer phases
// (the same samples on every machine), written with copyToChannel().
function toneBuffer(ctx, inc, gain) {
  const frames = ctx.sampleRate;
  const left = new Float32Array(frames);
  const right = new Float32Array(frames);
  const tri = (p) => {
    const v = p >>> 8;  // 24 bits
    const t = v < (1 << 23) ? v : (1 << 24) - v;
    return (t - (1 << 22)) / 4194304;
  };
  let p1 = 0;
  let p2 = 0;
  let p3 = 0;
  for (let i = 0; i < frames; ++i) {
    p1 = (p1 + inc) >>> 0;
    p2 = (p2 + 2 * inc) >>> 0;
    p3 = (p3 + 3 * inc) >>> 0;
    const x = tri(p1) + tri(p2) / 2 + tri(p3) / 3;
    left[i] = gain * x;
    right[i] = gain * 0.8 * x;
  }
  const buffer = ctx.createBuffer(2, frames, ctx.sampleRate);
  buffer.copyToChannel(left, 0);
  buffer.copyToChannel(right, 1);
  return buffer;
}

async function startTones() {
  const contexts = [new AudioContext(), new AudioContext({sampleRate: 44100})];
  const incs = [11000000, 17000000];
  for (let i = 0; i < contexts.length; ++i) {
    const ctx = contexts[i];
    const source = ctx.createBufferSource();
    source.buffer = toneBuffer(ctx, incs[i], 0.2);
    source.loop = true;
    source.connect(ctx.destination);
    source.start();
    await ML.timeout(ctx.resume(), 3000);
  }
  // Let the output streams start before the microphone does.
  await ML.sleep(300);
  return {
    running: contexts.map((c) => c.state),
    rates: contexts.map((c) => c.sampleRate),
    stop: () => Promise.all(contexts.map((c) => c.close())),
  };
}

async function micPlayback() {
  if (!window.MediaStreamTrackProcessor) {
    return {'audio.mic_remote_only': 'absent'};
  }
  const tones = await startTones();
  try {
    const remoteOnly = await tryCapture('remote-only');
    await ML.sleep(500);
    const playback = await tryCapture(true);
    return {
      'audio.mic_playback_tones': {state: tones.running, rates: tones.rates},
      'audio.mic_remote_only': hashOf(remoteOnly),
      'audio.mic_remote_only_settings': processingSettings(
          remoteOnly.settings),
      'audio.mic_playback': hashOf(playback),
    };
  } finally {
    await tones.stop();
  }
}

window.mlMicProcessing = () => micProcessing().catch((e) => ({
  'audio.mic_processed': ML.errInfo(e),
  'audio.mic_processed_stable': ML.errInfo(e)}));
window.mlMicPlayback = () => micPlayback().catch((e) => ({
  'audio.mic_remote_only': ML.errInfo(e)}));
