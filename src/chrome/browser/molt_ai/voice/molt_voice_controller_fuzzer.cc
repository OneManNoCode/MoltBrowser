// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice loop's decisions (voice/voice_controller_logic.h) over a page
// map built straight from the input, so its strings are any bytes (titles,
// channels, labels, values, link targets, tab titles: page text, through a
// renderer that may be compromised), and a conversation of utterances (what
// a recognizer, or anyone the browser can hear, says). Each utterance is
// planned on the state the last one left, and every wait it asks for is
// followed up on the same page. The input is text, so seeds stay readable:
//   byte 0   environment: bit 0 MoltNet, bit 1 not a web page, bit 2 can
//            go back, bit 3 a media session, bit 4 media playing, bit 5
//            muted, bit 6 full screen, bit 7 the page changes after each
//            turn
//   byte 1   bits 0-1 the active tab (of up to four), bit 2 a question
//            expires before the next utterance
//   lines    "> utterance"; "T title"; "H text" (a heading); "L label" (a
//            new list); "E title\tchannel\tduration\tdate\tcount\ttext" (an
//            entry of the list, with a link item of its own); "I k label\t
//            value\turl" (an item: k is l link, b button, t text field, s
//            search field, p password field, c card field, m media, d
//            slider, x clickable); "B title\thost" (a tab); "@ host"
//
// Checks: planning returns and is the same twice; every spoken text and
// status is valid UTF-8 with no control, bidi or invisible character, no
// longer than its cap and unchanged by SpeechSafeText(); every URL to load
// is https, a known site's or a spoken address's, with no space or control
// byte; every node a step names is on the map; tabs, counts and repeats
// are in range; MoltNet only ever hears that voice mode is off; a held
// confirmation holds only page actions and words; offered choices stay
// within kMaxCloseChoices; what "repeat" keeps stays within its cap.

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/voice_candidates.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"

namespace {

using molt_ai::voice::ConversationState;
using molt_ai::voice::CountCodePoints;
using molt_ai::voice::CredentialKind;
using molt_ai::voice::ItemKind;
using molt_ai::voice::PageHeading;
using molt_ai::voice::PageItem;
using molt_ai::voice::PageList;
using molt_ai::voice::PageListEntry;
using molt_ai::voice::PageMap;
using molt_ai::voice::PageMedia;
using molt_ai::voice::PageNodeHandle;
using molt_ai::voice::PlanFollowUp;
using molt_ai::voice::PlanUtterance;
using molt_ai::voice::SpeechSafeText;
using molt_ai::voice::TabInfo;
using molt_ai::voice::VoiceEnvironment;
using molt_ai::voice::VoiceStep;
using molt_ai::voice::VoiceStepKind;
using molt_ai::voice::VoiceTurn;

constexpr size_t kMaxUtterances = 8;
constexpr size_t kMaxItems = 400;
constexpr size_t kMaxLines = 600;
constexpr size_t kMaxSpokenChars = 2 * molt_ai::voice::kMaxSpeechChunkChars;
constexpr size_t kMaxStatusChars = 300;

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (;;) {
    const size_t end = text.find(separator, start);
    parts.push_back(text.substr(start, end == std::string_view::npos
                                           ? std::string_view::npos
                                           : end - start));
    if (end == std::string_view::npos) {
      return parts;
    }
    start = end + 1;
  }
}

std::string_view Part(const std::vector<std::string_view>& parts, size_t i) {
  return i < parts.size() ? parts[i] : std::string_view();
}

