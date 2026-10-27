// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Collects what docs/FINGERPRINTING.md promises is the same for every
// MoltNet user, plus a few values nothing promises (to show what still
// varies). Flat keys; leakkit/expectations.py says what each must be.
// mlGeometry() is called again by the harness after every window resize
// and zoom change. No value that identifies the machine beyond what a site
// can already read is collected (no unmasked GPU strings, no device labels).
'use strict';

let lastMouse = null;
// navigator.connection change events from page load to the end of the
// collection (none in MoltNet: its values never change).
let connectionChanges = 0;
if (navigator.connection) {
  navigator.connection.addEventListener('change', () => {
    connectionChanges += 1;
  });
}
addEventListener('mousemove', (e) => {
  lastMouse = {screenX: e.screenX, screenY: e.screenY, clientX: e.clientX,
    clientY: e.clientY};
});

function mq(q) {
  try {
    return matchMedia(q).matches;
  } catch (e) {
    return null;
  }
}

// The bits per colour component the (color: N) media query matches, or
// null when none from 0 to 16 does.
function colorBits() {
  for (let bits = 0; bits <= 16; ++bits) {
    if (mq('(color: ' + bits + ')')) {
      return bits;
    }
  }
  return null;
}

// AudioContext.renderQuantumSize for renderSizeHint "hardware" ('absent'
// without the API).
function hardwareRenderQuantum() {
  if (!('renderQuantumSize' in BaseAudioContext.prototype)) {
    return 'absent';
  }
  const ctx = new AudioContext({renderSizeHint: 'hardware'});
  const size = ctx.renderQuantumSize;
  ctx.close();
  return size;
}

// Web Audio's output device as a page sees it (MoltNet: one 48 kHz stereo
// device with a 10 ms buffer and a 24 ms output latency).
async function audioDevice(v) {
  if (!window.AudioContext) {
    v['audio.sample_rate'] = 'absent';
    return;
  }
  const ctx = new AudioContext();
  v['audio.sample_rate'] = ctx.sampleRate;
  v['audio.output_latency'] = ctx.outputLatency;
  v['audio.max_channel_count'] = ctx.destination.maxChannelCount;
  v['audio.channel_count'] = ctx.destination.channelCount;
  try {
    ctx.destination.channelCount = 3;
    v['audio.channel_count_3'] = 'ok';
  } catch (e) {
    v['audio.channel_count_3'] = ML.errName(e);
  }
  v['audio.script_processor_buffer'] = ctx.createScriptProcessor().bufferSize;
  // Nothing played yet reads {0, 0}; afterwards the sample being heard is at
  // least the output latency behind currentTime.
  const ts = ctx.getOutputTimestamp();
  v['audio.output_timestamp_ok'] = ts.contextTime === 0 ?
      ts.performanceTime === 0 || ctx.currentTime > 0 :
      ts.contextTime <= Math.max(0, ctx.currentTime - 0.024) + 1e-9 &&
      ts.performanceTime > 0;
  if (ctx.playbackStats) {
    const s = ctx.playbackStats;
    v['audio.playback_latency_max'] = s.maximumLatency;
    v['audio.playback_underruns'] = s.underrunEvents;
  } else {
    v['audio.playback_latency_max'] = 'absent';
    v['audio.playback_underruns'] = 'absent';
  }
  v['audio.sink_id'] = typeof ctx.sinkId === 'string' ? ctx.sinkId :
      typeof ctx.sinkId;
  await ctx.close();
  const latencies = {};
  for (const [name, options] of [['interactive', {}],
    ['balanced', {latencyHint: 'balanced'}],
    ['playback', {latencyHint: 'playback'}],
    ['exact_0', {latencyHint: 0}], ['exact_20ms', {latencyHint: 0.02}],
    ['rate_44100', {sampleRate: 44100}]]) {
    const c = new AudioContext(options);
    latencies[name] = c.baseLatency;
    await c.close();
  }
  v['audio.base_latency'] = latencies;
}

// When Web Audio renders, as a page can time it (MoltNet: on the uniform
// device's clock, 480 frames every 10 ms, whatever the real device).
// currentTime is polled on the main thread for 300 ms; its changes less
// than 1.5 ms apart are one burst of rendering. Reports the median time
// between bursts and the burst sizes, in frames (3 or 4 render quanta of 128
// for 480 frames), that make up at least a tenth of the bursts. A poll held
// up (the machine, a garbage collection) can split or merge bursts, so the
// bursts next to one aren't sized. Needs a running context, so the harness
// calls it with a user gesture after the page's collection.
async function audioCadence() {
  if (!window.AudioContext) {
    return {'audio.cadence_interval_ms': 'absent',
      'audio.cadence_burst_frames': 'absent'};
  }
  const ctx = new AudioContext();
  try {
    await ML.timeout(ctx.resume(), 3000);
    const wait = performance.now();
    while (ctx.currentTime === 0 && performance.now() - wait < 2000) {
      await new Promise((r) => setTimeout(r, 5));
    }
    if (ctx.currentTime === 0) {
      return {'audio.cadence_interval_ms': 'not running',
        'audio.cadence_burst_frames': 'not running'};
    }
    const bursts = [];
    let last = ctx.currentTime;
    let prev = performance.now();
    let held = false;
    const end = prev + 300;
    for (let now = prev; now < end; prev = now, now = performance.now()) {
      if (now - prev > 0.5) {
        held = true;
        const b = bursts[bursts.length - 1];
        if (b && prev - b.end < 1.5) {
          b.held = true;
        }
      }
      const t = ctx.currentTime;
      if (t === last) {
        continue;
      }
      const frames = Math.round((t - last) * ctx.sampleRate);
      last = t;
      const b = bursts[bursts.length - 1];
      if (b && now - b.end < 1.5) {
        b.frames += frames;
        b.end = now;
        b.held = b.held || held;
      } else {
        bursts.push({start: now, end: now, frames, held});
      }
      held = false;
    }
    // The first and last may be cut short.
    const whole = bursts.slice(1, -1);
    const gaps = whole.slice(1).map((b, i) => b.start - whole[i].start)
        .sort((a, b) => a - b);
    const sized = whole.filter((b) => !b.held);
    const counts = new Map();
    for (const b of sized) {
      counts.set(b.frames, (counts.get(b.frames) || 0) + 1);
    }
    return {
      'audio.cadence_interval_ms': gaps.length ?
          Math.round(gaps[Math.floor(gaps.length / 2)]) : 'no bursts',
      'audio.cadence_burst_frames': [...counts]
          .filter(([, n]) => n * 10 >= sized.length)
          .map(([frames]) => frames).sort((a, b) => a - b),
    };
  } finally {
    await ctx.close();
  }
}
window.mlAudioCadence = () => audioCadence().catch((e) => ({
  'audio.cadence_interval_ms': ML.errInfo(e),
  'audio.cadence_burst_frames': ML.errInfo(e)}));

// A WAV file of `seconds` of digital silence, 16-bit mono at `rate`.
function silentWav(seconds, rate) {
  const frames = Math.round(seconds * rate);
  const d = new DataView(new ArrayBuffer(44 + frames * 2));
  const text = (at, s) => {
    for (let i = 0; i < s.length; ++i) {
      d.setUint8(at + i, s.charCodeAt(i));
    }
  };
  text(0, 'RIFF');
  d.setUint32(4, 36 + frames * 2, true);
  text(8, 'WAVE');
  text(12, 'fmt ');
  d.setUint32(16, 16, true);
  d.setUint16(20, 1, true);
  d.setUint16(22, 1, true);
  d.setUint32(24, rate, true);
  d.setUint32(28, rate * 2, true);
  d.setUint16(32, 2, true);
  d.setUint16(34, 16, true);
  text(36, 'data');
  d.setUint32(40, frames * 2, true);
  return new Blob([d.buffer], {type: 'audio/wav'});
}

// An <audio> element playing (silence, unmuted, so through the output
// device's path; MoltNet: on its fixed clock, device or not). Reports
// whether currentTime runs at the media's pace and, for the record, how
// long after play() it starts moving (rounded to 10 ms). Needs a user
// gesture, so the harness calls it after the page's collection.
async function mediaClock() {
  const audio = new Audio();
  const url = URL.createObjectURL(silentWav(3, 48000));
  try {
    await ML.timeout(new Promise((resolve, reject) => {
      audio.oncanplaythrough = resolve;
      audio.onerror = () => reject(new Error('media error'));
      audio.src = url;
    }), 5000);
    const t0 = performance.now();
    await ML.timeout(audio.play(), 3000);
    while (audio.currentTime === 0 && performance.now() - t0 < 2000) {
      await ML.sleep(2);
    }
    if (audio.currentTime === 0) {
      return {'media.clock_running': false,
        'media.clock_start_ms': 'not running'};
    }
    const start = performance.now() - t0;
    const c0 = audio.currentTime;
    const w0 = performance.now();
    await ML.sleep(500);
    const pace = (audio.currentTime - c0) * 1000 / (performance.now() - w0);
    return {'media.clock_running': pace > 0.8 && pace < 1.2,
      'media.clock_start_ms': Math.round(start / 10) * 10};
  } finally {
    audio.pause();
    audio.removeAttribute('src');
    audio.load();
    URL.revokeObjectURL(url);
  }
}
window.mlMediaClock = () => mediaClock().catch((e) => ({
  'media.clock_running': ML.errInfo(e),
  'media.clock_start_ms': ML.errInfo(e)}));

// getUserMedia() before any permission, from a frame of its own that is
// removed after 1.5 s (ending the request and any prompt). A request the
// user is asked about is still pending then; Chromium fails at once, with
// NotFoundError or OverconstrainedError, when no device could meet it.
async function userMediaBeforePermission(constraints) {
  const frame = document.createElement('iframe');
  document.body.appendChild(frame);
  try {
    const md = frame.contentWindow.navigator.mediaDevices;
    if (!md || !md.getUserMedia) {
      return 'absent';
    }
    return await Promise.race([
      md.getUserMedia(constraints).then((stream) => {
        stream.getTracks().forEach((t) => t.stop());
        return 'granted';
      }, (e) => ML.errName(e)),
      ML.sleep(1500).then(() => 'pending')]);
  } finally {
    frame.remove();
  }
}

function decodingAnswer(info) {
  return {supported: info.supported, smooth: info.smooth,
    powerEfficient: info.powerEfficient};
}

// Encrypted Media Extensions, Media Capabilities and WebCodecs (MoltNet: no
// key systems, a fixed table, software codecs only).
async function mediaCapabilities(v, step) {
  const n = navigator;
  const webm = [{initDataTypes: ['webm'], videoCapabilities: [
    {contentType: 'video/webm; codecs="vp8"'}]}];
  for (const [name, system] of [['clearkey', 'org.w3.clearkey'],
    ['widevine', 'com.widevine.alpha'],
    ['playready', 'com.microsoft.playready.recommendation']]) {
    await step('eme.' + name, () => n.requestMediaKeySystemAccess(system,
        webm).then(() => 'granted', (e) => ML.errName(e)));
  }
  const video = (contentType, width, height, framerate) => ({contentType,
    width, height, bitrate: 2000000, framerate});
  const mc = n.mediaCapabilities;
  const queries = {
    'mc.vp8_360p': {type: 'file', video: video('video/webm; codecs="vp8"',
        640, 360, 30)},
    'mc.vp9_1080p': {type: 'file', video: video(
        'video/webm; codecs="vp09.00.40.08"', 1920, 1080, 30)},
    'mc.av1_2160p60': {type: 'media-source', video: video(
        'video/webm; codecs="av01.0.13M.08"', 3840, 2160, 60)},
    'mc.opus': {type: 'file', audio: {contentType:
        'audio/webm; codecs="opus"'}},
    'mc.h264': {type: 'media-source', video: video(
        'video/mp4; codecs="avc1.640028"', 1920, 1080, 30)},
    'mc.hevc': {type: 'media-source', video: video(
        'video/mp4; codecs="hvc1.1.6.L120.90"', 1920, 1080, 30)},
    'mc.encrypted': {type: 'media-source', video: video(
        'video/webm; codecs="vp09.00.40.08"', 1920, 1080, 30),
    keySystemConfiguration: {keySystem: 'org.w3.clearkey'}},
    'mc.webrtc_decoding': {type: 'webrtc', video: video('video/VP8', 1280,
        720, 30)},
  };
  for (const [key, query] of Object.entries(queries)) {
    await step(key, () => mc ? mc.decodingInfo(query).then(decodingAnswer) :
        'absent');
  }
  await step('mc.webrtc_encoding', () => mc ? mc.encodingInfo({type:
      'webrtc', video: video('video/VP8', 1280, 720, 30)}).then(
      decodingAnswer) : 'absent');
  const canPlay = {};
  const element = document.createElement('video');
  for (const type of ['video/webm; codecs="vp8, vorbis"',
    'video/webm; codecs="vp09.00.10.08"', 'video/mp4; codecs="av01.0.05M.08"',
    'video/mp4; codecs="avc1.42E01E, mp4a.40.2"',
    'video/mp4; codecs="hvc1.1.6.L93.B0"', 'audio/mpeg', 'audio/flac',
    'audio/ogg; codecs="opus"', 'audio/mp4; codecs="mp4a.40.2"']) {
    canPlay[type] = [element.canPlayType(type), window.MediaSource ?
        MediaSource.isTypeSupported(type) : null];
  }
  v['media.can_play'] = canPlay;
  const vp9 = 'vp09.00.10.08';
  await step('webcodecs.decoder_hardware', () => window.VideoDecoder ?
      VideoDecoder.isConfigSupported({codec: vp9, hardwareAcceleration:
          'prefer-hardware'}).then((s) => s.supported) : 'absent');
  await step('webcodecs.decoder_software', () => window.VideoDecoder ?
      VideoDecoder.isConfigSupported({codec: vp9, hardwareAcceleration:
          'prefer-software'}).then((s) => s.supported) : 'absent');
  await step('webcodecs.encoder_hardware', () => window.VideoEncoder ?
      VideoEncoder.isConfigSupported({codec: 'vp8', width: 640, height: 480,
        hardwareAcceleration: 'prefer-hardware'}).then((s) => s.supported) :
      'absent');
  await step('webcodecs.encoder_vp8', () => window.VideoEncoder ?
      VideoEncoder.isConfigSupported({codec: 'vp8', width: 640, height: 480})
          .then((s) => s.supported) : 'absent');
  await step('webcodecs.encoder_h264', () => window.VideoEncoder ?
      VideoEncoder.isConfigSupported({codec: 'avc1.42001E', width: 640,
        height: 480}).then((s) => s.supported) : 'absent');
}

