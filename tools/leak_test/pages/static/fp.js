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
  v['perf.jsHeapSizeLimit'] = performance.memory ?
      performance.memory.jsHeapSizeLimit : null;
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
