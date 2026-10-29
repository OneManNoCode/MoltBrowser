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
// doesn't have gives. Then, for a track with no constraints: its
// ImageCapture's answers (photo capabilities and settings, the image capture
// members of the track's capabilities and settings, a required zoom, the
// sizes of takePhoto() and grabFrame()); its frames over about a second
// (whether every timestamp lies on MoltNet's grid of 30 a second, the steps
// between them, their colour space); requestVideoFrameCallback()'s metadata
// for it in a <video>; and the statistics of a clone at 15 fps. In MoltNet
// all the same on every device and OS (leakkit/expectations.py).
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

// The members ImageCapture adds to a camera track's capabilities and
// settings, each a control of the device.
const IMAGE_CAPTURE_MEMBERS = [
  'whiteBalanceMode', 'exposureMode', 'focusMode', 'pointsOfInterest',
  'exposureCompensation', 'exposureTime', 'colorTemperature', 'iso',
  'brightness', 'contrast', 'saturation', 'sharpness', 'focusDistance',
  'pan', 'tilt', 'zoom', 'torch', 'backgroundBlur',
  'backgroundSegmentationMask', 'eyeGazeCorrection', 'faceFraming'];

// A photo capability's range as [min, max, step].
function photoRange(r) {
  return r && typeof r === 'object' ? [r.min, r.max, r.step] : null;
}

// What a value of a step is, or how it failed.
async function cameraStep(fn, ms) {
  try {
    return await ML.timeout(Promise.resolve().then(fn), ms || 8000);
  } catch (e) {
    return {name: ML.errName(e),
      constraint: e && e.constraint !== undefined ? e.constraint : null};
  }
}

// ImageCapture's answers for `track`.
async function cameraImageCapture(track) {
  if (!window.ImageCapture) {
    return 'absent';
  }
  const capture = new ImageCapture(track);
  const result = {};
  result.photo_capabilities = await cameraStep(async () => {
    const c = await capture.getPhotoCapabilities();
    return {redEyeReduction: c.redEyeReduction === undefined ? null :
        c.redEyeReduction,
    imageWidth: photoRange(c.imageWidth),
    imageHeight: photoRange(c.imageHeight),
    fillLightMode: Array.isArray(c.fillLightMode) ?
        c.fillLightMode.map((x) => String(x)).sort() : null};
  });
  result.photo_settings = await cameraStep(async () => {
    const s = await capture.getPhotoSettings();
    return {imageWidth: s.imageWidth === undefined ? null : s.imageWidth,
      imageHeight: s.imageHeight === undefined ? null : s.imageHeight};
  });
  // The track's capabilities and settings once ImageCapture answered.
  const capabilities = track.getCapabilities ? track.getCapabilities() : {};
  const settings = track.getSettings();
  result.track_members = {
    capabilities: IMAGE_CAPTURE_MEMBERS.filter((m) => m in capabilities),
    settings: IMAGE_CAPTURE_MEMBERS.filter((m) => m in settings)};
  // A zoom the page requires, which a camera without one can't meet.
  result.required_zoom = await cameraStep(async () => {
    await track.applyConstraints({zoom: {exact: 2}});
    return 'applied';
  });
  result.photo = await cameraStep(async () => {
    const blob = await capture.takePhoto();
    const bitmap = await createImageBitmap(blob);
    const photo = {type: blob.type, width: bitmap.width,
      height: bitmap.height};
    bitmap.close();
    return photo;
  });
  result.grab_frame = await cameraStep(async () => {
    const bitmap = await capture.grabFrame();
    const frame = {width: bitmap.width, height: bitmap.height};
    bitmap.close();
    return frame;
  });
  return result;
}

// MoltNet's grid: tick n is n/30 s after the first, in whole microseconds.
function gridTick(us) {
  return Math.round(us * 30 / 1e6);
}

function onGrid(us) {
  return Math.floor(gridTick(us) * 1e6 / 30) === us;
}

// `track`'s frames over about a second: whether every timestamp lies on the
// grid, the steps between frames in ticks, and the first frame's colour
// space.
async function cameraTiming(track) {
  if (!window.MediaStreamTrackProcessor) {
    return {on_grid: 'absent', steps: 'absent', color_space: 'absent'};
  }
  const reader = new MediaStreamTrackProcessor({track}).readable.getReader();
  const timestamps = [];
  let colorSpace = null;
  const end = performance.now() + 1000;
  try {
    while (performance.now() < end) {
      const {value, done} = await ML.timeout(reader.read(), 3000);
      if (done) {
        break;
      }
      timestamps.push(value.timestamp);
      if (!colorSpace) {
        const c = value.colorSpace;
        colorSpace = {primaries: c.primaries, transfer: c.transfer,
          matrix: c.matrix, fullRange: c.fullRange};
      }
      value.close();
    }
  } finally {
    reader.cancel();
  }
  const steps = new Set();
  for (let i = 1; i < timestamps.length; ++i) {
    steps.add(gridTick(timestamps[i]) - gridTick(timestamps[i - 1]));
  }
  return {on_grid: timestamps.length > 1 && timestamps.every(onGrid),
    steps: [...steps].sort((a, b) => a - b), color_space: colorSpace};
}