// A classic scrollbar's width in CSS pixels, from a box that always has one
// (0 with overlay scrollbars).
function scrollbarBox() {
  const box = document.createElement('div');
  box.style.cssText = 'position:absolute;left:0;top:0;width:200px;' +
      'height:100px;overflow:scroll;visibility:hidden';
  document.body.appendChild(box);
  const width = box.offsetWidth - box.clientWidth;
  box.remove();
  return width;
}

// Whether the width/height media queries and vw/vh see exactly the page's
// innerWidth x innerHeight, strict comparisons included (1400.0001 would
// match (width > 1400px)).
function viewportExact() {
  const w = innerWidth + 'px';
  const h = innerHeight + 'px';
  return mq('(width: ' + w + ') and (height: ' + h + ')') === true &&
      [`(width > ${w})`, `(width < ${w})`, `(height > ${h})`,
        `(height < ${h})`, '(width > 100vw)', '(width < 100vw)',
        '(height > 100vh)', '(height < 100vh)'].every((q) => mq(q) === false);
}

window.mlGeometry = () => {
  const s = screen;
  const v = {
    'win.innerWidth': innerWidth, 'win.innerHeight': innerHeight,
    'win.outerWidth': outerWidth, 'win.outerHeight': outerHeight,
    'win.screenX': screenX, 'win.screenY': screenY,
    'win.devicePixelRatio': devicePixelRatio,
    'screen.width': s.width, 'screen.height': s.height,
    'screen.availWidth': s.availWidth, 'screen.availHeight': s.availHeight,
    'screen.availLeft': s.availLeft, 'screen.availTop': s.availTop,
    'screen.colorDepth': s.colorDepth, 'screen.pixelDepth': s.pixelDepth,
    'screen.isExtended': s.isExtended === undefined ? null : s.isExtended,
    'screen.orientation.type': s.orientation ? s.orientation.type : null,
    'screen.orientation.angle': s.orientation ? s.orientation.angle : null,
    'mq.resolution_1dppx': mq('(resolution: 1dppx)'),
    'mq.device_size_matches': mq('(device-width: ' + s.width +
        'px) and (device-height: ' + s.height + 'px)'),
    'scrollbar.width': innerWidth - document.documentElement.clientWidth,
    'scrollbar.box': scrollbarBox(),
    // Overlay scrollbars take no room (macOS's default style).
    'scrollbar.overlay': scrollbarBox() === 0,
    'mq.viewport_exact': viewportExact(),
    'vv.width': visualViewport ? visualViewport.width : null,
    'vv.height': visualViewport ? visualViewport.height : null,
    'vv.scale': visualViewport ? visualViewport.scale : null,
  };
  if (lastMouse) {
    v['mouse.event'] = lastMouse;
    v['mouse.screen_equals_client'] = lastMouse.screenX === lastMouse.clientX &&
        lastMouse.screenY === lastMouse.clientY;
  }
  return v;
};

const WEBGL1 = ['MAX_TEXTURE_SIZE', 'MAX_CUBE_MAP_TEXTURE_SIZE',
  'MAX_RENDERBUFFER_SIZE', 'MAX_VERTEX_ATTRIBS', 'MAX_VERTEX_UNIFORM_VECTORS',
  'MAX_FRAGMENT_UNIFORM_VECTORS', 'MAX_VARYING_VECTORS',
  'MAX_TEXTURE_IMAGE_UNITS', 'MAX_VERTEX_TEXTURE_IMAGE_UNITS',
  'MAX_COMBINED_TEXTURE_IMAGE_UNITS', 'SUBPIXEL_BITS', 'MAX_VIEWPORT_DIMS',
  'ALIASED_POINT_SIZE_RANGE', 'ALIASED_LINE_WIDTH_RANGE'];
const WEBGL2 = WEBGL1.concat(['MAX_VERTEX_UNIFORM_COMPONENTS',
  'MAX_FRAGMENT_UNIFORM_COMPONENTS', 'MAX_VARYING_COMPONENTS',
  'MAX_VERTEX_OUTPUT_COMPONENTS', 'MAX_FRAGMENT_INPUT_COMPONENTS',
  'MAX_DRAW_BUFFERS', 'MAX_COLOR_ATTACHMENTS', 'MAX_3D_TEXTURE_SIZE',
  'MAX_ARRAY_TEXTURE_LAYERS', 'MAX_SAMPLES', 'MAX_VERTEX_UNIFORM_BLOCKS',
  'MAX_FRAGMENT_UNIFORM_BLOCKS', 'MAX_COMBINED_UNIFORM_BLOCKS',
  'MAX_UNIFORM_BUFFER_BINDINGS', 'MAX_UNIFORM_BLOCK_SIZE',
  'MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS',
  'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS',
  'UNIFORM_BUFFER_OFFSET_ALIGNMENT', 'MIN_PROGRAM_TEXEL_OFFSET',
  'MAX_PROGRAM_TEXEL_OFFSET', 'MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS',
  'MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS',
  'MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS', 'MAX_ELEMENT_INDEX',
  'MAX_ELEMENTS_VERTICES', 'MAX_ELEMENTS_INDICES', 'MAX_TEXTURE_LOD_BIAS',
  'MAX_SERVER_WAIT_TIMEOUT']);
const PRECISIONS = ['LOW_FLOAT', 'MEDIUM_FLOAT', 'HIGH_FLOAT', 'LOW_INT',
  'MEDIUM_INT', 'HIGH_INT'];

function plain(x) {
  if (x === null || x === undefined) {
    return null;
  }
  if (ArrayBuffer.isView(x)) {
    return Array.from(x);
  }
  return x;
}

function webgl(v, kind) {
  const prefix = kind === 'webgl2' ? 'webgl2' : 'webgl';
  let gl;
  try {
    gl = document.createElement('canvas').getContext(kind);
  } catch (e) {
    gl = null;
  }
  if (!gl) {
    v[prefix + '.context'] = false;
    return;
  }
  v[prefix + '.context'] = true;
  v[prefix + '.debug_renderer_info'] =
      gl.getExtension('WEBGL_debug_renderer_info') !== null;
  v[prefix + '.extensions'] = gl.getSupportedExtensions();
  v[prefix + '.vendor'] = gl.getParameter(gl.VENDOR);
  v[prefix + '.renderer'] = gl.getParameter(gl.RENDERER);
  for (const name of kind === 'webgl2' ? WEBGL2 : WEBGL1) {
    if (gl[name] === undefined) {
      continue;
    }
    try {
      v[prefix + '.p.' + name] = plain(gl.getParameter(gl[name]));
    } catch (e) {
      v[prefix + '.p.' + name] = ML.errInfo(e);
    }
  }
  const aniso = gl.getExtension('EXT_texture_filter_anisotropic');
  if (aniso) {
    v[prefix + '.p.MAX_TEXTURE_MAX_ANISOTROPY_EXT'] =
        gl.getParameter(aniso.MAX_TEXTURE_MAX_ANISOTROPY_EXT);
  }
  for (const shader of ['VERTEX_SHADER', 'FRAGMENT_SHADER']) {
    for (const p of PRECISIONS) {
      const f = gl.getShaderPrecisionFormat(gl[shader], gl[p]);
      v[prefix + '.precision.' + shader + '.' + p] = f ?
          [f.rangeMin, f.rangeMax, f.precision] : null;
    }
  }
  if (kind === 'webgl2') {
    try {
      v['webgl2.samples_rgba8'] = plain(gl.getInternalformatParameter(
          gl.RENDERBUFFER, gl.RGBA8, gl.SAMPLES));
    } catch (e) {
      v['webgl2.samples_rgba8'] = ML.errInfo(e);
    }
  }
}

// Family names a page might probe: Linux's allow-list and aliases, and
// families common elsewhere (none of which a MoltNet page on Linux may see).
const FONT_PROBES = ['DejaVu Sans', 'DejaVu Sans Condensed',
  'DejaVu Sans Light', 'DejaVu Sans Mono', 'DejaVu Serif',
  'DejaVu Serif Condensed', 'Liberation Mono', 'Liberation Sans',
  'Liberation Serif', 'Noto Color Emoji', 'Noto Sans', 'Noto Sans Mono',
  'Noto Serif', 'Arial', 'Helvetica', 'Times New Roman', 'Times',
  'Courier New', 'Courier', 'Ubuntu', 'Ubuntu Mono', 'Cantarell',
  'Noto Sans CJK JP', 'Noto Sans CJK SC', 'Noto Sans Arabic', 'FreeSans',
  'FreeSerif', 'FreeMono', 'Droid Sans', 'Roboto', 'Open Sans', 'Lato',
  'Inconsolata', 'Hack', 'Fira Code', 'Fira Sans', 'Source Code Pro',
  'Liberation Sans Narrow', 'Comic Sans MS', 'Impact', 'Verdana', 'Georgia',
  'Tahoma', 'Trebuchet MS', 'Segoe UI', 'Calibri', 'Cambria', 'Consolas',
  'Menlo', 'Monaco', 'Helvetica Neue', 'Gill Sans', 'Lucida Grande',
  'Wingdings', 'Symbol', 'Bitstream Vera Sans', 'URW Gothic',
  'Nimbus Sans', 'Nimbus Roman', 'C059', 'P052', 'Z003', 'Latin Modern Math',
  'STIX Two Math', 'Carlito', 'Caladea', 'Gentium', 'Linux Libertine O'];

function textWidth(family) {
  const c = document.createElement('canvas').getContext('2d');
  c.font = '32px ' + family;
  return Math.round(c.measureText('mmmmmmmmmlli WW@#0123 éß')
      .width * 100) / 100;
}

// Canvas text metrics (FINGERPRINTING.md "Text metrics (MoltNet)"):
// MoltNet rounds them to whole pixels at these sizes (left-aligned,
// alphabetic baseline), and on Linux lays every font out unhinted, so they
// are the same on every machine with the same fonts. Element geometry
// isn't rounded; it is recorded to compare runs.
const TEXT_METRICS = ['width', 'actualBoundingBoxLeft',
  'actualBoundingBoxRight', 'actualBoundingBoxAscent',
  'actualBoundingBoxDescent', 'fontBoundingBoxAscent',
  'fontBoundingBoxDescent', 'alphabeticBaseline', 'hangingBaseline',
  'ideographicBaseline', 'emHeightAscent', 'emHeightDescent'];
const TEXT_SAMPLE = 'MoltNet fjord Wave 0123 \u00e9\u00df';

function textMetrics(v) {
  const c = document.createElement('canvas').getContext('2d');
  const metrics = {};
  let whole = true;
  for (const family of ['sans-serif', 'serif', 'monospace']) {
    for (const size of [16, 48]) {
      c.font = size + 'px ' + family;
      const m = c.measureText(TEXT_SAMPLE);
      // emHeight* only exist with ExtendedTextMetrics.
      const row = TEXT_METRICS.filter((k) => typeof m[k] === 'number')
          .map((k) => m[k]);
      whole = whole && row.every((x) => Number.isInteger(x));
      metrics[family + ' ' + size] = row;
    }
  }
  v['text.metrics'] = metrics;
  v['text.metrics_whole'] = whole;
  c.font = '32px sans-serif';
  v['text.emoji'] = ['\u{1F600}', '\u{1F9C5}', '\u{1FAE8}']
      .map((e) => c.measureText(e).width);
  const span = document.createElement('span');
  span.style.cssText = 'position:absolute;white-space:pre;font:16px serif';
  span.textContent = TEXT_SAMPLE;
  document.body.appendChild(span);
  v['text.dom_width'] = span.getBoundingClientRect().width;
  span.remove();
}

function forms(v) {
  const box = document.createElement('div');
  box.innerHTML = '<input id="mlreq" required>' +
      '<input id="mlnum" type="number" min="10" value="5">' +
      '<input id="mldate" type="date"><input id="mltime" type="time">' +
      '<input id="mlfile" type="file"><input id="mlsubmit" type="submit">' +
      '<details id="mldetails"></details>';
  document.body.appendChild(box);
  const $ = (id) => document.getElementById(id);
  v['form.validationMessage'] = $('mlreq').validationMessage;
  v['form.numberMessage'] = $('mlnum').validationMessage;
  v['form.widths'] = {date: $('mldate').offsetWidth,
    time: $('mltime').offsetWidth, file: $('mlfile').offsetWidth,
    submit: $('mlsubmit').offsetWidth, details: $('mldetails').offsetWidth,
    details_h: $('mldetails').offsetHeight};
  v['css.default_font_size'] = getComputedStyle(document.body).fontSize;
  box.remove();
}

async function canvasHash() {
  const c = document.createElement('canvas');
  c.width = 220;
  c.height = 40;
  const g = c.getContext('2d');
  g.fillStyle = '#f60';
  g.fillRect(10, 5, 100, 30);
  g.fillStyle = 'rgba(30, 120, 200, 0.7)';
  g.font = '16px sans-serif';
  g.fillText('moltleak \u{1F9C5} 0123', 4, 25);
  return hex(new TextEncoder().encode(c.toDataURL()));
}