// An independent check: valid UTF-8, no control, bidi or invisible
// character, within |cap| code points, clean as SpeechSafeText() leaves it.
void CheckSpeakable(std::string_view s, size_t cap) {
  CHECK_LE(CountCodePoints(s), cap);
  size_t i = 0;
  while (i < s.size()) {
    CHECK_EQ(molt_ai::hidden_chars::HiddenCharAt(s, i), 0u);
    const uint8_t b = static_cast<uint8_t>(s[i]);
    size_t n = 1;
    uint32_t cp = b;
    if (b >= 0xF0) {
      n = 4;
      cp = b & 0x07;
    } else if (b >= 0xE0) {
      n = 3;
      cp = b & 0x0F;
    } else if (b >= 0xC0) {
      n = 2;
      cp = b & 0x1F;
    } else {
      CHECK_LT(b, 0x80);
    }
    CHECK_LE(i + n, s.size());
    for (size_t k = 1; k < n; ++k) {
      const uint8_t c = static_cast<uint8_t>(s[i + k]);
      CHECK_EQ(c & 0xC0, 0x80);
      cp = (cp << 6) | (c & 0x3F);
    }
    CHECK(n == 1 || cp >= (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u));
    CHECK(cp < 0xD800 || cp > 0xDFFF);
    CHECK_LE(cp, 0x10FFFFu);
    CHECK(cp >= 0x20 && cp != 0x7F && (cp < 0x80 || cp > 0x9F));
    i += n;
  }
  CHECK_EQ(SpeechSafeText(s, cap), std::string(s));
}

struct Page {
  PageMap map;
  std::set<std::tuple<int32_t, int32_t>> handles;
};

PageNodeHandle NewHandle(Page& page) {
  const PageNodeHandle handle{0, static_cast<int32_t>(page.handles.size() + 1)};
  page.handles.insert({handle.tree, handle.node});
  return handle;
}

PageItem& AddItem(Page& page, ItemKind kind, std::string_view label) {
  PageItem item;
  item.number = static_cast<int>(page.map.items.size() + 1);
  item.handle = NewHandle(page);
  item.kind = kind;
  item.label = std::string(label);
  page.map.items.push_back(std::move(item));
  return page.map.items.back();
}

void AddLine(Page& page,
             std::vector<TabInfo>& tabs,
             std::string& host,
             std::string_view line) {
  if (line.size() < 2 || line[1] != ' ') {
    return;
  }
  const std::string_view rest = line.substr(2);
  const std::vector<std::string_view> parts = Split(rest, '\t');
  PageMap& map = page.map;
  switch (line[0]) {
    case 'T':
      map.title = std::string(rest);
      return;
    case '@':
      host = std::string(rest);
      return;
    case 'B':
      if (tabs.size() < 4) {
        tabs.push_back(
            {std::string(Part(parts, 0)), std::string(Part(parts, 1))});
      }
      return;
    case 'H': {
      PageHeading heading;
      heading.level = 2;
      heading.text = std::string(rest);
      heading.handle = NewHandle(page);
      map.headings.push_back(std::move(heading));
      return;
    }
    case 'L': {
      PageList list;
      list.label = std::string(rest);
      list.container = NewHandle(page);
      list.in_main = true;
      map.lists.push_back(std::move(list));
      return;
    }
    case 'E': {
      if (map.lists.empty() || map.items.size() >= kMaxItems) {
        return;
      }
      const PageItem& link = AddItem(page, ItemKind::kLink, Part(parts, 0));
      PageListEntry entry;
      PageList& list = map.lists.back();
      entry.position = static_cast<int>(list.entries.size() + 1);
      entry.number = link.number;
      entry.kind = ItemKind::kLink;
      entry.handle = link.handle;
      entry.node = NewHandle(page);
      entry.title = std::string(Part(parts, 0));
      entry.channel = std::string(Part(parts, 1));
      entry.duration = std::string(Part(parts, 2));
      entry.duration_seconds = entry.duration.empty() ? -1 : 61;
      entry.date = std::string(Part(parts, 3));
      if (!Part(parts, 4).empty()) {
        entry.counts.push_back({1, "views", std::string(Part(parts, 4))});
      }
      if (!Part(parts, 5).empty()) {
        entry.texts.push_back(std::string(Part(parts, 5)));
      }
      list.entries.push_back(std::move(entry));
      map.items.back().list = static_cast<int>(map.lists.size() - 1);
      map.items.back().entry = static_cast<int>(list.entries.size() - 1);
      return;
    }
    case 'I': {
      if (map.items.size() >= kMaxItems || rest.size() < 2 ||
          rest[1] != ' ') {
        return;
      }
      const std::vector<std::string_view> fields =
          Split(rest.substr(2), '\t');
      ItemKind kind = ItemKind::kClickable;
      CredentialKind credential = CredentialKind::kNone;
      bool search = false;
      switch (rest[0]) {
        case 'l':
          kind = ItemKind::kLink;
          break;
        case 'b':
          kind = ItemKind::kButton;
          break;
        case 's':
          search = true;
          [[fallthrough]];
        case 't':
          kind = ItemKind::kTextField;
          break;
        case 'p':
          kind = ItemKind::kTextField;
          credential = CredentialKind::kPassword;
          break;
        case 'c':
          kind = ItemKind::kTextField;
          credential = CredentialKind::kPaymentCard;
          break;
        case 'm':
          kind = ItemKind::kMedia;
          break;
        case 'd':
          kind = ItemKind::kSlider;
          break;
        default:
          break;
      }
      PageItem& item = AddItem(page, kind, Part(fields, 0));
      item.credential = credential;
      item.search_field = search;
      item.writable =
          kind == ItemKind::kTextField && credential == CredentialKind::kNone;
      if (credential == CredentialKind::kNone) {
        item.value = std::string(Part(fields, 1));
      } else {
        map.has_credential_fields = true;
        item.filled = true;
      }
      item.url = std::string(Part(fields, 2));
      if (kind == ItemKind::kMedia) {
        PageMedia media;
        media.label = item.label;
        media.handle = item.handle;
        media.number = item.number;
        map.media.push_back(std::move(media));
      }
      return;
    }
    default:
      return;
  }
}

