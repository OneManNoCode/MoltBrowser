// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/activation_policy.h"

#include <algorithm>

#include "chrome/browser/molt_ai/security/page_request_asker.h"

namespace molt_ai {
namespace activation {

namespace {

constexpr Ruling kProceed{Decision::kProceed, false};

bool IsLowerAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// |text| lower-cased, keeping letters, digits and |extra|, at most |max|.
std::string Keep(std::string_view text, std::string_view extra, size_t max) {
  std::string out;
  for (char c : text) {
    if (out.size() >= max) {
      break;
    }
    c = LowerAscii(c);
    if (IsLowerAlnum(c) || extra.find(c) != std::string_view::npos) {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

void NoteAction(TabRecord& tab, int64_t now_ms, Intent intent) {
  tab.action_ms = now_ms;
  tab.intent = intent;
  tab.activation_ms = -1;
  tab.user_input = false;
  tab.history_entries = 0;
}

void NoteActivation(TabRecord& tab, int64_t now_ms) {
  if (IsActing(tab, now_ms)) {
    tab.activation_ms = std::max(tab.activation_ms, now_ms);
  }
}

void NoteUserInput(TabRecord& tab) {
  if (tab.action_ms >= 0) {
    tab.user_input = true;
  }
}

bool IsActing(const TabRecord& tab, int64_t now_ms) {
  if (tab.action_ms < 0 || tab.user_input) {
    return false;
  }
  const int64_t from = std::max(tab.action_ms, tab.activation_ms);
  // A clock that went back (a monotonic one doesn't) counts as acting.
  return now_ms <= from || now_ms - from < tab.window_ms;
}

bool NoteHistoryEntry(TabRecord& tab, int64_t now_ms) {
  // The page's activation is the user's own (or there was none).
  if (tab.action_ms < 0 || tab.user_input) {
    return false;
  }
  const bool acting = IsActing(tab, now_ms);
  if (tab.history_entries < 1000) {
    ++tab.history_entries;
  }
  // The page the action led to is what the user asked for.
  return !(acting && tab.history_entries == 1);
}

Ruling Decide(const TabRecord& tab,
              int64_t now_ms,
              Request request,
              Asker asker,
              bool prompts_by_voice) {
  if (!IsActing(tab, now_ms)) {
    return kProceed;
  }
  const bool voice = asker == Asker::kVoiceMode;
  const Ruling ask_or_refuse = asker == Asker::kNone
                                   ? Ruling{Decision::kRefuse, false}
                                   : Ruling{Decision::kAsk, false};
  switch (request) {
    case Request::kNewWindow:
      if (tab.intent == Intent::kOpenInNewTab) {
        return kProceed;
      }
      return ask_or_refuse;
    case Request::kDownload:
    case Request::kExternalApp:
      return ask_or_refuse;
    case Request::kPermissionPrompt:
      // The activation only let the page ask; the answer is the user's.
      // Voice mode that reads prompts asks it by voice, and allowing takes
      // the prompt's confirmation word (the voice gesture,
      // TOUCHLESS_BROWSING.md §7.3). One that can't (only typed text) has
      // it refused and said: the user may not see it. Shown as usual
      // otherwise: the user answers it, never the agent.
      if (voice) {
        return prompts_by_voice ? Ruling{Decision::kAsk, false}
                                : Ruling{Decision::kRefuse, true};
      }
      return kProceed;
    case Request::kFullScreen:
      if (tab.intent == Intent::kFullScreen ||
          tab.intent == Intent::kPlayMedia) {
        return kProceed;
      }
      return Ruling{Decision::kRefuse, voice};
    case Request::kClipboardWrite:
      if (tab.intent == Intent::kCopy) {
        return kProceed;
      }
      return Ruling{Decision::kRefuse, voice};
  }
  return Ruling{Decision::kRefuse, false};
}

bool PageRequestAsker::TakesPromptsByVoice() const {
  return false;
}

std::string_view RequestName(Request request) {
  switch (request) {
    case Request::kNewWindow:
      return "new window";
    case Request::kDownload:
      return "download";
    case Request::kExternalApp:
      return "external app";
    case Request::kPermissionPrompt:
      return "permission prompt";
    case Request::kFullScreen:
      return "full screen";
    case Request::kClipboardWrite:
      return "clipboard write";
  }
  return "unknown";
}

std::string_view DecisionName(Decision decision) {
  switch (decision) {
    case Decision::kProceed:
      return "proceed";
    case Decision::kAsk:
      return "ask";
    case Decision::kRefuse:
      return "refuse";
  }
  return "unknown";
}

std::string SiteForSpeech(std::string_view host) {
  // GURL's hosts are canonical already; the browser cleans them again, so
  // a host is never more than letters, digits, dots and dashes when said.
  std::string site = Keep(host, ".-", kMaxSiteChars + 4);
  if (site.rfind("www.", 0) == 0) {
    site.erase(0, 4);
  }
  if (site.size() > kMaxSiteChars) {
    site.resize(kMaxSiteChars);
  }
  // No leading or trailing punctuation ("." alone says nothing).
  const size_t first = site.find_first_not_of(".-");
  if (first == std::string::npos) {
    return std::string();
  }
  const size_t last = site.find_last_not_of(".-");
  return site.substr(first, last - first + 1);
}

std::string SchemeForSpeech(std::string_view scheme) {
  return Keep(scheme, "+-.", kMaxSchemeChars);
}

Question QuestionFor(Request request,
                     std::string_view site,
                     std::string_view scheme) {
  const std::string clean_site = SiteForSpeech(site);
  const std::string to_site = clean_site.empty() ? "" : " to " + clean_site;
  const std::string from_site =
      clean_site.empty() ? "" : " from " + clean_site;
  Question question;
  question.request = request;
  switch (request) {
    case Request::kNewWindow:
      question.said = "The page tried to open a new window" + to_site + ".";
      question.verb = "open it";
      return question;
    case Request::kDownload:
      question.said = "The page tried to download a file" + from_site + ".";
      question.verb = "download it";
      return question;
    case Request::kExternalApp: {
      const std::string clean_scheme = SchemeForSpeech(scheme);
      question.said =
          clean_scheme.empty()
              ? std::string("The page tried to open another app.")
              : "The page tried to open another app with a " + clean_scheme +
                    " link.";
      question.verb = "open it";
      return question;
    }
    case Request::kPermissionPrompt:
    case Request::kFullScreen:
    case Request::kClipboardWrite:
      break;
  }
  // Never asked about: refused or shown as usual.
  question.said = "The page asked for something.";
  question.verb = "allow it";
  return question;
}

std::string RefusalNotice(Request request, std::string_view what) {
  switch (request) {
    case Request::kPermissionPrompt: {
      // The prompt's subject is the browser's word for it; cleaned anyway.
      const std::string clean = Keep(what, " -", 40);
      const std::string asked =
          clean.empty() ? std::string("The page asked for a permission.")
                        : "The page asked for " + clean + " permission.";
      return asked +
             " I can't take an answer to that by voice right now, so I "
             "turned it down.";
    }
    case Request::kFullScreen:
      return "The page tried to go full screen. I stopped it. Say full "
             "screen if you want it.";
    case Request::kClipboardWrite:
      return "The page tried to change your clipboard. I stopped it.";
    case Request::kNewWindow:
      return "The page tried to open a new window. I stopped it.";
    case Request::kDownload:
      return "The page tried to download a file. I stopped it.";
    case Request::kExternalApp:
      return "The page tried to open another app. I stopped it.";
  }
  return std::string();
}

}  // namespace activation
}  // namespace molt_ai