async function audioHash() {
  const ctx = new OfflineAudioContext(1, 5000, 44100);
  const osc = ctx.createOscillator();
  osc.type = 'triangle';
  osc.frequency.value = 10000;
  const comp = ctx.createDynamicsCompressor();
  osc.connect(comp);
  comp.connect(ctx.destination);
  osc.start(0);
  const buf = await ctx.startRendering();
  return hex(new Uint8Array(buf.getChannelData(0).buffer));
}

async function hex(bytes) {
  const h = await crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(h)).slice(0, 12)
      .map((b) => b.toString(16).padStart(2, '0')).join('');
}

function frameLoad(src, onMessage) {
  return new Promise((resolve) => {
    const f = document.createElement('iframe');
    const done = (value) => {
      resolve(value);
      f.remove();
    };
    if (onMessage) {
      addEventListener('message', (e) => {
        if (e.source === f.contentWindow) {
          done(e.data);
        }
      });
    } else {
      f.onload = () => {
        try {
          done(f.contentDocument.characterSet);
        } catch (e) {
          done(ML.errName(e));
        }
      };
    }
    f.src = src;
    document.body.appendChild(f);
    setTimeout(() => done('timeout'), 15000);
  });
}

// MediaRecorder: the type it records for each container a page can ask for
// (MoltNet: the build's software encoders on every machine, so VP8 in WebM
// and Matroska, VP9 in MP4), and what isTypeSupported() says.
const RECORDER_TYPES = ['video/webm;codecs=vp8', 'video/webm;codecs=vp9',
  'video/webm;codecs=av01', 'video/webm;codecs=h264',
  'video/x-matroska;codecs=avc1', 'video/mp4;codecs=avc1.42E01E',
  'video/mp4;codecs=avc1.640028', 'video/mp4;codecs=av01.0.04M.08',
  'video/mp4;codecs=hvc1.1.6.L93.B0', 'video/x-matroska;codecs=hev1.1.6.L93.B0',
  'audio/webm;codecs=opus', 'audio/webm;codecs=pcm',
  'audio/mp4;codecs=mp4a.40.2', 'audio/mp4;codecs=opus'];

async function recorderTypes() {
  if (!window.MediaRecorder) {
    return 'absent';
  }
  const c = document.createElement('canvas');
  c.width = 64;
  c.height = 48;
  c.getContext('2d').fillRect(0, 0, 8, 8);
  const stream = c.captureStream();
  const out = {};
  for (const type of ['', 'video/webm', 'video/x-matroska', 'video/mp4']) {
    const label = type || 'default';
    try {
      // mimeType names the chosen codecs from the start event on; stopping
      // right away delivers it too.
      const r = new MediaRecorder(stream, type ? {mimeType: type} : {});
      const stopped = new Promise((resolve) => {
        r.onstop = resolve;
        setTimeout(resolve, 2000);
      });
      r.start();
      r.stop();
      await stopped;
      out[label] = r.mimeType;
    } catch (e) {
      out[label] = ML.errName(e);
    }
  }
  stream.getTracks().forEach((t) => t.stop());
  return out;
}

// src: local() of faces no OS's allow-list has (any that loads is a
// failure: an installed one must look missing), and of faces of Linux's
// listed families (the same on every machine of an OS). Only the result is
// checked: a hidden face isn't looked up at all, so its timing equals a
// missing one's, which a test run can't measure reliably anyway.
const LOCAL_HIDDEN = ['Ubuntu', 'Ubuntu-Regular', 'Ubuntu Bold',
  'Cantarell', 'Cantarell-Regular', 'Noto Sans CJK JP Regular',
  'NotoSansCJKjp-Regular', 'Liberation Sans Narrow',
  'LiberationSansNarrow-Bold', 'Fira Sans', 'FiraSans-Regular', 'Open Sans',
  'OpenSans-Regular', 'Source Code Pro', 'SourceCodePro-Regular'];
const LOCAL_LISTED = ['DejaVu Sans', 'DejaVuSans-Bold',
  'DejaVu Sans Bold Oblique', 'DejaVuSansMono', 'Liberation Sans',
  'LiberationSans-Bold', 'Liberation Serif Italic', 'Noto Sans',
  'NotoSans-Bold'];

async function localFaces(names) {
  const loaded = [];
  for (const name of names) {
    try {
      await new FontFace('mlLocalProbe', 'local("' + name + '")').load();
      loaded.push(name);
    } catch (e) {
      // Not available to this page.
    }
  }
  return loaded;
}

// MoltNet's bundled font set (FINGERPRINTING.md "Fonts"): MoltNet pages
// take every font from it, on every OS. MOLTNET_FONTS is every name a
// MoltNet page may see (the set's families, their other names and the
// aliases; tools/fonts/moltnet_fonts.json, checked by the kit's tests).
const MOLTNET_FONTS = ['Arimo', 'Tinos', 'Cousine', 'Noto Sans',
  'Noto Naskh Arabic', 'Noto Sans Hebrew', 'Noto Sans Armenian',
  'Noto Sans Georgian', 'Noto Sans Ethiopic', 'Noto Sans Devanagari',
  'Noto Sans Bengali', 'Noto Sans Gujarati', 'Noto Sans Gurmukhi',
  'Noto Sans Kannada', 'Noto Sans Malayalam', 'Noto Sans Oriya',
  'Noto Sans Tamil', 'Noto Sans Telugu', 'Noto Sans Sinhala',
  'Noto Sans Thai', 'Noto Sans Lao', 'Noto Sans Khmer', 'Noto Sans Myanmar',
  'Noto Serif Tibetan', 'Noto Sans Thaana', 'Noto Sans Syriac',
  'Noto Sans Cherokee', 'Noto Sans Canadian Aboriginal',
  'Noto Sans Javanese', 'Noto Sans Ol Chiki', 'Noto Sans Adlam',
  'Noto Sans Mongolian', 'Noto Sans NKo', 'Noto Sans Tifinagh',
  'Noto Sans Yi', 'Noto Sans Symbols', 'Noto Sans Symbols 2',
  'Noto Sans Math', 'Noto Sans CJK JP', 'Noto Sans CJK KR',
  'Noto Sans CJK SC', 'Noto Sans CJK TC', 'Noto Sans CJK HK',
  'Noto Color Emoji', 'Noto Sans Hebrew Light', 'Arial', 'Helvetica',
  'Liberation Sans', 'Times New Roman', 'Times', 'MS Serif',
  'Liberation Serif', 'Courier New', 'Courier', 'Liberation Mono'];
// The family each generic keyword renders Latin text with, everywhere.
const MOLTNET_GENERICS = {'serif': 'Tinos', 'sans-serif': 'Arimo',
  'monospace': 'Cousine', 'cursive': 'Tinos', 'fantasy': 'Tinos',
  'system-ui': 'Noto Sans', 'math': 'Noto Sans Math'};
// Text no Latin family has: each run falls back through the set's list for
// its script, so its width is the same on every machine and OS.
const FALLBACK_SAMPLES = {
  Arab: 'العربية',
  Hebr: 'עברית',
  Deva: 'हिन्दी',
  Beng: 'বাংলা',
  Taml: 'தமிழ்',
  Thai: 'ภาษาไทย',
  Khmr: 'ភាសា',
  Mymr: 'မြန်မာ',
  Ethi: 'አማርኛ',
  Geor: 'ქართული',
  Armn: 'Հայերեն',
  Sinh: 'සිංහල',
  Hani: '漢字骨直',
  Kana: 'ひらがなカタ',
  Hang: '한국어',
  Symbols: '☃♥✈⌘☀♻',
  Math: '∑∫√∂\u{1D400}\u{1D7D8}',
  Emoji: '\u{1F600}\u{1F9C5}\u{1F44D}\u{1F3FD}\u{1FAE8}',
  Unassigned: '\u{10FFFD}͸',
};
// src: local() names of the set's faces (PostScript and full names).
const LOCAL_BUNDLED = ['Arimo-Regular', 'Arimo Italic', 'Tinos-Bold',
  'Tinos Bold Italic', 'Cousine-Regular', 'NotoSans-Regular',
  'Noto Naskh Arabic Regular', 'NotoSansMath-Regular', 'NotoSansCJKjp-Thin',
  'Noto Color Emoji'];

async function bundledFonts(v, step) {
  await step('fonts.moltnet_set', async () => {
    await document.fonts.ready;
    return MOLTNET_FONTS.filter((f) => document.fonts.check('16px "' + f +
        '"'));
  });
  // Whether each generic keyword measures exactly like its family.
  v['fonts.generic_map'] = {};
  for (const [generic, family] of Object.entries(MOLTNET_GENERICS)) {
    v['fonts.generic_map'][generic] =
        textWidth(generic) === textWidth('"' + family + '"');
  }
  const c = document.createElement('canvas').getContext('2d');
  v['fonts.fallback_widths'] = {};
  for (const [script, text] of Object.entries(FALLBACK_SAMPLES)) {
    const row = [];
    for (const family of ['sans-serif', 'serif', 'monospace']) {
      c.font = '32px ' + family;
      row.push(Math.round(c.measureText(text).width * 100) / 100);
    }
    v['fonts.fallback_widths'][script] = row;
  }
  await step('fonts.local_bundled', () => localFaces(LOCAL_BUNDLED));
}

async function recorderAndLocalFonts(v, step) {
  await step('recorder.mime_type', recorderTypes);
  v['recorder.supported'] = window.MediaRecorder ?
      RECORDER_TYPES.filter((t) => MediaRecorder.isTypeSupported(t)) :
      'absent';
  await step('fonts.local_hidden', () => localFaces(LOCAL_HIDDEN));
  await step('fonts.local_listed', () => localFaces(LOCAL_LISTED));
}

// Media pixels and encoded bytes: a decoded video frame read back as YUV
// and as RGBA (from WebCodecs and from a <video>), and what VideoEncoder
// and AudioEncoder make of fixed input whose size or rate they must convert.
// In MoltNet each hash is the same on every machine of an OS, whatever its
// CPU, GPU and video decoder; `<video>` frames are I420 in CPU memory
// (software decoding). The clip is one 320x240 VP9 key frame (BT.709): SMPTE
// bars over a colour gradient.
const PIXELS_VP9_FRAME =
    'gkmDQkAT8A72BjgkHBj2AAPAb6z9vqOlYx4R/P+YHtfl/r/Nw31u6/FVfjEfZ3UfrfXhPDjl' +
    'daOC/IvYwYX9OEyo+Hi4ho2xIv4dAMAAZvgxS52/8ujpO3iTfMq6R+guC/jVlRs1mbIe87yC' +
    'kJahPD32hDmE5VwhBPK7hh2GgiVk9RtVdJGSlW0naYhusEwfn//8Li//47/8vcom17ANNfcF' +
    'zyO7AxgCtGQqge5M3uAnJZodyIXNa8j9nne5hOKNcwXF+ooNSDmhPMO8a4EhYdGz92jmt6Zr' +
    'L29dqnLkpTuxaZMzO+x3vIYbHDSExjpU01P44k/ohdTyg/gxo7yBeobzicxbmyENAJHAv83k' +
    'BesWx1pdvQgkGnPp8JvGEnDTrHTp5Xo3h3Sxf5qUz/9uI4JsCePLJMBitNbqAOKIEvucTJlU' +
    '8aGWwKUAVtnTDbIaoUfOIt7FhShtgmyQCnwjkwOpk2FL6Hx3cIteVp+oV/zv5Q+mvChGIjf7' +
    '1MabXAAbaxGPLeIYqczoSXZ78EJcu5HvI5YgD+hXMq6DJesPYh1mnXIHmTxbl8789mtSAxde' +
    'qPlBXzDk0AxNaYVZX3+4cc7DT/Lp2qJb8L51tu+RE3SbGn7CUQS/Xp9NE0uFC/dB3bUz4IOp' +
    'qyT4dEgq7ueSSJhzt+knhR31PRnkVM1VzRWFT732E0DfrBE9T7Mz+oV3X7elzToxZBvFI1n+' +
    'DjhxvUcOGNu10PNWBQ29ZkANIgPvwxX43QkxbqfgF2CDjES983JVvVe0CC3xoQXGd8lw9Px1' +
    'vrrKupaC8psa/d60QsImSE31RpnswX3A3KkzF/jXS9R954rtqQGHBc2xkuWSjOKN29UQkKZs' +
    'SCBdcc4l8tL8zZ4IA1XuK/CEUzXZTlFun9aHDUbs5lvMhhKUM9jFM4psFDFQjTgrsahROHXv' +
    'U/xy+dRTmcgbPUXP5MUVpQb3fdWbrjrQGgDM3iM2PWBpoArmeX6mzBE0R8QcLllBc7+y0KqF' +
    'GzOYSIHJzsJHxezxCQM1lNXXc7leow30itt4Gi/bIURvuxb5rwHYGF6qIAkJdq0R0gX4LAOB' +
    'yiudqjHdOP/KvrPHQtfkIbclgDMyJfifj5Y6mSXRNcslKm/MtNwbkA/6FCZsEoRLn8b2nvWP' +
    '8ky0k/cbxTkZPVjeyPoSgSHFqpko90/5ORS6cE+OGigPcieqIf4v6qTf2NbjsdRmSduA';
const PIXELS_VP9_WEBM =
    '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' +
    'sdRmSduAHFO7a5G7j7OBALeK94EB8YIBefCBAw==';

function base64Bytes(s) {
  return Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
}

async function hashChunks(chunks) {
  let size = 0;
  for (const c of chunks) {
    size += c.length;
  }
  const all = new Uint8Array(size);
  let at = 0;
  for (const c of chunks) {
    all.set(c, at);
    at += c.length;
  }
  return {chunks: chunks.length, bytes: size, hash: await hex(all)};
}

