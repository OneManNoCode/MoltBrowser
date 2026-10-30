// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_prompts.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"

namespace molt_ai {
namespace voice {

namespace {

// What is compared of an answer, at most.
constexpr size_t kMaxAnswerScanBytes = 200;
constexpr size_t kMaxWhatChars = 60;

using Phrases = std::span<const std::string_view>;

constexpr auto kYes = std::to_array<std::string_view>(
    {"yes", "ok", "okay", "sure", "yeah", "yep", "yes please", "confirm",
     "ok yes", "okay yes", "thats fine", "fine"});
constexpr auto kNo = std::to_array<std::string_view>(
    {"no", "nope", "cancel", "no thanks", "no thank you", "cancel it", "dont",
     "no dont"});
constexpr auto kBlock = std::to_array<std::string_view>(
    {"no", "block", "block it", "deny", "deny it", "dont allow", "never allow",
     "no thanks", "no thank you", "dont allow it", "nope"});
// A permission prompt dismissed, not blocked: nothing is remembered.
constexpr auto kDismiss = std::to_array<std::string_view>(
    {"cancel", "not now", "dismiss", "dismiss it", "later", "ignore it"});
constexpr auto kAllowWords = std::to_array<std::string_view>(
    {"yes", "allow", "allow it", "ok", "okay", "sure", "yes please",
     "yes allow", "yes allow it", "keep", "keep it", "yes keep it"});
constexpr auto kClose = std::to_array<std::string_view>(
    {"ok", "okay", "yes", "close", "close it", "dismiss", "got it",
     "ok close it", "no", "cancel"});
constexpr auto kLeave = std::to_array<std::string_view>(
    {"leave", "leave it", "leave the page", "leave page", "yes", "yes leave",
     "reload", "reload it", "yes reload", "ok", "okay"});
constexpr auto kStay = std::to_array<std::string_view>(
    {"stay", "stay here", "stay on the page", "stay on page", "no", "cancel",
     "dont leave", "dont reload", "no stay"});
constexpr auto kDiscard = std::to_array<std::string_view>(
    {"discard", "discard it", "delete", "delete it", "remove", "remove it",
     "no", "dont keep it", "cancel"});
constexpr auto kSave = std::to_array<std::string_view>(
    {"save", "save it", "yes", "save password", "yes save", "save the password",
     "ok", "okay"});
constexpr auto kNever = std::to_array<std::string_view>(
    {"never", "never save", "never for this site", "never save it"});
constexpr auto kNotNow = std::to_array<std::string_view>(
    {"not now", "no", "no thanks", "no thank you", "cancel", "later",
     "not now thanks"});

bool In(Phrases phrases, std::string_view phrase) {
  return std::find(phrases.begin(), phrases.end(), phrase) != phrases.end();
}

// |heard| folded, without "please" at either end.
std::string Folded(std::string_view heard) {
  std::string folded = FoldForComparison(
      heard.substr(0, std::min(heard.size(), kMaxAnswerScanBytes)));
  for (std::string_view polite : {"please ", "um ", "uh "}) {
    while (folded.rfind(polite, 0) == 0) {
      folded.erase(0, polite.size());
    }
  }
  for (std::string_view polite : {" please", " thanks", " thank you"}) {
    if (folded.size() > polite.size() &&
        folded.compare(folded.size() - polite.size(), polite.size(), polite) ==
            0) {
      folded.resize(folded.size() - polite.size());
    }
  }
  return folded;
}

// Lower-case letters and single spaces of the browser's own words.
std::string Words(std::string_view text, size_t max) {
  std::string out;
  for (char c : text) {
    if (out.size() >= max) {
      break;
    }
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
      out.push_back(c);
    } else if (!out.empty() && out.back() != ' ') {
      out.push_back(' ');
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string Site(const BrowserPrompt& prompt) {
  const std::string site = activation::SiteForSpeech(prompt.site);
  return site.empty() ? std::string("This page") : site;
}

// The dictated answer: what follows "answer" or "type" in |heard|, as said.
std::optional<std::string> Dictated(std::string_view heard,
                                    const std::string& folded) {
  static constexpr std::array<std::string_view, 5> kLeads = {
      "answer ", "type ", "my answer is ", "the answer is ", "reply "};
  size_t words = 0;
  for (std::string_view lead : kLeads) {
    if (folded.rfind(lead, 0) == 0 && folded.size() > lead.size()) {
      words = static_cast<size_t>(std::count(lead.begin(), lead.end(), ' '));
      break;
    }
  }
  if (words == 0) {
    return std::nullopt;
  }
  // Skip that many words of what was said (its case and punctuation kept).
  const std::string clean = CleanSpokenText(heard, kMaxPromptAnswerBytes * 2);
  size_t pos = 0;
  for (size_t i = 0; i < words; ++i) {
    pos = clean.find(' ', pos);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    ++pos;
  }
  std::string text = CleanSpokenText(std::string_view(clean).substr(pos),
                                     kMaxPromptAnswerBytes);
  // "answer: Bob" keeps "Bob".
  while (!text.empty() &&
         (text.front() == ':' || text.front() == ',' || text.front() == ' ')) {
    text.erase(0, 1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  return text;
}

PromptReply Reply(PromptAnswer answer) {
  PromptReply reply;
  reply.answer = answer;
  return reply;
}

// The gesture's word, another one, one not for sure, or none.
std::optional<PromptAnswer> WordAnswer(const PendingPrompt& prompt,
                                       std::string_view heard) {
  const HeardWord word = HearConfirmationWord(heard);
  if (word.kind == HeardWord::Kind::kNone) {
    return std::nullopt;
  }
  if (prompt.word.empty()) {
    return PromptAnswer::kCantAllow;
  }
  if (word.kind == HeardWord::Kind::kUnsure) {
    return PromptAnswer::kSayAgain;
  }
  return ConfirmationWords()[word.index] == prompt.word
             ? PromptAnswer::kAllow
             : PromptAnswer::kWrongWord;
}

}  // namespace

BrowserPrompt::BrowserPrompt() = default;
BrowserPrompt::~BrowserPrompt() = default;
BrowserPrompt::BrowserPrompt(const BrowserPrompt&) = default;
BrowserPrompt& BrowserPrompt::operator=(const BrowserPrompt&) = default;
BrowserPrompt::BrowserPrompt(BrowserPrompt&&) = default;
BrowserPrompt& BrowserPrompt::operator=(BrowserPrompt&&) = default;

PendingPrompt::PendingPrompt() = default;
PendingPrompt::~PendingPrompt() = default;
PendingPrompt::PendingPrompt(const PendingPrompt&) = default;
PendingPrompt& PendingPrompt::operator=(const PendingPrompt&) = default;
PendingPrompt::PendingPrompt(PendingPrompt&&) = default;
PendingPrompt& PendingPrompt::operator=(PendingPrompt&&) = default;

std::string_view PromptKindName(PromptKind kind) {
  switch (kind) {
    case PromptKind::kPermission:
      return "permission";
    case PromptKind::kAlert:
      return "alert";
    case PromptKind::kConfirm:
      return "confirm";
    case PromptKind::kTextPrompt:
      return "prompt";
    case PromptKind::kBeforeUnload:
      return "beforeunload";
    case PromptKind::kDangerousDownload:
      return "dangerous download";
    case PromptKind::kSavePassword:
      return "save password";
    case PromptKind::kConfirmSites:
      return "confirm sites";
  }
  return "unknown";
}

std::string_view PromptAnswerName(PromptAnswer answer) {
  switch (answer) {
    case PromptAnswer::kNone:
      return "none";
    case PromptAnswer::kAllow:
      return "allow";
    case PromptAnswer::kWrongWord:
      return "wrong word";
    case PromptAnswer::kRefuse:
      return "refuse";
    case PromptAnswer::kAccept:
      return "accept";
    case PromptAnswer::kDecline:
      return "decline";
    case PromptAnswer::kDictate:
      return "dictate";
    case PromptAnswer::kNeedWord:
      return "need word";
    case PromptAnswer::kSayAgain:
      return "say again";
    case PromptAnswer::kCantAllow:
      return "can't allow";
  }
  return "unknown";
}

bool NeedsGesture(PromptKind kind) {
  return kind == PromptKind::kPermission ||
         kind == PromptKind::kDangerousDownload ||
         kind == PromptKind::kConfirmSites;
}

int64_t PromptWindowMs(PromptKind kind) {
  switch (kind) {
    case PromptKind::kAlert:
    case PromptKind::kConfirm:
    case PromptKind::kTextPrompt:
    case PromptKind::kBeforeUnload:
      return kDialogWindowMs;
    case PromptKind::kPermission:
    case PromptKind::kDangerousDownload:
    case PromptKind::kSavePassword:
    case PromptKind::kConfirmSites:
      return kPromptWindowMs;
  }
  return kPromptWindowMs;
}

bool PromptWaits(const PendingPrompt& prompt, int64_t now_ms) {
  return prompt.id != 0 && now_ms < prompt.expires_ms;
}

std::string PromptSpeech(const BrowserPrompt& prompt, std::string_view word) {
  const std::string site = Site(prompt);
  const std::string text = SpeechSafeText(prompt.text, kMaxDialogSpokenChars);
  // The page's text ends with the site's sentence; the browser's words
  // follow.
  const std::string says =
      text.empty() ? std::string()
                   : text + (text.back() == '.' || text.back() == '?' ||
                                     text.back() == '!'
                                 ? ""
                                 : ".");
  switch (prompt.kind) {
    case PromptKind::kPermission: {
      const std::string what = Words(prompt.what, kMaxWhatChars);
      std::string out = what.empty() ? site + " is asking for a permission."
                                     : site + " wants to " + what + ".";
      if (!word.empty()) {
        out +=
            " To allow it, say " + std::string(word) + ". To block it, say no.";
      } else {
        out +=
            " To block it, say no. To allow it, use the mouse or "
            "keyboard.";
      }
      return out;
    }
    case PromptKind::kAlert:
      if (prompt.flood) {
        return site + " shows another message. Say OK to close it.";
      }
      return says.empty() ? site + " shows a message. Say OK to close it."
                          : site + " says: " + says + " Say OK to close it.";
    case PromptKind::kConfirm:
      if (prompt.flood) {
        return site + " asks another question. Say yes or no.";
      }
      return says.empty() ? site + " asks you to confirm. Say yes or no."
                          : site + " asks: " + says + " Say yes or no.";
    case PromptKind::kTextPrompt: {
      const std::string ask = prompt.flood || says.empty()
                                  ? site + " asks for an answer."
                                  : site + " asks: " + says;
      return ask + " Say answer, then your answer, or say no to cancel.";
    }
    case PromptKind::kBeforeUnload:
      return prompt.reload
                 ? "Reload " + site +
                       "? Changes you made may not be saved. Say reload or "
                       "stay."
                 : "Leave " + site +
                       "? Changes you made may not be saved. Say leave or "
                       "stay.";
    case PromptKind::kDangerousDownload: {
      const std::string name =
          SpeechSafeText(prompt.text, kMaxFileNameSpokenChars);
      const std::string file =
          name.empty() ? std::string("A file") : "The file " + name;
      if (prompt.malicious) {
        return file + " from " + site +
               " is dangerous, so it was blocked. To delete it, say "
               "discard. To keep it anyway, use the downloads page.";
      }
      std::string out = file + " from " + site + " may be harmful.";
      if (!word.empty()) {
        out += " To keep it, say " + std::string(word) +
               ". To delete it, say discard.";
      } else {
        out +=
            " To delete it, say discard. To keep it, use the downloads "
            "page.";
      }
      return out;
    }
    case PromptKind::kSavePassword:
      return "Save your password for " + site +
             "? Say save, never, or not now.";
    case PromptKind::kConfirmSites: {
      // The sites, each already a cleaned host ("a.example and b.example");
      // cleaned again here.
      const std::string sites =
          SpeechSafeText(prompt.text, kMaxDialogSpokenChars);
      std::string out =
          sites.empty() ? std::string("Confirm the sites this routine uses?")
                        : "Confirm the sites this routine uses: " + sites + "?";
      if (!word.empty()) {
        out += " To confirm them, say " + std::string(word) +
               ". To leave them unconfirmed, say no.";
      } else {
        out += " Confirm them in the Agent studio, or say no.";
      }
      return out;
    }
  }
  return std::string();
}

std::string PromptTimeoutSpeech(PromptKind kind) {
  switch (kind) {
    case PromptKind::kPermission:
      return "No answer, so I dismissed the permission request.";
    case PromptKind::kDangerousDownload:
      return "No answer, so the file stays blocked. You can decide on the "
             "downloads page.";
    case PromptKind::kConfirmSites:
      return "No answer, so the sites stay unconfirmed.";
    case PromptKind::kAlert:
    case PromptKind::kConfirm:
    case PromptKind::kTextPrompt:
    case PromptKind::kBeforeUnload:
    case PromptKind::kSavePassword:
      return std::string();
  }
  return std::string();
}

std::string NotCleanSpeech(std::string_view word) {
  return "I couldn't be sure that was you. When it's quiet and I've "
         "finished speaking, say " +
         std::string(word) + ".";
}

PromptReply ParsePromptReply(const PendingPrompt& prompt,
                             std::string_view heard) {
  const std::string folded = Folded(heard);
  if (folded.empty()) {
    return PromptReply();
  }
  switch (prompt.kind) {
    case PromptKind::kPermission:
      if (In(kBlock, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      if (In(kDismiss, folded)) {
        return Reply(PromptAnswer::kDecline);
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(kAllowWords, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kAlert:
      return In(kClose, folded) ? Reply(PromptAnswer::kAccept) : PromptReply();
    case PromptKind::kConfirm:
      if (In(kYes, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(kNo, folded) ? Reply(PromptAnswer::kDecline) : PromptReply();
    case PromptKind::kTextPrompt: {
      if (std::optional<std::string> text = Dictated(heard, folded)) {
        PromptReply reply = Reply(PromptAnswer::kDictate);
        reply.text = std::move(*text);
        return reply;
      }
      if (In(kYes, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(kNo, folded) ? Reply(PromptAnswer::kDecline) : PromptReply();
    }
    case PromptKind::kBeforeUnload:
      if (In(kLeave, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(kStay, folded) ? Reply(PromptAnswer::kDecline) : PromptReply();
    case PromptKind::kDangerousDownload:
      if (In(kDiscard, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      if (prompt.malicious) {
        // Never kept by voice: no word was offered.
        if (HearConfirmationWord(heard).kind != HeardWord::Kind::kNone ||
            In(kAllowWords, folded)) {
          return Reply(PromptAnswer::kCantAllow);
        }
        return PromptReply();
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(kAllowWords, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kConfirmSites:
      if (In(kNo, folded) || In(kDismiss, folded)) {
        return Reply(PromptAnswer::kDecline);
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(kYes, folded) || In(kAllowWords, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kSavePassword:
      if (In(kSave, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      if (In(kNever, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      return In(kNotNow, folded) ? Reply(PromptAnswer::kDecline)
                                 : PromptReply();
  }
  return PromptReply();
}

std::string ReplySpeech(const PendingPrompt& prompt, const PromptReply& reply) {
  const bool download = prompt.kind == PromptKind::kDangerousDownload;
  const bool sites = prompt.kind == PromptKind::kConfirmSites;
  switch (reply.answer) {
    case PromptAnswer::kNone:
      return std::string();
    case PromptAnswer::kAllow:
      return download ? "Kept the file." : sites ? "Confirmed." : "Allowed.";
    case PromptAnswer::kWrongWord:
      return download ? "That wasn't the word, so I didn't keep the file."
                      : "That wasn't the word, so I turned it down.";
    case PromptAnswer::kRefuse:
      switch (prompt.kind) {
        case PromptKind::kDangerousDownload:
          return "Deleted the file.";
        case PromptKind::kSavePassword:
          return "I won't offer to save passwords for this site.";
        default:
          return "Blocked.";
      }
    case PromptAnswer::kAccept:
      switch (prompt.kind) {
        case PromptKind::kAlert:
          return "Closed.";
        case PromptKind::kConfirm:
          return "Yes.";
        case PromptKind::kTextPrompt:
          return "Sent.";
        case PromptKind::kBeforeUnload:
          return "Leaving.";
        case PromptKind::kSavePassword:
          return "Saved.";
        default:
          return "Okay.";
      }
    case PromptAnswer::kDecline:
      switch (prompt.kind) {
        case PromptKind::kConfirm:
          return "No.";
        case PromptKind::kTextPrompt:
          return "Cancelled.";
        case PromptKind::kBeforeUnload:
          return "Staying on the page.";
        case PromptKind::kSavePassword:
          return "Not now.";
        case PromptKind::kPermission:
          return "Dismissed.";
        case PromptKind::kConfirmSites:
          return "Left unconfirmed.";
        default:
          return "Okay.";
      }
    case PromptAnswer::kDictate:
      return "Your answer: " +
             SpeechSafeText(reply.text, kMaxDialogSpokenChars) +
             ". Say yes to send it, or no to cancel.";
    case PromptAnswer::kNeedWord:
      return download ? "To keep it, say " + prompt.word + "."
             : sites  ? "To confirm them, say " + prompt.word + "."
                      : "To allow it, say " + prompt.word + ".";
    case PromptAnswer::kSayAgain:
      return "I didn't catch that. Say the word again.";
    case PromptAnswer::kCantAllow:
      return download && prompt.malicious
                 ? "I can't keep a dangerous file by voice. Use the "
                   "downloads page, or say discard."
             : download ? "I can't keep it by voice right now. Use the "
                          "downloads page, or say discard."
                        : "I can't allow that by voice right now. Use "
                          "the mouse or keyboard, or say no.";
  }
  return std::string();
}

DialogReadLimiter::DialogReadLimiter() = default;
DialogReadLimiter::~DialogReadLimiter() = default;

bool DialogReadLimiter::ShouldRead(int64_t now_ms) {
  while (!read_ms_.empty() &&
         (read_ms_.front() + kDialogFloodWindowMs <= now_ms ||
          read_ms_.front() > now_ms)) {
    read_ms_.pop_front();
  }
  if (read_ms_.size() >= kMaxDialogsRead) {
    return false;
  }
  read_ms_.push_back(now_ms);
  return true;
}

}  // namespace voice
}  // namespace molt_ai
