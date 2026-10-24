# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""What a MoltNet page must read: the values docs/FINGERPRINTING.md and
docs/MOLTNET_COMPLIANCE.md promise are the same for every MoltNet user.

Each rule is (key, op, expected, doc). Keys are the flat keys pages/static/
fp.js reports. Operators (uniformity.py):

  eq, in, lt          the value equals / is one of / is below `expected`
  eq_key              equals the value of another key
  step                a positive multiple of expected[0], at most expected[1]
  baseline_max        a WebGL limit: equal to the baseline, or below it on a
                      GPU that lacks it (min(real, baseline)); never above
  baseline_min        the other direction (max(real, baseline))
  range_clamp         a [low, high] range clamped to expected
  precision           a shader precision format clamped to expected
  subset              a list whose items all appear in `expected`
  blank_ua            userAgentData high-entropy values are blank
  only_hints          only the allowed client hints reach the server
  orientation         landscape-primary iff screen.width > screen.height
  regex               the string matches
  per_os_eq           equals expected[<the run's OS>] (INFO on another OS)

DIFF_KEYS lists what two runs (two machines, two OS locales) must report
identically even where the docs promise no fixed value; INFO_KEYS are
recorded and shown when they differ, without a verdict, because nothing
promises them (they are listed so the owner sees what still varies).
"""

FP = 'FINGERPRINTING.md'
CO = 'MOLTNET_COMPLIANCE.md'

LETTERBOX_STEP = (200, 100)
LETTERBOX_MAX = (1400, 900)

# Linux font allow-list (font_allowlists.cc, kLinuxFonts), with the aliases
# protected contexts resolve to the listed family.
LINUX_FONTS = (
    'DejaVu Sans', 'DejaVu Sans Condensed', 'DejaVu Sans Light',
    'DejaVu Sans Mono', 'DejaVu Serif', 'DejaVu Serif Condensed',
    'Liberation Mono', 'Liberation Sans', 'Liberation Serif',
    'Noto Color Emoji', 'Noto Sans', 'Noto Sans Mono', 'Noto Serif',
    'Arial', 'Helvetica', 'Times New Roman', 'Times', 'Courier New',
    'Courier',
)

# WebGL extension allow-list (webgl_uniform.cc, kAllowedExtensions).
WEBGL_EXTENSIONS = (
    'ANGLE_instanced_arrays', 'EXT_blend_minmax', 'EXT_color_buffer_float',
    'EXT_color_buffer_half_float', 'EXT_frag_depth', 'EXT_sRGB',
    'EXT_shader_texture_lod', 'EXT_texture_filter_anisotropic',
    'KHR_parallel_shader_compile', 'OES_element_index_uint',
    'OES_standard_derivatives', 'OES_texture_float',
    'OES_texture_float_linear', 'OES_texture_half_float',
    'OES_texture_half_float_linear', 'OES_vertex_array_object',
    'WEBGL_color_buffer_float', 'WEBGL_compressed_texture_s3tc',
    'WEBGL_compressed_texture_s3tc_srgb', 'WEBGL_depth_texture',
    'WEBGL_draw_buffers', 'WEBGL_lose_context', 'WEBGL_multi_draw',
)

# WebGL limits (webgl_uniform.cc kIntLimits / kFloatLimits): name ->
# (baseline, 'max' = reported min(real, baseline) | 'min' = max(...)).
WEBGL_LIMITS = {
    'MAX_TEXTURE_SIZE': (8192, 'max'),
    'MAX_CUBE_MAP_TEXTURE_SIZE': (8192, 'max'),
    'MAX_RENDERBUFFER_SIZE': (8192, 'max'),
    'MAX_VERTEX_ATTRIBS': (16, 'max'),
    'MAX_VERTEX_UNIFORM_VECTORS': (1024, 'max'),
    'MAX_FRAGMENT_UNIFORM_VECTORS': (1024, 'max'),
    'MAX_VARYING_VECTORS': (30, 'max'),
    'MAX_TEXTURE_IMAGE_UNITS': (16, 'max'),
    'MAX_VERTEX_TEXTURE_IMAGE_UNITS': (16, 'max'),
    'MAX_COMBINED_TEXTURE_IMAGE_UNITS': (32, 'max'),
    'SUBPIXEL_BITS': (4, 'max'),
    'MAX_TEXTURE_MAX_ANISOTROPY_EXT': (16, 'max'),
}
WEBGL2_LIMITS = dict(WEBGL_LIMITS)
WEBGL2_LIMITS.update({
    'MAX_VERTEX_UNIFORM_COMPONENTS': (4096, 'max'),
    'MAX_FRAGMENT_UNIFORM_COMPONENTS': (4096, 'max'),
    'MAX_VARYING_COMPONENTS': (120, 'max'),
    'MAX_VERTEX_OUTPUT_COMPONENTS': (120, 'max'),
    'MAX_FRAGMENT_INPUT_COMPONENTS': (120, 'max'),
    'MAX_DRAW_BUFFERS': (8, 'max'),
    'MAX_COLOR_ATTACHMENTS': (8, 'max'),
    'MAX_3D_TEXTURE_SIZE': (2048, 'max'),
    'MAX_ARRAY_TEXTURE_LAYERS': (2048, 'max'),
    'MAX_SAMPLES': (4, 'max'),
    'MAX_VERTEX_UNIFORM_BLOCKS': (12, 'max'),
    'MAX_FRAGMENT_UNIFORM_BLOCKS': (12, 'max'),
    'MAX_COMBINED_UNIFORM_BLOCKS': (24, 'max'),
    'MAX_UNIFORM_BUFFER_BINDINGS': (24, 'max'),
    'MAX_UNIFORM_BLOCK_SIZE': (16384, 'max'),
    'MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS': (53248, 'max'),
    'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS': (53248, 'max'),
    'UNIFORM_BUFFER_OFFSET_ALIGNMENT': (256, 'min'),
    'MIN_PROGRAM_TEXEL_OFFSET': (-8, 'min'),
    'MAX_PROGRAM_TEXEL_OFFSET': (7, 'max'),
    'MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS': (64, 'max'),
    'MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS': (4, 'max'),
    'MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS': (4, 'max'),
    'MAX_ELEMENT_INDEX': (4294967295, 'max'),
    'MAX_ELEMENTS_VERTICES': (2048, 'max'),
    'MAX_ELEMENTS_INDICES': (2048, 'max'),
    'MAX_TEXTURE_LOD_BIAS': (2.0, 'max'),
    'MAX_SERVER_WAIT_TIMEOUT': (0, 'max'),
})
WEBGL_RANGES = {
    'ALIASED_POINT_SIZE_RANGE': (1.0, 255.0),
    'ALIASED_LINE_WIDTH_RANGE': (1.0, 1.0),
}
WEBGL_VIEWPORT = 8192
PRECISION = {
    'LOW_FLOAT': (15, 15, 10), 'MEDIUM_FLOAT': (15, 15, 10),
    'HIGH_FLOAT': (127, 127, 23), 'LOW_INT': (15, 14, 0),
    'MEDIUM_INT': (15, 14, 0), 'HIGH_INT': (31, 30, 0),
}

# Client hints MoltNet may send (moltnet_device_values.cc).
ALLOWED_HINTS = ('sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
                 'save-data', 'sec-ch-prefers-color-scheme',
                 'sec-ch-prefers-reduced-motion',
                 'sec-ch-prefers-reduced-transparency')

S_SURFACE = FP + ' "Surface covered"'
S_LETTER = FP + ' "Letterboxing (MoltNet)"'
S_DEVICE = FP + ' "MoltNet: no touchscreen, client hints, memory and CPU"'
S_WEBGL = FP + ' "WebGL and WebGPU in MoltNet windows"'
S_TOUCH = FP + ' "MoltNet: touches, screens, orientation and form controls"'
S_LANG = FP + ' "MoltNet: the browser\'s UI language"'
S_SETTINGS = FP + ' "MoltNet: settings from the regular profile"'
S_AI = FP + ' "Built-in AI APIs"'
S_FONTS = FP + ' "Fonts"'
S_TEXT = FP + ' "Text metrics (MoltNet)"'
S_NETQ = FP + ' "MoltNet: connection and network quality"'
S_156 = FP + ' "Re-checked at Chromium 156"'
S_MEDIA = FP + ' "MoltNet: audio, DRM and media capabilities"'
S_REC = FP + ' "MoltNet: MediaRecorder"'
S_STORAGE = FP + ' "MoltNet: storage limits"'
S_MCLOCK = FP + ' "MoltNet: media playback clock"'
S_MDEV = FP + ' "MoltNet: cameras, microphones and outputs"'
S_MEM = FP + ' "MoltNet: performance.memory"'
S_PIXELS = FP + ' "MoltNet: pixels and encoded bytes"'

# MoltNet's scrollbar style: each platform's default (moltnet_values.h,
# UsesOverlayScrollbars).
OVERLAY_SCROLLBARS = {'macOS': True, 'Windows': False, 'Linux': False}
# The reported storage quota over the usage in an off-the-record profile
# (storage/browser/quota: 10 GiB at 156).
STATIC_QUOTA = 10 * 1024 ** 3

# Web Audio's output device in MoltNet (moltnet_media_values.h): 48 kHz,
# a 10 ms buffer (480 frames; 441 at a page's 44.1 kHz), 20 ms rounded up
# to 1024 frames for "playback", an exact hint of 0 as 128 frames.
AUDIO_BASE_LATENCY = {'interactive': 480 / 48000, 'balanced': 480 / 48000,
                      'playback': 1024 / 48000, 'exact_0': 128 / 48000,
                      'exact_20ms': 1024 / 48000, 'rate_44100': 441 / 44100}
# When MoltNet renders (moltnet_audio_clock.h): every 10 ms, 480 frames at
# 48 kHz, which AudioDestination renders as 3 or 4 quanta of 128.
AUDIO_TICK_MS = 10
AUDIO_TICK_BURSTS = [384, 512]
# The devices every MoltNet machine lists before a permission, in the order
# enumerateDevices() lists kinds (moltnet_media_devices.h).
MEDIA_DEVICES = {'audioinput': 1, 'videoinput': 1, 'audiooutput': 1}
MEDIA_DEVICE_ORDER = ['audioinput', 'videoinput', 'audiooutput']
# getUserMedia() before a permission: asked about (still pending when the
# probe gives up) or refused, never failed for want of a device.
USER_MEDIA_ASKED = ['pending', 'NotAllowedError']
# performance.memory.jsHeapSizeLimit in MoltNet: the 8 GB machine's limit
# (4 GiB + 96 MiB) rounded to the last of the 100 sizes
# (moltnet_memory_values.h).
JS_HEAP_SIZE_LIMIT = 3760000000

# Media Capabilities in MoltNet: a video the build decodes itself is smooth
# and not power efficient; key systems and WebRTC are never supported.
MC_SOFTWARE_VIDEO = {'supported': True, 'smooth': True,
                     'powerEfficient': False}
MC_UNSUPPORTED = {'supported': False, 'smooth': False,
                  'powerEfficient': False}

# MediaRecorder in MoltNet records with the build's software encoders
# (moltnet_encoders.h): the type it reports for a canvas stream and each
# container a page can ask for.
RECORDER_MIME = {'default': 'video/webm;codecs=vp8',
                 'video/webm': 'video/webm;codecs=vp8',
                 'video/x-matroska': 'video/x-matroska;codecs=vp8',
                 'video/mp4': 'video/mp4;codecs=vp9'}
# The quota the browser enforces per site in MoltNet (molt_net_storage_
# values.h: each site's budget, all its storage keys together), read through
# DevTools for a site that stores nothing: writing until QuotaExceededError
# would take too long for the kit, so the configured limit is checked
# instead.
MOLTNET_SITE_QUOTA = 60 * 1024 ** 2

# What MoltBrowser's torrc sets for the capacity every MoltNet site shares
# (chrome/browser/molt_ai/tor/torrc_builder.h).
TOR_CAPACITY = {'ConnLimit': '1000', 'MaxClientCircuitsPending': '64'}

# navigator.connection in MoltNet (moltnet_network_values.h).
CONNECTION = {'effectiveType': '4g', 'rtt': 200, 'downlink': 1.6,
              'saveData': False}

# Rules that hold for every fingerprint record (any window size, zoom or
# scale factor).
GEOMETRY_RULES = (
    ('win.innerWidth', 'step', (200, 1400), S_LETTER),
    ('win.innerHeight', 'step', (100, 900), S_LETTER),
    ('screen.width', 'eq_key', 'win.innerWidth', S_LETTER),
    ('screen.height', 'eq_key', 'win.innerHeight', S_LETTER),
    ('screen.availWidth', 'eq_key', 'screen.width', S_SURFACE),
    ('screen.availHeight', 'eq_key', 'screen.height', S_SURFACE),
    ('screen.availLeft', 'eq', 0, S_SURFACE),
    ('screen.availTop', 'eq', 0, S_SURFACE),
    ('win.outerWidth', 'eq_key', 'win.innerWidth', S_SURFACE),
    ('win.outerHeight', 'eq_key', 'win.innerHeight', S_SURFACE),
    ('win.screenX', 'eq', 0, S_SURFACE),
    ('win.screenY', 'eq', 0, S_SURFACE),
    ('win.devicePixelRatio', 'eq', 1, FP + ' "Device pixel ratio"'),
    ('mq.resolution_1dppx', 'eq', True, FP + ' "Device pixel ratio"'),
    ('mq.device_size_matches', 'eq', True, S_LETTER),
    ('mq.viewport_exact', 'eq', True, S_LETTER),
    # A classic scrollbar is 16 CSS pixels (0: overlay scrollbars). At 33%
    # and 67% zoom on a 1x screen, not in the kit's default zoom list, 16
    # CSS pixels aren't whole device pixels and this reads 15 or 17
    # (FINGERPRINTING.md "Zoom").
    ('scrollbar.box', 'in', [0, 16], S_LETTER),
    ('scrollbar.overlay', 'per_os_eq', OVERLAY_SCROLLBARS, S_156),
    ('screen.colorDepth', 'eq', 24, S_SURFACE),
    ('screen.pixelDepth', 'eq', 24, S_SURFACE),
    ('screen.isExtended', 'eq', False, S_SURFACE),
    ('screen.orientation.type', 'orientation', None, S_TOUCH),
    ('screen.orientation.angle', 'eq', 0, S_TOUCH),
    ('mouse.screen_equals_client', 'eq', True, S_SURFACE),
)

# Rules for the full fingerprint (collected once per launch).
FULL_RULES = (
    ('tz.resolved', 'in', ['UTC', 'Etc/UTC'], S_SURFACE),
    ('tz.offset_jan', 'eq', 0, S_SURFACE),
    ('tz.offset_jul', 'eq', 0, S_SURFACE),
    ('tz.date_string_utc', 'eq', True, S_SURFACE),
    ('intl.locale', 'eq', 'en-US', S_SURFACE),
    ('intl.number', 'eq', '1,234,567.891', S_SURFACE),
    ('nav.language', 'eq', 'en-US', S_SURFACE),
    ('nav.languages', 'eq', ['en-US'], S_SURFACE),
    ('hdr.accept-language', 'eq', 'en-US', S_SURFACE),
    ('nav.hardwareConcurrency', 'eq', 4, S_SURFACE),
    ('nav.deviceMemory', 'eq', 8, S_DEVICE),
    ('nav.cpuPerformance', 'in', [0, None], S_DEVICE),
    ('nav.connection', 'eq', CONNECTION, S_NETQ),
    ('worker.connection', 'eq', CONNECTION, S_NETQ),
    ('nav.connection_changes', 'eq', 0, S_NETQ),
    ('nav.maxTouchPoints', 'eq', 0, S_DEVICE),
    ('mq.pointer_fine', 'eq', True, S_DEVICE),
    ('mq.pointer_coarse', 'eq', False, S_DEVICE),
    ('mq.any_pointer_coarse', 'eq', False, S_DEVICE),
    ('mq.hover_hover', 'eq', True, S_DEVICE),
    ('mq.any_hover_none', 'eq', False, S_DEVICE),
    ('touch.ontouchstart', 'eq', False, S_DEVICE),
    ('touch.createEvent', 'eq', 'NotSupportedError', S_DEVICE),
    ('uad.high_entropy', 'blank_ua', None, S_DEVICE),
    ('hdr.client_hints', 'only_hints', ALLOWED_HINTS, S_DEVICE),
    ('webgl.debug_renderer_info', 'eq', False, S_WEBGL),
    ('webgl2.debug_renderer_info', 'eq', False, S_WEBGL),
    ('webgl.extensions', 'subset', WEBGL_EXTENSIONS, S_WEBGL),
    ('webgl2.extensions', 'subset', WEBGL_EXTENSIONS, S_WEBGL),
    ('webgpu.adapter', 'in', [None, 'absent'], S_WEBGL),
    ('fonts.visible', 'subset', 'per_os', S_FONTS),
    # measureText() at 16 and 48px, left-aligned: every value a whole pixel.
    ('text.metrics_whole', 'eq', True, S_TEXT),
    ('form.validationMessage', 'eq', 'Please fill out this field.', S_LANG),
    ('form.numberMessage', 'eq',
     'Value must be greater than or equal to 10.', S_LANG),
    ('doc.characterSet_undeclared', 'eq', 'windows-1252', S_LANG),
    ('css.default_font_size', 'eq', '16px', S_SETTINGS),
    ('nav.doNotTrack', 'eq', None, S_SETTINGS),
    ('hdr.dnt', 'eq', None, S_SETTINGS),
    ('nav.globalPrivacyControl', 'eq', True, CO + ' §6'),
    ('hdr.sec-gpc', 'eq', '1', CO + ' §6'),
    ('perm.window-management', 'eq', 'denied', S_TOUCH),
    ('screenDetails.error', 'eq', 'NotAllowedError', S_TOUCH),
    ('perm.geolocation', 'eq', 'prompt',
     FP + ' "Known gaps": a documented giveaway'),
    ('storage.persist', 'eq', False, CO + ' §1.11'),
    ('autoplay.mediaelement', 'eq', 'allowed-muted', CO + ' §1.11'),
    ('sharedStorage.set', 'in', ['rejected', 'absent'], CO + ' §1.11'),
    ('cookies.third_party', 'eq', False, CO + ' §1.11'),
    ('js.enabled_on_blocked_site', 'eq', True, CO + ' §4 step 15'),
    ('webrtc.error', 'eq', 'NotSupportedError', CO + ' §1.12'),
    ('ai.LanguageModel', 'in', ['unavailable', 'absent'], S_AI),
    ('ai.Summarizer', 'in', ['unavailable', 'absent'], S_AI),
    ('ai.Writer', 'in', ['unavailable', 'absent'], S_AI),
    ('ai.Rewriter', 'in', ['unavailable', 'absent'], S_AI),
    ('ai.Translator', 'in', ['unavailable', 'absent'], S_AI),
    ('ua.reduced', 'regex', r'Chrome/\d+\.0\.0\.0 ', S_DEVICE),
    # The display's colour: sRGB, standard dynamic range, 8 bits.
    ('mq.color_gamut_srgb', 'eq', True, S_156),
    ('mq.color_gamut_p3', 'eq', False, S_156),
    ('mq.color_gamut_rec2020', 'eq', False, S_156),
    ('mq.dynamic_range_high', 'eq', False, S_156),
    ('mq.video_dynamic_range_high', 'eq', False, S_156),
    ('mq.color_bits', 'eq', 8, S_156),
    ('mq.monochrome', 'eq', False, S_156),
    ('storage.quota_over_usage', 'eq', STATIC_QUOTA, S_156),
    ('audio.render_quantum_hardware', 'in', [128, 'absent'], S_156),
    # Web Audio's output device, EME, Media Capabilities, WebCodecs.
    ('audio.sample_rate', 'eq', 48000, S_MEDIA),
    ('audio.base_latency', 'eq', AUDIO_BASE_LATENCY, S_MEDIA),
    ('audio.output_latency', 'eq', 0.024, S_MEDIA),
    ('audio.max_channel_count', 'eq', 2, S_MEDIA),
    ('audio.channel_count', 'eq', 2, S_MEDIA),
    ('audio.channel_count_3', 'eq', 'IndexSizeError', S_MEDIA),
    ('audio.script_processor_buffer', 'eq', 2048, S_MEDIA),
    ('audio.output_timestamp_ok', 'eq', True, S_MEDIA),
    ('audio.playback_latency_max', 'in', [0, 0.024, 'absent'], S_MEDIA),
    ('audio.playback_underruns', 'in', [0, 'absent'], S_MEDIA),
    # Rendering follows the uniform device's clock, not the real device's:
    # a burst of 480 frames (3 or 4 quanta of 128) every 10 ms.
    ('audio.cadence_interval_ms', 'eq', AUDIO_TICK_MS, S_MEDIA),
    ('audio.cadence_burst_frames', 'eq', AUDIO_TICK_BURSTS, S_MEDIA),
    # An output device is listed before any permission, device or not.
    ('media.audiooutput', 'eq', 1, S_MEDIA),
    ('eme.clearkey', 'eq', 'NotSupportedError', S_MEDIA),
    ('eme.widevine', 'eq', 'NotSupportedError', S_MEDIA),
    ('eme.playready', 'eq', 'NotSupportedError', S_MEDIA),
    ('mc.vp8_360p', 'eq', MC_SOFTWARE_VIDEO, S_MEDIA),
    ('mc.vp9_1080p', 'eq', MC_SOFTWARE_VIDEO, S_MEDIA),
    ('mc.av1_2160p60', 'eq', MC_SOFTWARE_VIDEO, S_MEDIA),
    ('mc.encrypted', 'eq', MC_UNSUPPORTED, S_MEDIA),
    ('mc.webrtc_decoding', 'eq', MC_UNSUPPORTED, S_MEDIA),
    ('mc.webrtc_encoding', 'eq', MC_UNSUPPORTED, S_MEDIA),
    ('webcodecs.decoder_hardware', 'in', [False, 'absent'], S_MEDIA),
    ('webcodecs.encoder_hardware', 'in', [False, 'absent'], S_MEDIA),
    # MediaRecorder, src: local() and the storage limits.
    ('recorder.mime_type', 'eq', RECORDER_MIME, S_REC),
    ('fonts.local_hidden', 'eq', [], S_FONTS),
    ('storage.enforced_quota', 'eq', MOLTNET_SITE_QUOTA, S_STORAGE),
    # Media playback's clock, the device list, getUserMedia() and memory.
    ('media.clock_running', 'eq', True, S_MCLOCK),
    ('media.sink_default', 'eq', 'resolved', S_MCLOCK),
    ('media.devices', 'eq', MEDIA_DEVICES, S_MDEV),
    ('media.device_order', 'eq', MEDIA_DEVICE_ORDER, S_MDEV),
    ('media.device_blank', 'eq', True, S_MDEV),
    ('media.gum', 'in', USER_MEDIA_ASKED, S_MDEV),
    ('media.gum_overconstrained', 'in', USER_MEDIA_ASKED, S_MDEV),
    ('perf.jsHeapSizeLimit', 'eq', JS_HEAP_SIZE_LIMIT, S_MEM),
    ('perf.heap_rounded', 'eq', True, S_MEM),
    ('perf.heap_cached', 'eq', True, S_MEM),
    # <video> is decoded in software and its frames stay in CPU memory.
    ('pixels.video_frame_format', 'eq', 'I420', S_PIXELS),
)

# WebGL limit / precision rules are generated (they depend on whether the
# WebGL context exists).
for _name, (_base, _dir) in sorted(WEBGL_LIMITS.items()):
    FULL_RULES += (('webgl.p.' + _name, 'baseline_' + _dir, _base, S_WEBGL),)
for _name, (_base, _dir) in sorted(WEBGL2_LIMITS.items()):
    FULL_RULES += (('webgl2.p.' + _name, 'baseline_' + _dir, _base, S_WEBGL),)
for _ctx in ('webgl', 'webgl2'):
    for _name, _range in sorted(WEBGL_RANGES.items()):
        FULL_RULES += ((_ctx + '.p.' + _name, 'range_clamp', _range, S_WEBGL),)
    FULL_RULES += ((_ctx + '.p.MAX_VIEWPORT_DIMS', 'range_clamp',
                    (None, WEBGL_VIEWPORT), S_WEBGL),)
    for _shader in ('VERTEX_SHADER', 'FRAGMENT_SHADER'):
        for _prec, _fmt in sorted(PRECISION.items()):
            FULL_RULES += (('%s.precision.%s.%s' % (_ctx, _shader, _prec),
                            'precision', _fmt, S_WEBGL),)
FULL_RULES += (('webgl2.samples_rgba8', 'all_le', 4, S_WEBGL),)

# Keys two MoltNet runs on the same OS must report identically, beyond the
# rules above (the rules' keys are compared too).
DIFF_KEYS = (
    'nav.userAgent', 'nav.platform', 'uad.platform', 'uad.brands',
    'fonts.visible', 'fonts.widths', 'form.widths', 'css.default_font_size',
    'intl.datetime', 'intl.calendar', 'intl.numberingSystem',
    'mq.prefers_color_scheme_dark', 'mq.prefers_reduced_motion',
    'mq.forced_colors', 'mq.prefers_contrast_more', 'webgl.extensions',
    'webgl2.extensions',
    'webgl.vendor', 'webgl.renderer', 'nav.pdfViewerEnabled',
    'nav.plugins', 'doc.characterSet_undeclared',
    # The same fonts measure the same on every machine of an OS (on Linux,
    # of a distribution release: font package versions differ).
    'text.metrics', 'text.emoji', 'text.dom_width',
    # Codec support follows the build (the decoders and encoders compiled
    # in), never the machine (FINGERPRINTING.md "MoltNet: audio, DRM and
    # media capabilities").
    'mc.opus', 'mc.h264', 'mc.hevc', 'media.can_play',
    'webcodecs.decoder_software', 'webcodecs.encoder_vp8',
    'webcodecs.encoder_h264', 'audio.sink_id',
    # So does what MediaRecorder can record, and src: local() resolves
    # through the visible families (FINGERPRINTING.md "Fonts").
    'recorder.supported', 'fonts.local_listed',
    # Decoded frames read back as YUV and RGBA, and the encoders' output for
    # input they scale or resample, follow the build, never the CPU, GPU or
    # video decoder (FINGERPRINTING.md "MoltNet: pixels and encoded bytes").
    'pixels.webcodecs_frame', 'pixels.video_frame',
    'pixels.encoded_vp8_i420', 'pixels.encoded_vp8_rgba',
    'pixels.encoded_opus_48k', 'pixels.encoded_opus_44k',
)

# Recorded and diffed, but nothing promises them uniform. A difference
# between two runs is reported as a finding to look at, not as a failure.
INFO_KEYS = {
    'storage.quota': 'navigator.storage.estimate() quota',
    'media.clock_start_ms': 'how long after play() an <audio> element\'s '
                            'currentTime starts moving (rounded to 10 ms)',
    'speech.voices': 'speechSynthesis.getVoices() count',
    'battery': 'navigator.getBattery() level and charging',
    'keyboard.layout': 'navigator.keyboard.getLayoutMap() sample keys',
    'webrtc.codecs': 'RTCRtpSender.getCapabilities() (documented gap)',
    'fonts.system_ui_width': 'system-ui text width (documented gap on Linux)',
    'scrollbar.width': 'the page\'s own scrollbar width (0 when the page '
                       'doesn\'t scroll; scrollbar.box is the checked one)',
    'nav.webdriver': 'navigator.webdriver under the kit\'s DevTools pipe',
    'mq.prefers_color_scheme_dark': 'dark while the dark toolbar is on '
                                    '(documented)',
}


# Farbled per site and per session: two runs always differ, so these are
# never diffed (the identity checks compare them within one run instead).
FARBLED_KEYS = ('canvas.hash', 'canvas.hash_b', 'audio.hash')


def font_allowlist(os_name):
    """The allow-list a protected context may see on `os_name`, or None
    when the kit doesn't carry that OS's list (then only the diff checks)."""
    if os_name == 'Linux':
        return LINUX_FONTS
    return None