// The frame's own planes (format and bytes) and the frame as RGBA.
async function frameHashes(frame) {
  const out = {format: frame.format};
  if (frame.format) {
    const planes = new Uint8Array(frame.allocationSize());
    await frame.copyTo(planes);
    out.planes = await hex(planes);
  }
  const rgba = new Uint8Array(frame.displayWidth * frame.displayHeight * 4);
  await frame.copyTo(rgba, {format: 'RGBA', colorSpace: 'srgb'});
  out.rgba = await hex(rgba);
  return out;
}

async function webcodecsFrame() {
  if (!window.VideoDecoder) {
    return 'absent';
  }
  const frames = [];
  const decoder = new VideoDecoder({output: (f) => frames.push(f),
    error: () => {}});
  try {
    decoder.configure({codec: 'vp09.00.20.08', codedWidth: 320,
      codedHeight: 240});
    decoder.decode(new EncodedVideoChunk({type: 'key', timestamp: 0,
      data: base64Bytes(PIXELS_VP9_FRAME)}));
    await decoder.flush();
    return frames.length ? await frameHashes(frames[0]) : 'no frame';
  } finally {
    frames.forEach((f) => f.close());
    if (decoder.state !== 'closed') {
      decoder.close();
    }
  }
}

async function videoElementFrame() {
  if (!window.VideoFrame) {
    return 'absent';
  }
  const video = document.createElement('video');
  video.muted = true;
  video.preload = 'auto';
  const url = URL.createObjectURL(new Blob([base64Bytes(PIXELS_VP9_WEBM)],
      {type: 'video/webm'}));
  try {
    await new Promise((resolve, reject) => {
      video.onloadeddata = resolve;
      video.onerror = () => reject(video.error || new Error('video error'));
      video.src = url;
    });
    const frame = new VideoFrame(video, {timestamp: 0});
    try {
      return await frameHashes(frame);
    } finally {
      frame.close();
    }
  } finally {
    video.removeAttribute('src');
    video.load();
    URL.revokeObjectURL(url);
  }
}

// A 320x240 frame (I420 or RGBA, a fixed pattern) encoded as VP8 at
// 200x150: the encoder scales it first.
async function encodedVideo(format) {
  if (!window.VideoEncoder) {
    return 'absent';
  }
  const w = 320;
  const h = 240;
  let data;
  if (format === 'I420') {
    data = new Uint8Array(w * h * 3 / 2);
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        data[y * w + x] = ((x * 3 + y * 5) ^ (x >> 4 << 5)) & 255;
      }
    }
    const cw = w / 2;
    const ch = h / 2;
    for (let y = 0; y < ch; ++y) {
      for (let x = 0; x < cw; ++x) {
        data[w * h + y * cw + x] = (x * 2 + y) & 255;
        data[w * h + cw * ch + y * cw + x] = (255 - x - y * 2) & 255;
      }
    }
  } else {
    data = new Uint8Array(w * h * 4);
    for (let i = 0; i < w * h; ++i) {
      const x = i % w;
      const y = (i - x) / w;
      data.set([(x * 7) & 255, (y * 3 + x) & 255, (x ^ y) & 255, 255], i * 4);
    }
  }
  const chunks = [];
  const encoder = new VideoEncoder({output: (chunk) => {
    const b = new Uint8Array(chunk.byteLength);
    chunk.copyTo(b);
    chunks.push(b);
  }, error: () => {}});
  const frame = new VideoFrame(data, {format, codedWidth: w, codedHeight: h,
    timestamp: 0});
  try {
    encoder.configure({codec: 'vp8', width: 200, height: 150,
      bitrate: 400000, framerate: 30});
    encoder.encode(frame, {keyFrame: true});
    await encoder.flush();
    return await hashChunks(chunks);
  } finally {
    frame.close();
    if (encoder.state !== 'closed') {
      encoder.close();
    }
  }
}

// Half a second of a fixed stereo signal (a sweeping triangle and integer
// noise) encoded as Opus; 44.1 kHz input is resampled to 48 kHz first.
async function encodedAudio(sampleRate) {
  if (!window.AudioEncoder) {
    return 'absent';
  }
  const frames = sampleRate / 2;
  const data = new Float32Array(frames * 2);
  let phase = 0;
  let noise = 12345;
  for (let i = 0; i < frames; ++i) {
    phase = (phase + 40 + (i >> 6)) % 4096;
    noise = (Math.imul(noise, 1103515245) + 12345) >>> 0;
    const tri = Math.abs(phase - 2048) / 1024 - 1;
    data[i] = 0.5 * tri + (noise / 4294967296 - 0.5) / 8;
    data[frames + i] = 0.5 * tri - (noise % 1024) / 8192;
  }
  const chunks = [];
  const encoder = new AudioEncoder({output: (chunk) => {
    const b = new Uint8Array(chunk.byteLength);
    chunk.copyTo(b);
    chunks.push(b);
  }, error: () => {}});
  const audio = new AudioData({format: 'f32-planar', sampleRate,
    numberOfFrames: frames, numberOfChannels: 2, timestamp: 0, data});
  try {
    encoder.configure({codec: 'opus', sampleRate, numberOfChannels: 2,
      bitrate: 64000});
    encoder.encode(audio);
    await encoder.flush();
    return await hashChunks(chunks);
  } finally {
    audio.close();
    if (encoder.state !== 'closed') {
      encoder.close();
    }
  }
}

// MoltNet: what a page computes doesn't show the CPU (FINGERPRINTING.md
// "MoltNet: CPU features"). Two WebAssembly modules with one function of
// three v128 parameters: one uses a relaxed SIMD instruction
// (f32x4.relaxed_madd), the other the plain f32x4.add.
const WASM_HEAD = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x08, 0x01, 0x60, 0x03, 0x7b, 0x7b, 0x7b, 0x01, 0x7b,
  0x03, 0x02, 0x01, 0x00];
const WASM_RELAXED_SIMD = WASM_HEAD.concat([0x0a, 0x0d, 0x01, 0x0b, 0x00,
  0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfd, 0x85, 0x02, 0x0b]);
const WASM_SIMD = WASM_HEAD.concat([0x0a, 0x0b, 0x01, 0x09, 0x00,
  0x20, 0x00, 0x20, 0x01, 0xfd, 0xe4, 0x01, 0x0b]);

// Math functions, among them arguments for which C libraries' pow and tanh
// give other results with and without FMA. No result is NaN.
function mathHash() {
  const r = [Math.pow(9.628, -4.64), Math.pow(47.161, -5.06),
    Math.pow(82.58, 6.52), Math.pow(28.837, 3.57), 1.7 ** 2.9,
    Math.tanh(-0.164651), Math.tanh(-0.149006), Math.tanh(0.17547),
    Math.tanh(-0.080124), Math.tanh(3.3), Math.exp(-7.77),
    Math.expm1(0.001), Math.log(3.3), Math.log1p(1e-5), Math.log2(10),
    Math.log10(7), Math.cbrt(17), Math.cosh(1.7), Math.sinh(-2.2),
    Math.cos(1.2345), Math.acos(0.3), Math.asin(-0.6), Math.atanh(0.5),
    Math.asinh(3), Math.acosh(2.5), Math.atan2(1, 3), Math.hypot(3, 4.5)];
  return hex(new Uint8Array(new Float64Array(r).buffer));
}

// Math functions whose results differ between x86-64 and arm64 builds of
// V8's C library code for some arguments (sin and tan of tiny numbers,
// atan near 1, ...), and a sweep of sin, cos, tan and atan. MoltNet
// computes them the same way on every CPU and OS.
function mathCrossArch() {
  const r = [];
  for (const x of [4.461218733101436e-308, -4.625801642473825e-308]) {
    r.push(Math.sin(x), Math.tan(x));
  }
  for (const x of [1.0078749403766563, 1.315798130607, 2.427365698849,
    -1.0082559080619857, 9.563150059437]) {
    r.push(Math.atan(x), Math.atan2(x, 1.5), Math.asin(x / 10),
        Math.acos(x / 10));
  }
  for (let i = 1; i <= 2000; ++i) {
    const x = i * 0.0123456789;
    r.push(Math.sin(x), Math.cos(x), Math.tan(x), Math.atan(x));
  }
  return hex(new Uint8Array(new Float64Array(r).buffer));
}

// CSS math functions, through registered custom properties (their computed
// values are the calculated numbers and angles). mod(), rem() and round()
// amplify the result so that its last bits show in the six digits CSS
// serializes.
const CSS_NUMBER_MATH = [
  'mod(sin(1.2345) * 1e16, 1000)', 'mod(cos(1.2345) * 1e16, 1000)',
  'mod(tan(0.777) * 1e16, 1000)', 'mod(sin(37.3deg) * 1e16, 1000)',
  'mod(pow(9.628, -4.64) * 1e20, 1000)', 'mod(exp(-7.77) * 1e19, 1000)',
  'mod(log(3.3) * 1e16, 1000)', 'mod(log(7, 2) * 1e15, 1000)',
  'mod(hypot(3, 4.5) * 1e15, 1000)', 'mod(sqrt(2) * 1e16, 1000)',
  'rem(exp(3.3) * 1e14, 1000)',
  'calc(sin(0.7) * 1e16 - round(down, sin(0.7) * 1e16, 1000))',
];
const CSS_ANGLE_MATH = [
  'mod(asin(0.3) * 1e14, 1000deg)', 'mod(acos(0.3) * 1e14, 1000deg)',
  'mod(atan(1.0078749403766563) * 1e14, 1000deg)',
  'mod(atan2(1.0078749403766563, 1.5) * 1e14, 1000deg)',
];

function cssMath() {
  if (!window.CSS || !CSS.registerProperty) {
    return 'absent';
  }
  for (const [name, syntax, initialValue] of [
    ['--moltfp-n', '<number>', '0'], ['--moltfp-a', '<angle>', '0deg']]) {
    try {
      CSS.registerProperty({name, syntax, inherits: false, initialValue});
    } catch (e) {
      // Already registered (the harness collects twice in one page).
    }
  }
  const el = document.createElement('div');
  document.body.appendChild(el);
  const out = [];
  try {
    for (const [prop, list] of [['--moltfp-n', CSS_NUMBER_MATH],
      ['--moltfp-a', CSS_ANGLE_MATH]]) {
      for (const expr of list) {
        el.style.removeProperty(prop);
        el.style.setProperty(prop, expr);
        out.push(getComputedStyle(el).getPropertyValue(prop).trim());
      }
    }
  } finally {
    el.remove();
  }
  return out;
}

// DOMMatrix, SVGMatrix, CSSOM toMatrix(), a canvas transform and an SVG
// path's arcs: rotations and skews computed from sines, cosines and
// tangents, read back as doubles.
function domMatrixMath() {
  const v = [];
  v.push(...new DOMMatrix().rotate(33.3).rotateAxisAngle(1, 2, 3, 47.1)
      .skewX(12.7).skewY(-5.3).rotateFromVector(3, 7).rotate(10, 20, 30)
      .toFloat64Array());
  v.push(...new DOMMatrix(
      'rotate(33.3deg) skew(12deg, 3deg) rotate3d(1, 1, 0, 0.7rad)')
      .toFloat64Array());
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  const s = svg.createSVGMatrix().rotate(21.7).skewX(9.1).skewY(-3.3)
      .rotateFromVector(2, 9);
  v.push(s.a, s.b, s.c, s.d, s.e, s.f);
  if (window.CSSRotate && window.CSSSkew) {
    v.push(...new CSSRotate(CSS.deg(37.1)).toMatrix().toFloat64Array());
    v.push(...new CSSRotate(CSS.number(1), CSS.number(2), CSS.number(3),
        CSS.rad(0.7)).toMatrix().toFloat64Array());
    v.push(...new CSSSkew(CSS.deg(11), CSS.deg(-7)).toMatrix()
        .toFloat64Array());
  }
  const g = document.createElement('canvas').getContext('2d');
  g.rotate(0.7);
  g.transform(1, 0.2, 0.3, 1, 5, 6);
  g.rotate(-1.3);
  v.push(...g.getTransform().toFloat64Array());
  const path = document.createElementNS('http://www.w3.org/2000/svg',
      'path');
  path.setAttribute('d', 'M10 10 A 30 50 23 0 1 162.55 162.45 ' +
      'a 25 25 -30 1 0 50 -25');
  svg.appendChild(path);
  document.body.appendChild(svg);
  try {
    const p = path.getPointAtLength(123.4);
    v.push(p.x, p.y, path.getTotalLength());
  } finally {
    svg.remove();
  }
  return hex(new Uint8Array(new Float64Array(v).buffer));
}

// Where an edge lies along a line: the index (0 to 128) of the first of 129
// points, `step` apart from (x, y) - 64 steps along (dx, dy), whose test
// differs from the first point's, or -1. Arcs, ellipses, strokes, rounded
// shapes and superellipse corners are computed with sines, cosines, powers
// and cube roots, so their edges move with those functions' last bits; a
// step of one float ulp (canvas) or one layout unit (elements) reads them.
function edgeIndex(test, x, y, dx, dy, step) {
  const first = test(x - 64 * step * dx, y - 64 * step * dy);
  for (let k = -63; k <= 64; ++k) {
    if (test(x + k * step * dx, y + k * step * dy) !== first) {
      return k + 64;
    }
  }
  return -1;
}

function indexHash(list) {
  return hex(new Uint8Array(new Int16Array(list).buffer));
}

