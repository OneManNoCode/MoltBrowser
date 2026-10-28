// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The camera's values (FINGERPRINTING.md "MoltNet: cameras, microphones and
// outputs"), in the mic_processing launch: Chromium's fake capture device
// (six sizes from 96x96 to 3840x2160, at 20 fps) and its fake permission
// UI, which grants the page the camera without a prompt. The harness calls
// mlCameraValues() with a user gesture after the microphone's cases.
//
// A MoltNet page's camera shows the formats of one menu that it reaches
// (640x480 and 1280x720 at 30 fps), never the device's own modes; the fake
// device reaches both. The page reports, for a track with no constraints
// and for one that asks for Full HD, getSettings() (width, height,
// frameRate, aspectRatio, resizeMode, once frames arrive) and
// getCapabilities()' ranges; the ranges of the cameras' InputDeviceInfo.
// getCapabilities(); the first frame MediaStreamTrackProcessor hands the
// page (its coded size, visible rectangle, display size and format: the
// track's size exactly, I420); and what a request for a width the menu
// doesn't have gives. In MoltNet all the same on every device and OS
// (leakkit/expectations.py).
'use strict';

// A range of getCapabilities() as [min, max].
function cameraRange(r) {
  return r && typeof r === 'object' ? [r.min, r.max] : null;
}

function cameraCapabilities(capabilities) {
  const c = capabilities || {};
  return {width: cameraRange(c.width), height: cameraRange(c.height),
    frameRate: cameraRange(c.frameRate),
    aspectRatio: cameraRange(c.aspectRatio),
    resizeMode: Array.isArray(c.resizeMode) ?
        c.resizeMode.map((x) => String(x)).sort() : null};
}

function cameraSettings(settings) {
  const s = settings || {};
  const value = (x) => (x === undefined ? null : x);
  return {width: value(s.width), height: value(s.height),
    frameRate: value(s.frameRate), aspectRatio: value(s.aspectRatio),
    resizeMode: value(s.resizeMode)};
}

// The first frame of `track`, as a VideoFrame shows it.
async function firstCameraFrame(track) {
  if (!window.MediaStreamTrackProcessor) {
    return 'absent';
  }
  const reader = new MediaStreamTrackProcessor({track}).readable.getReader();
  try {
    const {value, done} = await ML.timeout(reader.read(), 5000);
    if (done) {
      return 'done';
    }
    const r = value.visibleRect;
    const frame = {codedWidth: value.codedWidth,
      codedHeight: value.codedHeight,
      visibleRect: r ? [r.x, r.y, r.width, r.height] : null,
      displayWidth: value.displayWidth, displayHeight: value.displayHeight,
      format: value.format};
    value.close();
    return frame;
  } finally {
    reader.cancel();
  }
}

// A camera track with `constraints`: its capabilities, its first frame and
// its settings once that frame arrived.
async function cameraTrack(constraints) {
  const stream = await navigator.mediaDevices.getUserMedia(
      {video: constraints});
  const track = stream.getVideoTracks()[0];
  try {
    const capabilities = cameraCapabilities(
        track.getCapabilities ? track.getCapabilities() : {});
    const frame = await firstCameraFrame(track);
    return {settings: cameraSettings(track.getSettings()), capabilities,
      frame};
  } finally {
    track.stop();
  }
}

// What a request for more than the menu has gives: the error's name and
// constraint, or 'granted'.
async function cameraOverconstrained() {
  try {
    const stream = await navigator.mediaDevices.getUserMedia(
        {video: {width: {min: 1281}}});
    stream.getTracks().forEach((t) => t.stop());
    return 'granted';
  } catch (e) {
    return {name: e.name,
      constraint: e.constraint === undefined ? null : e.constraint};
  }
}

// The distinct ranges of the cameras' InputDeviceInfo.getCapabilities().
async function cameraDeviceCapabilities() {
  const devices = await navigator.mediaDevices.enumerateDevices();
  const ranges = new Set();
  for (const device of devices) {
    if (device.kind === 'videoinput' && device.label &&
        device.getCapabilities) {
      ranges.add(JSON.stringify(cameraCapabilities(
          device.getCapabilities())));
    }
  }
  return [...ranges].sort().map((r) => JSON.parse(r));
}

async function cameraValues() {
  const plain = await cameraTrack(true);
  const hd = await cameraTrack({width: {ideal: 1920}, height: {ideal: 1080}});
  return {
    'video.camera_settings': plain.settings,
    'video.camera_capabilities': plain.capabilities,
    'video.camera_frame': plain.frame,
    'video.camera_hd_settings': hd.settings,
    'video.camera_hd_frame': hd.frame,
    'video.camera_device_capabilities': await cameraDeviceCapabilities(),
    'video.camera_overconstrained': await cameraOverconstrained(),
  };
}

window.mlCameraValues = () => cameraValues().catch((e) => ({
  'video.camera_settings': ML.errInfo(e)}));
