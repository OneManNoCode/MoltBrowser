// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Shared by the end-to-end kit's fixture sites (docs/VOICE_E2E.md): a small
// element builder, and a player built like YouTube's (the parts the
// browser's YouTube skill looks for: a <video class="html5-main-video">
// inside #movie_player, the play, mute, captions, settings and full screen
// buttons, a volume slider and a seek slider). Pages keep their state in
// the DOM and in window.kit.state for the kit to read with
// Runtime.evaluate; nothing here acts on its own.
'use strict';

(function () {
  const kit = {state: {}};

  kit.el = function (tag, attrs, ...children) {
    const node = document.createElement(tag);
    for (const [name, value] of Object.entries(attrs || {})) {
      if (name.startsWith('on') && typeof value === 'function') {
        node.addEventListener(name.slice(2), value);
      } else if (value !== null && value !== undefined && value !== false) {
        node.setAttribute(name, value === true ? '' : String(value));
      }
    }
    for (const child of children) {
      if (child !== null && child !== undefined) {
        node.append(child);
      }
    }
    return node;
  };

  kit.param = function (name) {
    return new URLSearchParams(location.search).get(name);
  };

  kit.slug = function (s) {
    return s.toLowerCase().replace(/[^a-z0-9]/g, '');
  };

  // A player like YouTube's in |container|. opts: src, title, captions
  // (a .vtt URL), next (the next video's URL), ad (seconds of an ad first,
  // with a skip button after 3 s).
  kit.player = function (container, opts) {
    const el = kit.el;
    const video = el('video', {
      'class': 'video-stream html5-main-video', src: opts.src,
      preload: 'auto', 'aria-label': 'Video player', width: 640,
      height: 360});
    if (opts.captions) {
      video.append(el('track', {kind: 'captions', srclang: 'en',
                                label: 'English', src: opts.captions}));
    }
    const play = el('button', {'class': 'ytp-play-button ytp-button',
                               'aria-label': 'Play (k)', title: 'Play'});
    const mute = el('button', {'class': 'ytp-mute-button ytp-button',
                               'aria-label': 'Mute (m)', title: 'Mute'});
    const volume = el('input', {type: 'range', min: 0, max: 100, step: 5,
                                value: 70, 'class': 'ytp-volume-slider',
                                'aria-label': 'Volume'});
    const seek = el('input', {type: 'range', min: 0, max: 180, step: 5,
                              value: 0, 'class': 'ytp-progress-bar',
                              'aria-label': 'Seek slider'});
    const cc = el('button', {'class': 'ytp-subtitles-button ytp-button',
                             'aria-label': 'Subtitles/closed captions (c)',
                             'aria-pressed': 'false', title: 'Subtitles'});
    const settings = el('button', {'class': 'ytp-settings-button ytp-button',
                                   'aria-label': 'Settings',
                                   'aria-haspopup': 'menu',
                                   'aria-expanded': 'false'});
    const menu = el('div', {'class': 'ytp-settings-menu', role: 'menu',
                            'aria-label': 'Playback speed', hidden: true});
    for (const rate of [0.5, 0.75, 1, 1.25, 1.5, 2]) {
      menu.append(el('div', {role: 'menuitemradio', tabindex: -1,
                             'aria-checked': rate === 1 ? 'true' : 'false',
                             'data-rate': rate,
                             onclick: () => { video.playbackRate = rate; }},
                     rate === 1 ? 'Normal' : String(rate)));
    }
    const next = opts.next ?
        el('a', {'class': 'ytp-next-button ytp-button', href: opts.next,
                 'aria-label': 'Next (SHIFT+n)', title: 'Next'}, 'Next') :
        null;
    const full = el('button', {'class': 'ytp-fullscreen-button ytp-button',
                               'aria-label': 'Full screen (f)',
                               title: 'Full screen'});
    const bar = el('div', {'class': 'ytp-chrome-bottom'},
                   play, next, mute, volume, seek, cc, settings, menu, full);
    const box = el('div', {id: 'movie_player',
                           'class': 'html5-video-player',
                           'aria-label': 'Video Player', tabindex: -1},
                   el('div', {'class': 'html5-video-container'}, video), bar);
    container.append(box);

    const label = () => {
      play.setAttribute('aria-label', video.paused ? 'Play (k)' : 'Pause (k)');
      play.textContent = video.paused ? 'Play' : 'Pause';
      mute.setAttribute('aria-label', video.muted ? 'Unmute (m)' : 'Mute (m)');
      mute.textContent = video.muted ? 'Unmute' : 'Mute';
      volume.value = String(Math.round(video.volume * 100));
      for (const item of menu.children) {
        item.setAttribute('aria-checked',
                          Number(item.dataset.rate) === video.playbackRate ?
                              'true' : 'false');
      }
      const track = video.textTracks[0];
      const on = !!track && track.mode === 'showing';
      cc.setAttribute('aria-pressed', on ? 'true' : 'false');
      cc.textContent = on ? 'CC on' : 'CC';
    };
    const toggle = () => { video.paused ? video.play() : video.pause(); };
    video.addEventListener('click', toggle);
    play.addEventListener('click', toggle);
    mute.addEventListener('click', () => { video.muted = !video.muted; });
    volume.addEventListener('input', () => {
      video.volume = Number(volume.value) / 100;
      video.muted = false;
    });
    seek.addEventListener('input', () => { video.currentTime = Number(seek.value); });
    cc.addEventListener('click', () => {
      const track = video.textTracks[0];
      if (track) {
        track.mode = track.mode === 'showing' ? 'hidden' : 'showing';
      }
      label();
    });
    settings.addEventListener('click', () => {
      menu.hidden = !menu.hidden;
      settings.setAttribute('aria-expanded', menu.hidden ? 'false' : 'true');
    });
    full.addEventListener('click', () => {
      if (document.fullscreenElement) {
        document.exitFullscreen();
      } else {
        box.requestFullscreen().catch(() => {});
      }
    });
    for (const type of ['play', 'pause', 'volumechange', 'ratechange',
                        'timeupdate']) {
      video.addEventListener(type, label);
    }
    video.addEventListener('timeupdate', () => {
      seek.value = String(Math.round(video.currentTime));
    });
    if (video.textTracks[0]) {
      video.textTracks[0].mode = 'hidden';
    }
    video.volume = 0.7;
    label();

    if (opts.ad) {
      const ad = el('div', {'class': 'video-ads ytp-ad-module'},
                    el('span', {}, 'Ad · 0:15'));
      box.classList.add('ytp-ad-showing');
      box.append(ad);
      setTimeout(() => {
        ad.append(el('button', {
          'class': 'ytp-skip-ad-button', 'aria-label': 'Skip Ad',
          onclick: () => {
            ad.remove();
            box.classList.remove('ytp-ad-showing');
            kit.state.adSkipped = true;
          }}, 'Skip'));
      }, 3000);
    }
    kit.state.player = video;
    return video;
  };

  // In a frame: what the player does, for the page that embeds it.
  kit.reportToParent = function (video, origin) {
    const send = () => {
      const track = video.textTracks[0];
      parent.postMessage({moltMedia: {
        paused: video.paused, muted: video.muted, volume: video.volume,
        rate: video.playbackRate, time: video.currentTime,
        src: video.currentSrc || video.src,
        captions: !!track && track.mode === 'showing',
        fullscreen: !!document.fullscreenElement}}, origin);
    };
    for (const type of ['play', 'pause', 'volumechange', 'ratechange',
                        'timeupdate', 'seeked']) {
      video.addEventListener(type, send);
    }
    document.addEventListener('fullscreenchange', send);
    setInterval(send, 500);
    send();
  };

  // On the page that embeds a player from |origin|.
  kit.listenToFrame = function (origin) {
    window.addEventListener('message', (e) => {
      if (e.origin === origin && e.data && e.data.moltMedia) {
        window.__moltFrameMedia = e.data.moltMedia;
      }
    });
  };

  window.kit = kit;
})();