// Hit testing a page reads without pixels: isPointInPath() on arcs and
// rotated ellipses, isPointInStroke() on curves with round, miter and bevel
// joins, caps and dashes, and elementFromPoint() over clip-path shapes and
// corner-shape corners. MoltNet builds them with the portable functions.
async function geomHitTesting() {
  const ctx = document.createElement('canvas').getContext('2d');
  const ulp = 2 ** -17;  // a float ulp for coordinates from 64 to 128
  const edges = [];
  for (let i = 0; i < 40; ++i) {
    const cx = 96 + (i % 5) * 0.37;
    const cy = 96 + (i % 7) * 0.21;
    const r = 15 + i * 0.613;
    const a0 = i * 0.377 - 3;
    const a1 = a0 + 0.4 + i * 0.151;
    const rot = i * 0.21;
    const ry = r * 0.7;
    const p = new Path2D();
    p.moveTo(cx, cy);
    if (i % 2) {
      p.arc(cx, cy, r, a0, a1, false);
    } else {
      p.ellipse(cx, cy, r, ry, rot, a0, a1, false);
    }
    p.closePath();
    for (const t of [0.17, 0.5, 0.83]) {
      const th = a0 + (a1 - a0) * t;
      let x;
      let y;
      let nx;
      let ny;
      if (i % 2) {
        nx = Math.cos(th);
        ny = Math.sin(th);
        x = cx + r * nx;
        y = cy + r * ny;
      } else {
        const ex = r * Math.cos(th);
        const ey = ry * Math.sin(th);
        x = cx + ex * Math.cos(rot) - ey * Math.sin(rot);
        y = cy + ex * Math.sin(rot) + ey * Math.cos(rot);
        const gx = ex / (r * r);
        const gy = ey / (ry * ry);
        nx = gx * Math.cos(rot) - gy * Math.sin(rot);
        ny = gx * Math.sin(rot) + gy * Math.cos(rot);
        const n = Math.hypot(nx, ny);
        nx /= n;
        ny /= n;
      }
      edges.push(edgeIndex((px, py) => ctx.isPointInPath(p, px, py), x, y,
          nx, ny, ulp));
    }
  }
  // Strokes of cubic curves (their maximum-curvature points are cube roots)
  // and of arcs, with every join and cap, and dashed.
  const curve = new Path2D();
  curve.moveTo(70, 100);
  curve.bezierCurveTo(140, 20, 50, 30, 120, 110);
  curve.bezierCurveTo(90, 125, 75, 70, 100, 90);
  curve.arc(96, 96, 21.3, 0.3, 2.9);
  const bez = (t, p0, p1, p2, p3) => {
    const u = 1 - t;
    return [u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] +
        t * t * t * p3[0], u * u * u * p0[1] + 3 * u * u * t * p1[1] +
        3 * u * t * t * p2[1] + t * t * t * p3[1]];
  };
  const d = (t, p0, p1, p2, p3) => {
    const u = 1 - t;
    return [3 * u * u * (p1[0] - p0[0]) + 6 * u * t * (p2[0] - p1[0]) +
        3 * t * t * (p3[0] - p2[0]), 3 * u * u * (p1[1] - p0[1]) +
        6 * u * t * (p2[1] - p1[1]) + 3 * t * t * (p3[1] - p2[1])];
  };
  const seg = [[70, 100], [140, 20], [50, 30], [120, 110]];
  let k = 0;
  for (const join of ['round', 'miter', 'bevel']) {
    for (const cap of ['butt', 'round', 'square']) {
      for (const dash of [[], [7.7, 3.1, 1.3, 5.5]]) {
        ctx.lineWidth = 5.3 + k * 0.37;
        ctx.lineJoin = join;
        ctx.lineCap = cap;
        ctx.miterLimit = 3 + k * 0.5;
        ctx.setLineDash(dash);
        ctx.lineDashOffset = 1.7;
        for (const t of [0.13, 0.41, 0.66, 0.89]) {
          const [x, y] = bez(t, ...seg);
          const [tx, ty] = d(t, ...seg);
          const n = Math.hypot(tx, ty);
          for (const side of [1, -1]) {
            const nx = side * -ty / n;
            const ny = side * tx / n;
            edges.push(edgeIndex(
                (px, py) => ctx.isPointInStroke(curve, px, py),
                x + nx * ctx.lineWidth / 2, y + ny * ctx.lineWidth / 2, nx, ny,
                ulp));
          }
        }
        ++k;
      }
    }
  }
  // Elements: clip-path shapes and corner-shape corners, a layout unit
  // (1/64 px) at a time.
  const lu = 1 / 64;
  const host = document.createElement('div');
  host.style.cssText = 'position:fixed;left:0;top:0;width:640px;' +
      'height:200px;z-index:2147483647';
  document.body.appendChild(host);
  const shapes = [
    ['clip-path', 'polygon(round 11.3px, 10% 5%, 93% 17%, 71% 91%, 7% 77%)'],
    ['clip-path', 'circle(closest-side at 37% 61%)'],
    ['clip-path', 'ellipse(41.3% 37.7% at 50% 50%)'],
    ['clip-path', 'shape(from 10% 10%, arc to 90% 20% of 70% 40% ' +
        'rotate 17deg large cw, line to 80% 90%, close)'],
    ['corner-shape', 'superellipse(2.3)'],
    ['corner-shape', 'superellipse(-0.7)'],
    ['corner-shape', 'squircle'],
  ];
  try {
    const boxes = [];
    shapes.forEach(([prop, value], i) => {
      const el = document.createElement('div');
      el.style.cssText = 'position:absolute;top:10px;width:83px;height:91px;' +
          'background:#888;border-radius:37px 29px 41px 23px';
      el.style.left = (10 + i * 89) + 'px';
      el.style.setProperty(prop, value);
      host.appendChild(el);
      boxes.push(el);
    });
    boxes.forEach((el, i) => {
      if (!el.style.getPropertyValue(shapes[i][0])) {
        edges.push(-2);  // the property isn't supported
        return;
      }
      const r = el.getBoundingClientRect();
      for (let j = 0; j < 12; ++j) {
        const th = j * Math.PI / 6 + 0.123;
        const nx = Math.cos(th);
        const ny = Math.sin(th);
        // Out from the centre, to where the edge is about.
        let lo = 0;
        let hi = Math.max(r.width, r.height);
        const cx = r.left + r.width / 2;
        const cy = r.top + r.height / 2;
        const hit = (px, py) => document.elementFromPoint(px, py) === el;
        for (let s = 0; s < 20; ++s) {
          const mid = (lo + hi) / 2;
          if (hit(cx + mid * nx, cy + mid * ny)) {
            lo = mid;
          } else {
            hi = mid;
          }
        }
        edges.push(edgeIndex(hit, cx + lo * nx, cy + lo * ny, nx, ny, lu));
        edges.push(Math.round(lo * 64));
      }
    });
  } finally {
    host.remove();
  }
  return indexHash(edges);
}

// Lengths and positions along paths: SVG shapes' getTotalLength() and
// getPointAtLength(), and offset-path positions (and offset-rotate: auto)
// on basic shapes, shape() arcs and rounded polygons, read back through
// getBoundingClientRect().
function geomPathLength() {
  const v = [];
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('width', '300');
  svg.setAttribute('height', '300');
  const make = (tag, attrs) => {
    const el = document.createElementNS(ns, tag);
    for (const [a, b] of Object.entries(attrs)) {
      el.setAttribute(a, b);
    }
    svg.appendChild(el);
    return el;
  };
  const shapes = [
    make('circle', {cx: 50.3, cy: 61.7, r: 37.7}),
    make('ellipse', {cx: 150.1, cy: 70.9, rx: 41.3, ry: 17.9}),
    make('rect', {x: 10.5, y: 120.25, width: 97.3, height: 61.1, rx: 13.7,
      ry: 9.1}),
    make('path', {d: 'M 20 200 C 90 120 140 290 210 210 Q 250 170 280 240 ' +
        'A 31 19 37 1 0 230 280 Z'}),
  ];
  const host = document.createElement('div');
  host.style.cssText = 'position:absolute;left:0;top:0;width:400px;' +
      'height:400px';
  document.body.appendChild(host);
  host.appendChild(svg);
  try {
    for (const s of shapes) {
      const len = s.getTotalLength();
      v.push(len);
      for (let i = 0; i <= 8; ++i) {
        const p = s.getPointAtLength(len * i / 8);
        v.push(p.x, p.y);
      }
    }
    const paths = [
      'circle(43% at 40% 60%)', 'ellipse(31% 23% at 50% 50%)',
      'polygon(round 9.7px, 10% 10%, 90% 20%, 60% 95%)',
      'inset(7% 9% 11% 13% round 17px 9px)',
      'shape(from 0% 0%, arc to 100% 50% of 60% 40% rotate 17deg, ' +
          'curve to 0% 100% with 50% 120% / 10% 80%, close)',
      'path("M 10 10 A 37 23 31 1 1 170 90 Z")',
    ];
    for (const path of paths) {
      const el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:30px;top:20px;width:7px;' +
          'height:5px;offset-rotate:auto';
      el.style.setProperty('offset-path', path);
      host.appendChild(el);
      if (!el.style.getPropertyValue('offset-path')) {
        v.push(-1);
        continue;
      }
      for (const dist of ['0%', '13.7%', '41%', '66.6%', '97%']) {
        el.style.offsetDistance = dist;
        const r = el.getBoundingClientRect();
        v.push(r.left, r.top, r.width, r.height);
      }
    }
  } finally {
    host.remove();
  }
  return hex(new Uint8Array(new Float64Array(v).buffer));
}

// shape-outside: the renderer draws the float's image, gradient or path on
// the CPU and lays the text out around its alpha (no farbling on the way):
// gradients with colour hints and in OKLab, an SVG image with a blur, a
// hue rotation turned into alpha, a gamma transfer and a lit surface, a
// rounded polygon, and a margin-box with radii and shape-margin. The
// inline boxes' positions are read back.
async function geomShapeOutside() {
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="150" ' +
      'height="160"><defs><linearGradient id="g"><stop offset="0" ' +
      'stop-color="#f30"/><stop offset="1" stop-color="#09f"/>' +
      '</linearGradient><filter id="b"><feGaussianBlur stdDeviation="1.3"/>' +
      '<feComponentTransfer><feFuncA type="gamma" amplitude="1.1" ' +
      'exponent="0.73" offset="0"/></feComponentTransfer></filter>' +
      '<filter id="h"><feColorMatrix type="hueRotate" values="37.3"/>' +
      '<feColorMatrix type="luminanceToAlpha"/></filter><filter id="l">' +
      '<feDiffuseLighting surfaceScale="3.3" diffuseConstant="1.1">' +
      '<feDistantLight azimuth="37" elevation="23"/></feDiffuseLighting>' +
      '<feColorMatrix type="luminanceToAlpha"/></filter></defs>' +
      '<circle cx="70" cy="40" r="33.3" filter="url(#b)"/>' +
      '<rect x="0" y="80" width="150" height="40" fill="url(#g)" ' +
      'filter="url(#h)"/><circle cx="60" cy="140" r="17" filter="url(#l)"/>' +
      '</svg>';
  const url = 'data:image/svg+xml,' + encodeURIComponent(svg);
  try {
    const img = new Image();
    img.src = url;
    await img.decode();
  } catch (e) {
    // Laid out without the image if it can't be decoded.
  }
  const floats = [
    'shape-outside:linear-gradient(105deg, black, 37.3%, transparent);' +
        'shape-image-threshold:0.31',
    'shape-outside:radial-gradient(in oklab, rgb(0 0 0 / 0.9), 23%, ' +
        'rgb(255 0 0 / 0.1));shape-image-threshold:0.47',
    'shape-outside:url("' + url + '");shape-image-threshold:0.27',
    'shape-outside:polygon(round 13px, 0 0, 100% 10%, 40% 50%, 90% 95%, ' +
        '0 100%);shape-margin:3.7px',
    'shape-outside:margin-box;border-radius:41px 17px 53px 29px;' +
        'shape-margin:7.3px;margin:3px',
    'shape-outside:circle(closest-side at 40% 45%);shape-margin:2.1px',
  ];
  const host = document.createElement('div');
  host.style.cssText = 'position:absolute;left:0;top:0;width:300px;' +
      'line-height:4px;font-size:4px';
  document.body.appendChild(host);
  const v = [];
  try {
    const blocks = floats.map((style) => {
      const block = document.createElement('div');
      block.style.cssText = 'width:300px;height:170px;overflow:hidden';
      const f = document.createElement('div');
      f.style.cssText = 'float:left;width:150px;height:160px;' + style;
      block.appendChild(f);
      for (let i = 0; i < 40 * 30; ++i) {
        const s = document.createElement('span');
        s.style.cssText = 'display:inline-block;width:3px;height:4px';
        block.appendChild(s);
      }
      host.appendChild(block);
      return block;
    });
    await new Promise((r) => requestAnimationFrame(() =>
      requestAnimationFrame(r)));
    for (const block of blocks) {
      const top = block.getBoundingClientRect().top;
      let line = null;
      for (const s of block.querySelectorAll('span')) {
        const r = s.getBoundingClientRect();
        if (r.top !== line) {
          line = r.top;
          v.push(r.left, r.top - top);
        }
      }
    }
  } finally {
    host.remove();
  }
  return hex(new Uint8Array(new Float64Array(v).buffer));
}

// CSS contrast-color() over the colours where black and white are about
// equally readable: the choice follows the colours' relative luminance.
function geomContrastColor() {
  const el = document.createElement('div');
  document.body.appendChild(el);
  const out = [];
  try {
    el.style.color = 'contrast-color(red)';
    if (!el.style.color) {
      return 'absent';
    }
    for (let g = 104; g <= 132; ++g) {
      for (const [r, b] of [[g, g], [g + 31, g - 23], [g - 40, g + 61]]) {
        el.style.color = 'contrast-color(rgb(' + r + ' ' + g + ' ' + b + '))';
        out.push(getComputedStyle(el).color === 'rgb(0, 0, 0)' ? 'b' : 'w');
      }
    }
  } finally {
    el.remove();
  }
  return out.join('');
}

