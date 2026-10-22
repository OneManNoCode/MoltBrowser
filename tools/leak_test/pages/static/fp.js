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
  v['mq.color_gamut_p3'] = mq('(color-gamut: p3)');
  v['mq.dynamic_range_high'] = mq('(dynamic-range: high)');
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
  return v;
}

collect().then((v) => {
  Object.assign(ML.results, v);
  ML.finish();
}, (e) => {
  ML.set('collect_error', ML.errInfo(e));
  ML.finish();
});