// requestVideoFrameCallback()'s metadata for `stream` in a <video>: its
// members, whether mediaTime lies on the grid, whether captureTime keeps
// mediaTime's intervals (to the 0.1 ms of a page's clock), and the size.
async function cameraFrameCallbacks(stream) {
  const video = document.createElement('video');
  if (!video.requestVideoFrameCallback) {
    return 'absent';
  }
  video.muted = true;
  video.playsInline = true;
  video.srcObject = stream;
  document.body.appendChild(video);
  try {
    await ML.timeout(video.play(), 5000);
    const metadata = [];
    await ML.timeout(new Promise((resolve) => {
      const onFrame = (now, m) => {
        metadata.push(m);
        if (metadata.length >= 6) {
          resolve();
        } else {
          video.requestVideoFrameCallback(onFrame);
        }
      };
      video.requestVideoFrameCallback(onFrame);
    }), 5000);
    const keys = new Set();
    for (const m of metadata) {
      for (const k of Object.keys(m)) {
        keys.add(k);
      }
    }
    let follows = true;
    for (let i = 1; i < metadata.length; ++i) {
      const capture = metadata[i].captureTime - metadata[0].captureTime;
      const media = (metadata[i].mediaTime - metadata[0].mediaTime) * 1000;
      if (!(Math.abs(capture - media) <= 0.25)) {
        follows = false;
      }
    }
    return {keys: [...keys].sort(),
      media_time_on_grid: metadata.every(
          (m) => onGrid(Math.round(m.mediaTime * 1e6))),
      capture_time_follows: follows,
      size: [metadata[0].width, metadata[0].height]};
  } finally {
    video.pause();
    video.srcObject = null;
    video.remove();
  }
}

// MediaStreamTrack.stats of a clone of `track` at 15 fps over a second:
// frames neither delivered nor discarded (dropped), and whether it discarded
// as many as it delivered, as a 30 fps camera's 15 fps track does.
async function cameraStats(track) {
  const clone = track.clone();
  try {
    if (!clone.stats) {
      return 'absent';
    }
    await clone.applyConstraints({frameRate: 15});
    await ML.sleep(300);
    const before = clone.stats.toJSON();
    await ML.sleep(1000);
    const after = clone.stats.toJSON();
    const delivered = after.deliveredFrames - before.deliveredFrames;
    const discarded = after.discardedFrames - before.discardedFrames;
    const total = after.totalFrames - before.totalFrames;
    return {dropped: total - delivered - discarded,
      half_discarded: delivered > 0 && Math.abs(delivered - discarded) <= 2};
  } finally {
    clone.stop();
  }
}

// The answers and timing of a track with no constraints.
async function cameraBehaviour() {
  const stream = await navigator.mediaDevices.getUserMedia({video: true});
  const track = stream.getVideoTracks()[0];
  try {
    const imageCapture = await cameraStep(() => cameraImageCapture(track),
        30000);
    const timing = await cameraStep(() => cameraTiming(track), 8000);
    const shown = track.clone();
    const callbacks = await cameraStep(
        () => cameraFrameCallbacks(new MediaStream([shown])), 10000);
    shown.stop();
    const stats = await cameraStep(() => cameraStats(track), 8000);
    return {
      'video.camera_image_capture': imageCapture,
      'video.camera_timing_on_grid': timing.on_grid === undefined ? timing :
          timing.on_grid,
      'video.camera_timing_steps': timing.steps === undefined ? timing :
          timing.steps,
      'video.camera_color_space': timing.color_space === undefined ? timing :
          timing.color_space,
      'video.camera_frame_callbacks': callbacks,
      'video.camera_stats': stats,
    };
  } finally {
    track.stop();
  }
}

async function cameraValues() {
  const plain = await cameraTrack(true);
  const hd = await cameraTrack({width: {ideal: 1920}, height: {ideal: 1080}});
  return Object.assign({
    'video.camera_settings': plain.settings,
    'video.camera_capabilities': plain.capabilities,
    'video.camera_frame': plain.frame,
    'video.camera_hd_settings': hd.settings,
    'video.camera_hd_frame': hd.frame,
    'video.camera_device_capabilities': await cameraDeviceCapabilities(),
    'video.camera_overconstrained': await cameraOverconstrained(),
  }, await cameraBehaviour());
}

window.mlCameraValues = () => cameraValues().catch((e) => ({
  'video.camera_settings': ML.errInfo(e)}));