// Web Audio: BiquadFilterNode and IIRFilterNode frequency responses (read
// as they are computed) and a compressor's gain reduction after rendering a
// sawtooth through it.
async function audioMath() {
  if (!window.OfflineAudioContext) {
    return 'absent';
  }
  const ctx = new OfflineAudioContext(1, 4410, 44100);
  const freqs = new Float32Array(64);
  for (let i = 0; i < freqs.length; ++i) {
    freqs[i] = 20 + i * 337.5;
  }
  const chunks = [];
  const response = (node) => {
    const mag = new Float32Array(freqs.length);
    const phase = new Float32Array(freqs.length);
    node.getFrequencyResponse(freqs, mag, phase);
    chunks.push(new Uint8Array(mag.buffer), new Uint8Array(phase.buffer));
  };
  for (const type of ['lowpass', 'highpass', 'bandpass', 'lowshelf',
    'highshelf', 'peaking', 'notch', 'allpass']) {
    response(new BiquadFilterNode(ctx, {type, frequency: 1234.5, Q: 3.3,
      gain: 7.7, detune: 33}));
  }
  response(new IIRFilterNode(ctx, {feedforward: [0.1, 0.2, 0.1],
    feedback: [1, -0.5, 0.25]}));
  const osc = new OscillatorNode(ctx, {type: 'sawtooth', frequency: 440});
  const comp = new DynamicsCompressorNode(ctx, {threshold: -40, knee: 12,
    ratio: 8, attack: 0.003, release: 0.1});
  osc.connect(comp).connect(ctx.destination);
  osc.start();
  await ctx.startRendering();
  return {response: (await hashChunks(chunks)).hash,
    reduction: comp.reduction};
}

// A 64x32 sRGB frame, as RGBA and as I420, read back in Display P3: Skia
// converts the colours on the CPU.
async function p3Readback() {
  if (!window.VideoFrame) {
    return 'absent';
  }
  const w = 64;
  const h = 32;
  const rgba = new Uint8Array(w * h * 4);
  const i420 = new Uint8Array(w * h * 3 / 2);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      rgba.set([x * 4, y * 8, (x * y) & 255, 255], (y * w + x) * 4);
      i420[y * w + x] = (x * 3 + y * 5) & 255;
    }
  }
  for (let i = w * h; i < i420.length; ++i) {
    i420[i] = (i * 7) & 255;
  }
  const srgb = {primaries: 'bt709', transfer: 'iec61966-2-1', matrix: 'rgb',
    fullRange: true};
  const frames = {
    rgba: new VideoFrame(rgba, {format: 'RGBA', codedWidth: w,
      codedHeight: h, timestamp: 0, colorSpace: srgb}),
    i420: new VideoFrame(i420, {format: 'I420', codedWidth: w,
      codedHeight: h, timestamp: 0}),
  };
  const out = {};
  try {
    for (const [name, frame] of Object.entries(frames)) {
      const dest = new Uint8Array(w * h * 4);
      await frame.copyTo(dest, {format: 'RGBA', colorSpace: 'display-p3'});
      out[name] = await hex(dest);
    }
  } finally {
    Object.values(frames).forEach((f) => f.close());
  }
  return out;
}

// A quarter second of stereo 48 kHz Vorbis (a sweep and two tones,
// libvorbis at its lowest quality): the three headers, Xiph-laced (with an
// empty comment header), and the audio packets.
const VORBIS_DESCRIPTION =
    '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' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAACAAAcAgAALodCQFQFAnACAwXEsCwAAHEnSLAAA' +
    'cCRL0wAAwNI0UQQAAEvTRBEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAwIADAECACWWg0JCVAEAUAIBBMTwNYFkA' +
    'ywJoGkDTAJ4H8DyAKAIAAQAABQ4AAAE2aEosDlBoyEoAIAoAwKAolmVZngdN0zRRhKZpmihC' +
    '0zRPFKFpmiaKEEXPM014oueZJkxTFE0TiKJpCgAAKHAAAAiwQVNicYBCQ1YCACEBAAZHsSxP' +
    '8zzPE0XTVFVomueJoiiKpmmqKjTN80RRFE3TNFUVmuZ5oiiKpqmqqgpN8zxRFEXTVFVVheeJ' +
    'oiiapmmqquvC80RRFE3TNFXVdSGKomiapqmqquu6QBRN0zRVVVVdF4iiaZqmqrquLANRNE3T' +
    'VFXXlWVgmqqqqqrrurIMUE1VVVXXlWWAqrqq67quLANUVXVd15VlGeC6ruvKsmzbAFzXdWXZ' +
    'tgUAABw4AAAEGEEnGVUWYaMJFx6AQkNWBABRAACAMUwpppRhTEIoITSKSQgphExKSqmVVEFI' +
    'JaVSKgippFRKRqWllFLKIJRSUioVhFRKKqUAALADBwCwAwuh0JCVAEAeAABBiFKMMcaclFIp' +
    'xpxzTkqpFGPOOSelZIwx55yTUjLGmHPOSSkdc84556SUjDnnnHNSSuecc845KaWUzjnnnJRS' +
    'Sgidc05KKaVzzjknAACowAEAIMBGkc0JRoIKDVkJAKQCABgcx7I0TdM8TxQ1SdI0z/M8UTRN' +
    'TbI0zfM8TxRNk+d5niiKommqKs/zPFEURdNUVa4riqZpmqqqqmRZFEXRNFVVdWGapqmqquq6' +
    'ME1RVFXVdV3IsmmqquvKMmzbNFXVdWUZqKqqyq4sA9dVVdeVZQEA4AkOAEAFNqyOcFI0Flho' +
    'yEoAIAMAgCAEIaUUQkophJRSCCmlEBIAADDgAAAQYEIZKDRkRQAQJwAAICSlgk5KJaGUUkop' +
    'pZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJK' +
    'KaWUUkoppZRSSimlk1JKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRS' +
    'SimllJJSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSSmllFJKKaWUUkoppZRSSiml' +
    'lFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkop' +
    'pZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJK' +
    'KaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRS' +
    'SimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWU' +
    'CgDQjXAA0H0woQwUGrISAEgFAACMUYoxCKnFViHEmHMSWmutQogx5yS0lGLPmHMQSmkttp4x' +
    'xyCUklqLvZTOSUmttRh7Kh2jklJLMfbeSyklpdhi7L2nkEKOLcbYe88xpRZbq7H3XmNKsdUY' +
    'Y++99xhjq7HW3nvvMbZWa44FAGA2OABAJNiwOsJJ0VhgoSErAYCQAADCGKUYY8w555xzTkrJ' +
    'GHPOQQghhBBKKRljzDkIIYQQQiklY845ByGEUEIopWTMOegghFBCKKWUzjkHHYQQQgmllJIx' +
    '5yCEEEIJpZRSOucghBBCKKWEVEopnYMQQighhFJKSSmEEEIIoYRQUikphRBCCCGEUEJKJaUQ' +
    'QgghhBBKSKWklFIIIYQQQgillJRSCiWUEEIooaSSSimlhBBKCKGkVFIqqZQSQgglhJJKSimV' +
    'VEooIYRSAADAgQMAQIARdJJRZRE2mnDhASg0ZCUAEAUAABkHHZSWG4CQctRahxyEFFsLkUMM' +
    'WoydcoxBSilkkDHGpJWSQscYpNRiS6GDFHvPuZXUAgAAIAgACDABBAYICr4QAmIMAEAQIjNE' +
    'QmEVLDAogwaHeQDwABEhEQAkJijSLi6gywAXdHHXgRCCEIQgFgdQQAIOTrjhiTc84QYn6BSV' +
    'OhAAAAAAAAUAeAAAQCiAiIhmrsLiAiNDY4Ojw+MDRAAAAAAApADgAwAACQEiIpq5CosLjAyN' +
    'DY4Ojw+QAABAAAEAAAAAEEAAAgICAAAAAAABAAAAAgI=';
const VORBIS_PACKETS = [
  'hPfq2dx4r57tN/dPXpu6BQAQAADavyR8dzJIAA==',
  'mgrfqlfharj7NTjdG2qKv/EH+RDOyTuTVt8B0dUU/Rn9PK98+nbyWfn24f0Hf/XHvz65ow' +
      'WAAAAAAAAAAAAAAAAA6vPd3Pm525d8gontlY8hy7Kskycj',
  'nvpe8w8BdH9364R3FDmoKbbCD/UujEnH72j5i66m6Ebe/vJ1Prb3BgAAAAAAAAAAAAAAAO' +
      'AKYeoXTXOwAbD68IOn3rv0kW+92QA=',
  'vvo+B+4COLpHR3feUFNchR8b3AU0ftotb1r+pk9XU4R+I7409t3tuvywfBS9BwEAAAAAAA' +
      'AAAAAAAHTiidOUxNfe928PgGS5LwM=',
  'vvo+xEMAvjz7PwOr97dATfE1fg7cBbD9PsoC75yNUVOEjv7KJ/8NINblw601AAAAAAAAAA' +
      'AAAABAu+yB4Gnc00gT1vaNP3w15/IPLwAA',
  'vvqej5fgN+D7c4v3e46Ir6b4Gj/tuwB67T7y8413lI6qn5oidAK4ZbIGAAAAAAAAAAAAAA' +
      'CAfvEyAEwi8aOv0sGcO+p22xL32dDw+iIZAACKGQA=',
  'vvoenx+C3/9Z7Lj5vgdRT6Wm+Brf2jcBQYHcR/33xDvngCispKYInQB2AAAAAAAAAAAAAA' +
      'DAGD/rAED1/1UYq1QAAAAVh/2XPz/8ap2ch3ardbv9Aw==',
  'vvqeny/B5/97Zhf+vdlv0Stqiq/x1XoKoOs9rXODd5SO/zU1RegEEBMAAAAAAAAAAAAAAN' +
      '1nAQDD68avX9MAAADA0u10O93893f3mejp7O2k+QA=',
  'vvoeH1+C72BzpH8LnmLkn5ria7yrHwKqB0+78V3yjjLio/1KTRFaAQCLAAAAAAAAAAAAAA' +
      'BAf/kSAFb3XgxDMwAAgI/b93/3fiUSac5ye/wA',
  'vvquz2++DXvdX+EpPuzvUlN8jRf+W0Bl56PKsneUPX4RpaYInQBQJgAAAAAAAAAAAAAAhp' +
      'cAgLVlbOMc0gAAADT3W79vX1+3gNP72/vbWTROBQ==',
  'vvo2r998Mvba/3Z+3nLdqSm+xi1iMwmo/njaTXn/fKOM36dTU4RWAEASAwAAAAAAAAAAAA' +
      'Cgv3QJAEfnlKaLAgAAADnS7bj7AOT89n7yZ/54CxI=',
  'vvrWr9982vfaPzlvUZaLmuJrfDj8JSAq22k3Ff5+R+nf+0VNEToBgE0EAAAAAAAAAAAAAB' +
      'h+BgCo/mfGjskAAADQr37zTw5tCw6/SCueBAA=',
  'nvpmr9/ccp7a10Z7y83J0TXF17eKN4uAAHHfA/x4e4ft81FTdMPl8/28pcWCBUZr1gAAAA' +
      'AAAAAAAACAvf//PLHh8Fit+uGF1eloRfjy/e12c7+73dzpxEtMABz2X74SRw2iq6dOf+uD' +
      'D15YRwA=',
  'nuiNzx/hZ2Kv/eLpLVeNmuKg18b/W8Bf/2f34al4O80Vaoqu25wZmMoEAyICAAAAAADgoY' +
      'Sr1xJ+vdxVZyhvd4h1f7y+8GBciNMX3P/m/tf908dPHz/9dLD9YjiuV3eTPrkfNj2iJ+e3' +
      '9q09aQqQDh74stOmd/1NzTSrBQkF',
];

// The Vorbis packets decoded by WebCodecs, as 32-bit float planes.
async function decodedVorbis() {
  if (!window.AudioDecoder) {
    return 'absent';
  }
  const outputs = [];
  const decoder = new AudioDecoder({output: (d) => outputs.push(d),
    error: () => {}});
  try {
    decoder.configure({codec: 'vorbis', sampleRate: 48000,
      numberOfChannels: 2, description: base64Bytes(VORBIS_DESCRIPTION)});
    VORBIS_PACKETS.forEach((p, i) => decoder.decode(new EncodedAudioChunk(
        {type: 'key', timestamp: i * 10000, data: base64Bytes(p)})));
    await decoder.flush();
    const planes = [];
    let frames = 0;
    for (const d of outputs) {
      for (let ch = 0; ch < d.numberOfChannels; ++ch) {
        const plane = new Float32Array(d.numberOfFrames);
        d.copyTo(plane, {planeIndex: ch, format: 'f32-planar'});
        planes.push(new Uint8Array(plane.buffer));
      }
      frames += d.numberOfFrames;
    }
    return {frames, hash: (await hashChunks(planes)).hash};
  } finally {
    outputs.forEach((d) => d.close());
    if (decoder.state !== 'closed') {
      decoder.close();
    }
  }
}

// MoltNet: what pages can learn about the processor (FINGERPRINTING.md).
// The bits Float16Array and DataView.setFloat16() store for NaNs with
// payloads, as the page computes them at run time (a payload NaN from a
// Float64Array, the same times one, and negated), before and after V8's
// optimizing compilers take the storing functions. MoltNet's x86-64 V8
// converts in software on every CPU (no F16C), which keeps the sign and
// stores the canonical NaN (0x7e00, 0xfe00); arm64's optimized code converts
// with the base instruction set's FCVT, which keeps the payload's top bits,
// the same on every arm64 CPU.
const F16_NANS = [[0x7ff80000, 0], [0xfff80000, 0], [0x7ffc0000, 0x12345678],
  [0x7ff81234, 0x9abcdef0], [0xfffdeadb, 0xeef00d00], [0x7ff40000, 1],
  [0xfff00000, 0x400]];