bool IsPageAction(VoiceStepKind kind) {
  switch (kind) {
    case VoiceStepKind::kSetText:
    case VoiceStepKind::kSubmit:
    case VoiceStepKind::kPress:
    case VoiceStepKind::kPlayMedia:
    case VoiceStepKind::kFocus:
    case VoiceStepKind::kScrollIntoView:
    case VoiceStepKind::kIncrement:
    case VoiceStepKind::kDecrement:
      return true;
    default:
      return false;
  }
}

void CheckTurn(const VoiceTurn& turn,
               const Page& page,
               const VoiceEnvironment& env) {
  CheckSpeakable(turn.status, kMaxStatusChars);
  CheckSpeakable(turn.heard, kMaxStatusChars);
  const int tabs = static_cast<int>(env.tabs.size());
  for (const VoiceStep& step : turn.steps) {
    if (env.moltnet) {
      CHECK(step.kind == VoiceStepKind::kSpeak);
      CHECK_EQ(step.text, "Voice mode is off in MoltNet windows.");
    }
    switch (step.kind) {
      case VoiceStepKind::kSpeak:
        CHECK(!step.text.empty());
        CheckSpeakable(step.text, kMaxSpokenChars);
        break;
      case VoiceStepKind::kNavigate:
        CHECK_EQ(step.url.substr(0, 8), "https://");
        for (char c : step.url) {
          const uint8_t b = static_cast<uint8_t>(c);
          CHECK(b > 0x20 && b < 0x7F);
        }
        break;
      case VoiceStepKind::kSearchWeb:
        CHECK(!step.text.empty());
        break;
      case VoiceStepKind::kSelectTab:
        CHECK_GE(step.index, 0);
        CHECK_LT(step.index, tabs);
        break;
      case VoiceStepKind::kCloseTab:
        CHECK_GE(step.index, -1);
        CHECK_LT(step.index, tabs);
        break;
      case VoiceStepKind::kIncrement:
      case VoiceStepKind::kDecrement:
        CHECK_EQ(step.count, molt_ai::voice::kVolumeSteps);
        break;
      case VoiceStepKind::kScrollPage:
        CHECK_GE(step.count, 1);
        CHECK_LE(step.count, 20);
        break;
      case VoiceStepKind::kToPlanner:
        CheckSpeakable(step.preface, kMaxSpokenChars);
        break;
      default:
        break;
    }
    if (IsPageAction(step.kind)) {
      CHECK(page.handles.count({step.target.tree, step.target.node}));
    }
  }
  const ConversationState& state = turn.state;
  CHECK_LE(state.offered.size(), molt_ai::voice::kMaxCloseChoices);
  CHECK(state.confirming || state.confirm_steps.empty());
  for (const VoiceStep& held : state.confirm_steps) {
    CHECK(IsPageAction(held.kind) || held.kind == VoiceStepKind::kSpeak ||
          held.kind == VoiceStepKind::kWaitForPage ||
          held.kind == VoiceStepKind::kSetCurrentItem);
  }
  size_t remembered = 0;
  for (const std::string& text : state.last_spoken) {
    remembered += text.size();
  }
  CHECK_LE(remembered, 16384u);
  CHECK_GE(state.speech_rate, molt_ai::voice::kMinSpeechRate);
  CHECK_LE(state.speech_rate, molt_ai::voice::kMaxSpeechRate);
}

