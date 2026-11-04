// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_media_controls.h"

#include <algorithm>
#include <string>

#include "chrome/browser/molt_ai/voice/spoken_answers.h"

namespace molt_ai {
namespace voice {

namespace {

// The script: a function of the operation and its value, called by
// MediaScript(). It touches the one media element it picks, and returns
// what that element is then. An exception (a speed the element refuses)
// leaves the element as it was and is reported as it is.
constexpr char kMediaFunction[] = R"JS((op, value) => {
  const media = Array.from(document.querySelectorAll('video, audio'));
  if (media.length === 0) {
    return {found: false};
  }
  const area = (m) => {
    const r = m.getBoundingClientRect();
    return Math.max(0, r.width) * Math.max(0, r.height);
  };
  let target = media.find((m) => !m.paused && !m.ended);
  if (!target) {
    target = media.reduce(
        (best, m) => (area(m) > area(best) ? m : best), media[0]);
  }
  const tracks = Array.from(target.textTracks || []).filter(
      (t) => t.kind === 'captions' || t.kind === 'subtitles');
  try {
    if (op === 1) {
      target.playbackRate = value / 100;
    } else if (op === 2) {
      target.volume = value / 100;
      if (value > 0) {
        target.muted = false;
      }
    } else if (op === 3) {
      tracks.forEach((t, i) => {
        t.mode = value === 1 && i === 0 ? 'showing' : 'disabled';
      });
    }
  } catch (e) {
  }
  return {
    found: true,
    rate: Math.round(target.playbackRate * 100),
    volume: Math.round(target.volume * 100),
    muted: target.muted,
    tracks: tracks.length,
    showing: tracks.some((t) => t.mode === 'showing'),
  };
})JS";

int OpCode(MediaOp op) {
  switch (op) {
    case MediaOp::kQuery:
      return 0;
    case MediaOp::kSetRate:
      return 1;
    case MediaOp::kSetVolume:
      return 2;
    case MediaOp::kCaptions:
      return 3;
  }
  return 0;
}

}  // namespace

int ClampPlaybackRate(int rate) {
  return std::clamp(rate, kMinPlaybackRate, kMaxPlaybackRate);
}

int NextPlaybackRate(int current, bool faster) {
  current = ClampPlaybackRate(current);
  if (faster) {
    for (int step : kPlaybackRateSteps) {
      if (step > current) {
        return step;
      }
    }
    return current;
  }
  int next = current;
  for (int step : kPlaybackRateSteps) {
    if (step < current) {
      next = step;
    }
  }
  return next;
}

std::string SpokenRate(int rate, Language language) {
  rate = ClampPlaybackRate(rate);
  std::string out = std::to_string(rate / 100);
  int fraction = rate % 100;
  if (fraction == 0) {
    return out;
  }
  out += language == Language::kSpanish ? ',' : '.';
  out += static_cast<char>('0' + fraction / 10);
  if (fraction % 10 != 0) {
    out += static_cast<char>('0' + fraction % 10);
  }
  return out;
}

std::string MediaScript(MediaOp op, int value) {
  switch (op) {
    case MediaOp::kQuery:
      value = 0;
      break;
    case MediaOp::kSetRate:
      value = ClampPlaybackRate(value);
      break;
    case MediaOp::kSetVolume:
      value = std::clamp(value, 0, 100);
      break;
    case MediaOp::kCaptions:
      value = value ? 1 : 0;
      break;
  }
  return std::string("(") + kMediaFunction + ")(" + std::to_string(OpCode(op)) +
         ", " + std::to_string(value) + ")";
}

std::string MediaOutcomeSpeech(MediaOp op,
                               int asked,
                               const MediaState& state,
                               bool tab_muted,
                               Language language) {
  if (!state.found) {
    return std::string(
        Translated(language, "There's no video or audio on this page."));
  }
  switch (op) {
    case MediaOp::kQuery:
      return std::string();
    case MediaOp::kSetRate:
      if (state.rate == kNormalPlaybackRate) {
        return std::string(Translated(language, "Normal speed."));
      }
      if (asked > 0 && state.rate != ClampPlaybackRate(asked)) {
        return Said(language,
                    "This player doesn't go to that speed. Speed: {0} times.",
                    {SpokenRate(state.rate, language)});
      }
      return Said(language, "Speed: {0} times.",
                  {SpokenRate(state.rate, language)});
    case MediaOp::kSetVolume: {
      std::string said =
          Said(language, "Volume {0} percent.",
               {std::to_string(std::clamp(state.volume, 0, 100))});
      if (tab_muted && state.volume > 0) {
        said +=
            " " + std::string(Translated(
                      language, "The tab is muted: say unmute to hear it."));
      }
      return said;
    }
    case MediaOp::kCaptions:
      if (state.caption_tracks == 0) {
        return std::string(
            Translated(language, "I can't find captions for this video."));
      }
      return std::string(Translated(
          language, state.captions_showing ? "Captions on." : "Captions off."));
  }
  return std::string();
}

}  // namespace voice
}  // namespace molt_ai