function float16Nan() {
  if (typeof Float16Array === 'undefined' ||
      typeof DataView.prototype.setFloat16 !== 'function') {
    return 'absent';
  }
  const f64 = new Float64Array(1);
  const u32 = new Uint32Array(f64.buffer);
  const one = Number('1');
  const values = [];
  for (const [hi, lo] of F16_NANS) {
    u32[1] = hi;
    u32[0] = lo;
    const x = f64[0];
    values.push(x, x * one, -x);
  }
  const signs = values.map((x) => {
    f64[0] = x;
    return u32[1] >>> 31;
  });
  const store = (a, i, x) => {
    a[i] = x;
  };
  const setv = (dv, i, x) => {
    dv.setFloat16(i * 2, x, true);
  };
  const run = () => {
    const a = new Float16Array(values.length);
    const dv = new DataView(new ArrayBuffer(values.length * 2));
    for (let i = 0; i < values.length; ++i) {
      store(a, i, values[i]);
      setv(dv, i, values[i]);
    }
    const u = new Uint16Array(a.buffer);
    const array = [];
    const view = [];
    for (let i = 0; i < values.length; ++i) {
      array.push(u[i]);
      view.push(dv.getUint16(i * 2, true));
    }
    return {array, view};
  };
  const first = run();
  // Ordinary values until the optimizing compilers take both functions.
  const warm = new Float16Array(64);
  const warmView = new DataView(new ArrayBuffer(128));
  for (let n = 0; n < 200000; ++n) {
    store(warm, n & 63, n * 0.25);
    setv(warmView, n & 63, n * 0.5);
  }
  const hot = run();
  const hexList = (list) => list.map((h) => h.toString(16).padStart(4, '0'))
      .join(' ');
  const canonical = (list) => list.every((h, i) =>
    h === (signs[i] ? 0xfe00 : 0x7e00));
  return {
    bits: {array: hexList(first.array), array_optimized: hexList(hot.array),
      dataview: hexList(first.view),
      dataview_optimized: hexList(hot.view)},
    canonical: canonical(first.array) && canonical(hot.array) &&
        canonical(first.view) && canonical(hot.view),
  };
}

// A 96 x 64 synthetic picture with alpha (gradients, edges, strokes, noise),
// lossy JPEG XL (VarDCT in XYB) made for the kit with libjxl: decoded by
// jxl-rs, whose default code gives other pixels with SSE4.2, AVX2 and NEON
// (MoltNet: its scalar code and portable math on every CPU).
const JXL_PROBE =
    '/woPwU4AUIAE+QMhaxUAgAqVUcYNXs71fPmhw2LaxnWGttq2sIQTlG2ra9DlioV1BxLTWvj9' +
    'w1JQmNbCcwoKCgoKjmisVoGRUURBgQrRdIzEGrt2jGiMdox2jHaMdYxtjGqMxxjjPZe6As4x' +
    'RsSPwWpXuSBdEGMMYxiDeXcAqFAAgl7+Qd3o+k+rsPu/7G+pKKooQQGIPcz2lf1jIJmmftIN' +
    'LKuxgDTGGKMha4wxBgrSGGOMEKIxxhgUh2OMMeyExxhjfECOMcYAJBnjZNA4Rir6GGM0CRqf' +
    'AJ9Fqdgm1DLGIf8aQi9bz1tU5g4yZTjkTBnHAX9uqnzb7zfe+iOpPE1+F+tmc8k8HobBhGcM' +
    'JEAQHaoQ8swS4dcr/AK5vkScN8XqL3Akh5n0m30GOHd22OzQ1xW0Wwv3QdvsenuBInQy2hEq' +
    'AxfTlnNwJeRymazhoUudMYr2zeLGJNvjjEIhwjW0ilIKNtCRbLiYE8izOg6NSRuYlvFCuMPg' +
    'JuFn2S1JYpRSLEWoGkEpPkz0whjFBOudxFF3JNk9tskC7K0LFC2AXzLRgCLi7ur4yNM9aOSV' +
    'XDYKIFvEiWAQ5f3//1/8P/0ZLvja7+vk//////2/BSAQkR9B/oGrwGB9dAv8f/xh/P/9//8f' +
    '/994fgnj/+X8v/0/29bTpkOKHBr2jqn34BiIQnMa4Q79awQUNaa+G+icFBSo0d79xLg0F4At' +
    'kcfLi3cZY4wTJ2PMlHQqAUCSaUGvAfjjg7q0x7cFyMCCFDj/2xrVT7CUUJC3dG0H7z9TuPPz' +
    '5p9h3Ar/PjsFMSTjr3k4lAZr+25NcHWuHLeLFgZGUgRJLhREOPtAcIIYJ2c//iZOj0w3Nuwi' +
    'B7oFPsyLjiYJjXVBM2zsPlTNmkzPDEDzdaNn/hkgQ+Lz1sC2pko0oloanYEbRdkXpHOo/fXc' +
    'uaF0GMbWtDLZHpDJJqYDN4YHqzvze4mF6OZRnI1u/vAfKnkE4gsAcAS/szunRtriKEaLMi31' +
    'r4cve6+7AfPf+O0gZLNOemC4puZCJz9ARwrOmunQv4PwU4CMWZ5tkiDbgMkrEMQmcha43/EP' +
    '5sd0ei4NAvcGQhcm2Adq85UTkwInUGg7Lujdyhgdy5OBAgnDpJeqFSA/Z/u3J3j9NVD/ZTpG' +
    'TaAVr+nSHrk334S2cW58u6sxS6bjFbgrboTAWdVcrVMnvrf5IpXAP1X1su1ODeNqJ7QdRd4y' +
    'zRWrECwiQRrAyCRdvLv2quJUdIKV1S2zihrVrQFWgWGe+MLEhKCiMbSRIfz8B+6Vb9W3uvDT' +
    'BD6lXCCZwOlz6O/ObN2HB8yZWttGem1q8r8HjVXqJHejRDIBk/7AACh1zqxng4JKCboFEyj3' +
    'wW35NrTewzAAy/m9y+MiAb7Q7x8CibrO9c+8Omc4dtvP7vXUKj8EZwg2aa4q4RIVnqEHSq6O' +
    'h90IBPxfmWkNt27JGA/JvW1mRe+9ymQAt0YRg2asJlaF1nLPhaABvS/sgREJwFcw27wnM41K' +
    'wHUV1v/aHjCRTw8oYT1UJI/Lx1r098U/4eIpLcPR/Hatigez4AA++RGFDyhskgB1g56tTGch' +
    'CQZ8N/e5Avs5U1X4M1yPhBiuwPB5i2c7SeltXQFOBGV3MFcGR9A1TqMfIaTiCbpv9DZy730h' +
    'Il873qpFkLYTlFCSfV/fTrBr3CTAGGdGLdAFqaiM/MI4UgduiVBkKw1Ug63iarQ=';

// The JPEG XL image decoded by WebCodecs' ImageDecoder (decoded pixels, not
// farbled), as the frame's own planes and as RGBA.
async function jxlPixels() {
  if (!window.ImageDecoder) {
    return 'absent';
  }
  if (!await ImageDecoder.isTypeSupported('image/jxl')) {
    return 'unsupported';
  }
  const decoder = new ImageDecoder({data: base64Bytes(JXL_PROBE),
    type: 'image/jxl'});
  try {
    const {image} = await decoder.decode();
    try {
      return await frameHashes(image);
    } finally {
      image.close();
    }
  } finally {
    decoder.close();
  }
}

// 1.6 s of a 32-bit float stereo WAV at 48 kHz: a waveform of period 96
// (500 Hz, three triangle harmonics; another one on the right) whose
// amplitude grows by a millionth per period, so that the blocks time
// stretching compares are near ties that the sums' rounding decides.
function stretchWav() {
  const rate = 48000;
  const frames = rate * 8 / 5;
  const d = new DataView(new ArrayBuffer(44 + frames * 8));
  const text = (at, s) => {
    for (let i = 0; i < s.length; ++i) {
      d.setUint8(at + i, s.charCodeAt(i));
    }
  };
  text(0, 'RIFF');
  d.setUint32(4, 36 + frames * 8, true);
  text(8, 'WAVE');
  text(12, 'fmt ');
  d.setUint32(16, 16, true);
  d.setUint16(20, 3, true);  // IEEE float
  d.setUint16(22, 2, true);
  d.setUint32(24, rate, true);
  d.setUint32(28, rate * 8, true);
  d.setUint16(32, 8, true);
  d.setUint16(34, 32, true);
  text(36, 'data');
  d.setUint32(40, frames * 8, true);
  const tri = (p) => (p < 48 ? p : 96 - p) / 24 - 1;
  for (let i = 0; i < frames; ++i) {
    const p = i % 96;
    const gain = 0.3 * (1 + 1e-6 * Math.floor(i / 96));
    const l = gain * (tri(p) + tri((2 * p) % 96) / 2 + tri((3 * p) % 96) / 3);
    const r = gain * (tri((p + 17) % 96) - tri((5 * p) % 96) / 4);
    d.setFloat32(44 + i * 8, l, true);
    d.setFloat32(48 + i * 8, r, true);
  }
  return new Blob([d.buffer], {type: 'audio/wav'});
}

// The WAV played by an <audio> element at playbackRate 1.5 with its pitch
// kept (WSOLA time stretching) into Web Audio (createMediaElementSource),
// recorded by an AudioWorklet. A worklet's input is farbled in its low 8
// mantissa bits, so those are cleared; the stretching's choices change far
// more. Reports a hash of 0.75 s of the left channel from the first
// non-silent sample, or why there is none. Needs a user gesture (an unmuted
// element), so the harness calls it after the page's collection.
async function stretchedAudio() {
  if (!window.AudioWorkletNode) {
    return 'absent';
  }
  const ctx = new AudioContext();
  const audio = new Audio();
  const wav = URL.createObjectURL(stretchWav());
  const module = URL.createObjectURL(new Blob([
    'registerProcessor("mlk-record", class extends AudioWorkletProcessor {' +
    'process(inputs) { const c = inputs[0]; if (c && c.length) ' +
    '{ this.port.postMessage(c[0].slice()); } return true; } });'],
  {type: 'text/javascript'}));
  try {
    await ML.timeout(ctx.audioWorklet.addModule(module), 5000);
    await ML.timeout(ctx.resume(), 3000);
    const chunks = [];
    const node = new AudioWorkletNode(ctx, 'mlk-record',
        {numberOfInputs: 1, numberOfOutputs: 0, channelCount: 2,
          channelCountMode: 'explicit'});
    node.port.onmessage = (e) => chunks.push(e.data);
    await ML.timeout(new Promise((resolve, reject) => {
      audio.oncanplaythrough = resolve;
      audio.onerror = () => reject(new Error('media error'));
      audio.src = wav;
    }), 5000);
    audio.preservesPitch = true;
    audio.playbackRate = 1.5;
    ctx.createMediaElementSource(audio).connect(node);
    const ended = new Promise((resolve) => {
      audio.onended = resolve;
    });
    await ML.timeout(audio.play(), 3000);
    await Promise.race([ended, ML.sleep(5000)]);
    await ML.sleep(200);
    node.port.onmessage = null;
    let size = 0;
    for (const c of chunks) {
      size += c.length;
    }
    const all = new Float32Array(size);
    let at = 0;
    for (const c of chunks) {
      all.set(c, at);
      at += c.length;
    }
    const onset = all.findIndex((x) => Math.abs(x) > 1e-3);
    const want = 36000;
    if (onset < 0) {
      return 'silent';
    }
    if (all.length - onset < want) {
      return 'short';
    }
    const span = all.slice(onset, onset + want);
    let zeros = 0;
    for (const x of span) {
      zeros = x === 0 ? zeros + 1 : 0;
      if (zeros >= 64) {
        return 'dropout';
      }
    }
    const bits = new Uint32Array(span.buffer);
    for (let i = 0; i < bits.length; ++i) {
      bits[i] &= 0xffffff00;
    }
    return hex(new Uint8Array(bits.buffer));
  } finally {
    audio.pause();
    audio.removeAttribute('src');
    audio.load();
    URL.revokeObjectURL(wav);
    URL.revokeObjectURL(module);
    await ctx.close();
  }
}
window.mlStretchedAudio = () => stretchedAudio().then(
    (value) => ({'audio.stretched': value}),
    (e) => ({'audio.stretched': ML.errInfo(e)}));

async function cpuFeatures(v, step) {
  v['cpu.wasm_relaxed_simd'] = WebAssembly.validate(
      new Uint8Array(WASM_RELAXED_SIMD));
  v['cpu.wasm_simd'] = WebAssembly.validate(new Uint8Array(WASM_SIMD));
  await step('cpu.math', mathHash);
  await step('cpu.math_cross_arch', mathCrossArch);
  await step('cpu.css_math', cssMath);
  await step('cpu.dommatrix', domMatrixMath);
  await step('geom.hit_testing', geomHitTesting);
  await step('geom.path_length', geomPathLength);
  await step('geom.shape_outside', geomShapeOutside);
  await step('geom.contrast_color', geomContrastColor);
  await step('audio.math', audioMath);
  await step('pixels.p3_readback', p3Readback);
  await step('audio.decoded_vorbis', decodedVorbis);
  // What pages can learn about the processor: Float16 NaN bits (the
  // canonical NaN on every x86-64 CPU) and JPEG XL pixels.
  await step('cpu.float16_nan', float16Nan);
  const f16 = v['cpu.float16_nan'];
  if (f16 && typeof f16 === 'object' && 'bits' in f16) {
    v['cpu.float16_nan'] = f16.bits;
    v['cpu.float16_nan_canonical'] = f16.canonical;
  } else {
    v['cpu.float16_nan_canonical'] = f16;
  }
  await step('pixels.jxl', jxlPixels);
}