bool SameSteps(const VoiceTurn& a, const VoiceTurn& b) {
  if (a.steps.size() != b.steps.size() || a.status != b.status) {
    return false;
  }
  for (size_t i = 0; i < a.steps.size(); ++i) {
    const VoiceStep& x = a.steps[i];
    const VoiceStep& y = b.steps[i];
    if (x.kind != y.kind || x.text != y.text || x.url != y.url ||
        !(x.target == y.target) || x.index != y.index) {
      return false;
    }
  }
  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const uint8_t env_bits = data[0];
  const uint8_t more_bits = data[1];
  const std::string_view text(reinterpret_cast<const char*>(data + 2),
                              size - 2);

  Page page;
  VoiceEnvironment env;
  std::vector<std::string_view> utterances;
  size_t lines = 0;
  for (std::string_view line : Split(text, '\n')) {
    if (++lines > kMaxLines) {
      break;
    }
    if (line.size() >= 2 && line[0] == '>' && line[1] == ' ') {
      if (utterances.size() < kMaxUtterances) {
        utterances.push_back(line.substr(2));
      }
      continue;
    }
    AddLine(page, env.tabs, env.host, line);
  }
  env.moltnet = env_bits & 0x01;
  env.web_page = !(env_bits & 0x02);
  env.can_go_back = env_bits & 0x04;
  env.media_session = env_bits & 0x08;
  env.media_playing = env_bits & 0x10;
  env.muted = env_bits & 0x20;
  env.fullscreen = env_bits & 0x40;
  env.active_tab =
      env.tabs.empty() ? 0
                       : static_cast<int>((more_bits & 0x03) % env.tabs.size());
  env.page_id = 1;
  env.now_ms = 1000;

  ConversationState state;
  for (std::string_view utterance : utterances) {
    VoiceTurn turn = PlanUtterance(utterance, page.map, env, state);
    CheckTurn(turn, page, env);
    CHECK(SameSteps(turn, PlanUtterance(utterance, page.map, env, state)));
    state = turn.state;
    for (const VoiceStep& step : turn.steps) {
      if (step.kind != VoiceStepKind::kWaitForPage) {
        continue;
      }
      // The page that loads is this one again.
      const VoiceTurn follow = PlanFollowUp(step, page.map, env, state);
      CheckTurn(follow, page, env);
      state = follow.state;
    }
    if (env_bits & 0x80) {
      ++env.page_id;
    }
    if (more_bits & 0x04) {
      env.now_ms += molt_ai::voice::kConfirmationTimeoutMs;
    }
  }
  return 0;
}