async function mediaPixels(v, step) {
  await step('pixels.webcodecs_frame', webcodecsFrame);
  await step('pixels.video_frame', videoElementFrame);
  const frame = v['pixels.video_frame'];
  v['pixels.video_frame_format'] = frame && typeof frame === 'object' &&
      'format' in frame ? frame.format : frame;
  await step('pixels.encoded_vp8_i420', () => encodedVideo('I420'));
  await step('pixels.encoded_vp8_rgba', () => encodedVideo('RGBA'));
  await step('pixels.encoded_opus_48k', () => encodedAudio(48000));
  await step('pixels.encoded_opus_44k', () => encodedAudio(44100));
}

async function collect() {
  const v = {};
  const step = async (key, fn) => {
    try {
      v[key] = await ML.timeout(Promise.resolve().then(fn), 10000);
    } catch (e) {
      v[key] = ML.errInfo(e);
    }
  };
  const n = navigator;
  const ro = Intl.DateTimeFormat().resolvedOptions();
  v['tz.resolved'] = ro.timeZone;
  v['tz.offset_jan'] = new Date(2024, 0, 15).getTimezoneOffset();
  v['tz.offset_jul'] = new Date(2024, 6, 15).getTimezoneOffset();
  v['tz.date_string_utc'] = new Date(0).toString().includes('GMT+0000');
  v['intl.locale'] = ro.locale;
  v['intl.calendar'] = ro.calendar;
  v['intl.numberingSystem'] = ro.numberingSystem;
  v['intl.number'] = (1234567.891).toLocaleString();
  v['intl.datetime'] = new Date(Date.UTC(2024, 2, 5, 13, 4, 9))
      .toLocaleString(undefined, {timeZone: 'UTC'});
  v['nav.language'] = n.language;
  v['nav.languages'] = Array.from(n.languages || []);
  v['nav.hardwareConcurrency'] = n.hardwareConcurrency;
  v['nav.deviceMemory'] = n.deviceMemory === undefined ? null :
      n.deviceMemory;
  v['nav.cpuPerformance'] = n.cpuPerformance === undefined ? null :
      n.cpuPerformance;
  v['nav.maxTouchPoints'] = n.maxTouchPoints;
  v['nav.doNotTrack'] = n.doNotTrack;
  v['nav.globalPrivacyControl'] = n.globalPrivacyControl === undefined ?
      null : n.globalPrivacyControl;
  v['nav.userAgent'] = n.userAgent;
  v['ua.reduced'] = n.userAgent;
  v['nav.platform'] = n.platform;
  v['nav.pdfViewerEnabled'] = n.pdfViewerEnabled;
  v['nav.plugins'] = n.plugins ? n.plugins.length : null;
  v['nav.webdriver'] = n.webdriver;
  v['nav.connection'] = n.connection ? {effectiveType:
      n.connection.effectiveType, rtt: n.connection.rtt, downlink:
      n.connection.downlink, saveData: n.connection.saveData} : null;
  // The same in a dedicated worker, which has a navigator.connection of
  // its own.
  await step('worker.connection', () => new Promise((resolve) => {
    const src = 'const c = navigator.connection; postMessage(c ? ' +
        '{effectiveType: c.effectiveType, rtt: c.rtt, downlink: ' +
        'c.downlink, saveData: c.saveData} : null);';
    const w = new Worker(URL.createObjectURL(new Blob([src])));
    w.onmessage = (e) => {
      resolve(e.data);
      w.terminate();
    };
    w.onerror = () => resolve('error');
  }));
  if (n.userAgentData) {
    v['uad.platform'] = n.userAgentData.platform;
    v['uad.brands'] = n.userAgentData.brands.map((b) => b.brand + '/' +
        b.version);
    await step('uad.high_entropy', () => n.userAgentData.getHighEntropyValues([
      'platformVersion', 'architecture', 'bitness', 'model', 'uaFullVersion',
      'fullVersionList', 'wow64', 'formFactors']).then((h) => ({
      platformVersion: h.platformVersion, architecture: h.architecture,
      bitness: h.bitness, model: h.model, uaFullVersion: h.uaFullVersion,
      fullVersionList: h.fullVersionList, wow64: h.wow64,
      formFactors: h.formFactors})));
  }
  v['mq.pointer_fine'] = mq('(pointer: fine)');
  v['mq.pointer_coarse'] = mq('(pointer: coarse)');
  v['mq.any_pointer_coarse'] = mq('(any-pointer: coarse)');
  v['mq.hover_hover'] = mq('(hover: hover)');
  v['mq.any_hover_none'] = mq('(any-hover: none)');
  v['mq.prefers_color_scheme_dark'] = mq('(prefers-color-scheme: dark)');
  v['mq.prefers_reduced_motion'] = mq('(prefers-reduced-motion: reduce)');
  v['mq.forced_colors'] = mq('(forced-colors: active)');
  v['mq.prefers_contrast_more'] = mq('(prefers-contrast: more)');
  // The display's colour (an sRGB, standard-range, 8-bit screen in MoltNet).
  v['mq.color_gamut_srgb'] = mq('(color-gamut: srgb)');
  v['mq.color_gamut_p3'] = mq('(color-gamut: p3)');
  v['mq.color_gamut_rec2020'] = mq('(color-gamut: rec2020)');
  v['mq.dynamic_range_high'] = mq('(dynamic-range: high)');
  v['mq.video_dynamic_range_high'] = mq('(video-dynamic-range: high)');
  v['mq.color_bits'] = colorBits();
  v['mq.monochrome'] = mq('(monochrome)');
  v['touch.ontouchstart'] = 'ontouchstart' in window;
  try {
    document.createEvent('TouchEvent');
    v['touch.createEvent'] = 'ok';
  } catch (e) {
    v['touch.createEvent'] = ML.errName(e);
  }
  webgl(v, 'webgl');
  webgl(v, 'webgl2');
  await step('webgpu.adapter', async () => {
    if (!n.gpu) {
      return 'absent';
    }
    const a = await n.gpu.requestAdapter();
    return a ? 'present' : null;
  });
  await step('fonts.visible', async () => {
    await document.fonts.ready;
    return FONT_PROBES.filter((f) => document.fonts.check('16px "' + f +
        '"'));
  });
  v['fonts.widths'] = {};
  for (const fam of ['serif', 'sans-serif', 'monospace', 'cursive',
    'fantasy', '"Liberation Serif"', '"Liberation Sans"',
    '"DejaVu Sans Mono"', '"DejaVu Sans"']) {
    v['fonts.widths'][fam] = textWidth(fam);
  }
  v['fonts.system_ui_width'] = textWidth('system-ui');
  await bundledFonts(v, step);
  textMetrics(v);
  forms(v);
  await step('doc.characterSet_undeclared', () => frameLoad(
      'charset.html'));
  await step('headers', () => fetch('/headers').then((r) => r.json()));
  const h = v['headers'] || {};
  delete v['headers'];
  v['hdr.accept-language'] = h['accept-language'] || null;
  v['hdr.dnt'] = h['dnt'] || null;
  v['hdr.sec-gpc'] = h['sec-gpc'] || null;
  const hints = {};
  for (const k of Object.keys(h)) {
    if (k.startsWith('sec-ch-') || ['dpr', 'viewport-width', 'device-memory',
      'rtt', 'downlink', 'ect', 'width', 'save-data'].includes(k)) {
      hints[k] = h[k];
    }
  }
  v['hdr.client_hints'] = hints;
  for (const name of ['geolocation', 'notifications', 'camera', 'microphone',
    'window-management', 'persistent-storage', 'local-fonts', 'midi',
    'clipboard-read']) {
    await step('perm.' + name, () => n.permissions.query({name})
        .then((s) => s.state));
  }
  await step('screenDetails.error', async () => {
    if (!window.getScreenDetails) {
      return 'absent';
    }
    try {
      await getScreenDetails();
      return 'resolved';
    } catch (e) {
      return ML.errName(e);
    }
  });
  await step('storage.persist', () => n.storage.persist());
  await step('storage.quota', () => n.storage.estimate().then((e) =>
      e.quota));
  // Reported quota minus usage: a fixed 10 GiB in MoltNet (off-the-record
  // profiles report a static quota at 156).
  await step('storage.quota_over_usage', () => n.storage.estimate().then(
      (e) => e.quota - e.usage));
  await step('audio.render_quantum_hardware', hardwareRenderQuantum);
  try {
    await ML.timeout(audioDevice(v), 10000);
  } catch (e) {
    v['audio.sample_rate'] = ML.errInfo(e);
  }
  await mediaCapabilities(v, step);
  await recorderAndLocalFonts(v, step);
  await mediaPixels(v, step);
  await cpuFeatures(v, step);
  v['autoplay.mediaelement'] = n.getAutoplayPolicy ?
      n.getAutoplayPolicy('mediaelement') : 'absent';
  await step('sharedStorage.set', async () => {
    if (!window.sharedStorage) {
      return 'absent';
    }
    try {
      await sharedStorage.set('mlk', '1');
      return 'resolved';
    } catch (e) {
      return 'rejected';
    }
  });
  await step('cookies.third_party', () => frameLoad(ML.cfg.baseBHttps +
      '/t/' + ML.cfg.nonce + '/tp_frame.html', true).then((d) =>
    d && typeof d === 'object' ? d.cookie : d));
  v['js.enabled_on_blocked_site'] = true;
  try {
    new RTCPeerConnection();
    v['webrtc.error'] = 'available';
  } catch (e) {
    v['webrtc.error'] = ML.errName(e);
  }
  try {
    v['webrtc.codecs'] = RTCRtpSender.getCapabilities('video').codecs.length;
  } catch (e) {
    v['webrtc.codecs'] = ML.errName(e);
  }
  for (const name of ['LanguageModel', 'Summarizer', 'Writer', 'Rewriter',
    'Translator']) {
    await step('ai.' + name, async () => {
      const api = self[name];
      if (!api || !api.availability) {
        return 'absent';
      }
      return api.availability(name === 'Translator' ?
          {sourceLanguage: 'en', targetLanguage: 'de'} : {});
    });
  }
  await step('media.devices', async () => {
    const counts = {};
    for (const d of await n.mediaDevices.enumerateDevices()) {
      counts[d.kind] = (counts[d.kind] || 0) + 1;
    }
    return counts;
  });
  // Output devices listed before any permission (MoltNet: always the one).
  await step('media.audiooutput', async () =>
    (await n.mediaDevices.enumerateDevices()).filter(
        (d) => d.kind === 'audiooutput').length);
  // The device list in order, and whether it names nothing (MoltNet: one
  // of each kind, without ID, label or group, before any permission).
  await step('media.device_order', async () =>
    (await n.mediaDevices.enumerateDevices()).map((d) => d.kind));
  await step('media.device_blank', async () =>
    (await n.mediaDevices.enumerateDevices()).every((d) =>
      d.deviceId === '' && d.label === '' && d.groupId === ''));
  // A camera and microphone, and constraints no camera meets: asked about
  // like any request in MoltNet, whatever the machine has.
  await step('media.gum', () =>
    userMediaBeforePermission({audio: true, video: true}));
  await step('media.gum_overconstrained', () =>
    userMediaBeforePermission({video: {width: {exact: 12345}}}));
  // setSinkId('') on an element without a player: the default output
  // (MoltNet: on every machine).
  await step('media.sink_default', () => new Audio().setSinkId('').then(
      () => 'resolved', (e) => ML.errName(e)));
  v['perf.jsHeapSizeLimit'] = performance.memory ?
      performance.memory.jsHeapSizeLimit : null;
  // performance.memory rounded to one of 100 sizes (multiples of 100,000)
  // and refreshed once every 20 minutes (MoltNet), not every 50 ms.
  await step('perf.heap_rounded', () => performance.memory ?
      [performance.memory.usedJSHeapSize,
        performance.memory.totalJSHeapSize].every((b) => b % 100000 === 0) :
      'absent');
  await step('perf.heap_cached', async () => {
    if (!performance.memory) {
      return 'absent';
    }
    const before = performance.memory.usedJSHeapSize;
    const keep = new Array(1 << 20).fill(1.5);
    await ML.sleep(120);
    const after = performance.memory.usedJSHeapSize;
    return keep.length > 0 && after === before;
  });
  await step('speech.voices', () => new Promise((resolve) => {
    const now = speechSynthesis.getVoices();
    if (now.length) {
      resolve(now.length);
      return;
    }
    speechSynthesis.onvoiceschanged = () =>
      resolve(speechSynthesis.getVoices().length);
    setTimeout(() => resolve(speechSynthesis.getVoices().length), 1500);
  }));
  await step('battery', async () => {
    if (!n.getBattery) {
      return 'absent';
    }
    const b = await n.getBattery();
    return {level: b.level, charging: b.charging};
  });
  await step('keyboard.layout', async () => {
    if (!n.keyboard || !n.keyboard.getLayoutMap) {
      return 'absent';
    }
    const m = await n.keyboard.getLayoutMap();
    return {size: m.size, KeyZ: m.get('KeyZ'), KeyY: m.get('KeyY'),
      Semicolon: m.get('Semicolon'), BracketLeft: m.get('BracketLeft')};
  });
  await step('canvas.hash', canvasHash);
  await step('audio.hash', audioHash);
  Object.assign(v, window.mlGeometry());
  v['nav.connection_changes'] = navigator.connection ? connectionChanges :
      null;
  return v;
}

collect().then((v) => {
  Object.assign(ML.results, v);
  ML.finish();
}, (e) => {
  ML.set('collect_error', ML.errInfo(e));
  ML.finish();
});
