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
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"

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
constexpr auto kDictationLeads = std::to_array<std::string_view>(
    {"answer ", "type ", "my answer is ", "the answer is ", "reply "});
constexpr auto kPoliteBefore =
    std::to_array<std::string_view>({"please ", "um ", "uh "});
constexpr auto kPoliteAfter =
    std::to_array<std::string_view>({" please", " thanks", " thank you"});

// Spanish's (folded: no accents, "si" for "sí"), as a Spanish speaker
// answers: the familiar imperative ("bloquea", "quédate"), the infinitive
// ("bloquear") and the imperative with its object ("bloquéalo"). None is
// heard as a confirmation word (each word alone is in SpanishAnswerWords(),
// or comes after "no" or "de"): unit-tested.
constexpr auto kYesEs = std::to_array<std::string_view>(
    {"si",         "vale",         "ok",       "okay",      "claro",
     "de acuerdo", "si por favor", "confirma", "confirmar", "acepta",
     "aceptar",    "esta bien",    "bien",     "si vale",   "vale si",
     "dale",       "venga",        "seguro",   "sip",       "si claro"});
constexpr auto kNoEs = std::to_array<std::string_view>(
    {"no", "nop", "cancela", "cancelar", "cancelalo", "no gracias",
     "no lo hagas", "no no"});
constexpr auto kBlockEs = std::to_array<std::string_view>(
    {"no", "bloquea", "bloquear", "bloquealo", "bloquearlo", "deniega",
     "denegar", "no lo permitas", "no permitir", "no permitas", "no gracias",
     "nop", "nunca"});
constexpr auto kDismissEs = std::to_array<std::string_view>(
    {"cancela", "cancelar", "ahora no", "luego", "mas tarde", "ignora",
     "ignorar", "ignoralo", "cierra", "cerrar"});
constexpr auto kAllowWordsEs = std::to_array<std::string_view>(
    {"si",         "permite",      "permitir",   "permitelo", "permitirlo",
     "ok",         "okay",         "vale",       "claro",     "si por favor",
     "si permite", "si permitelo", "acepta",     "aceptar",   "guarda",
     "guardar",    "conserva",     "conservalo", "conservar", "si conservalo"});
constexpr auto kCloseEs = std::to_array<std::string_view>(
    {"vale", "ok", "okay", "si", "cierra", "cerrar", "entendido", "de acuerdo",
     "acepta", "aceptar", "no", "cancela", "cancelar"});
constexpr auto kLeaveEs = std::to_array<std::string_view>(
    {"sal", "salir", "sal de la pagina", "salir de la pagina", "si", "si sal",
     "recarga", "recargar", "recargala", "si recarga", "vale", "ok", "okay"});
constexpr auto kStayEs = std::to_array<std::string_view>(
    {"quedate", "quedarme", "quedate aqui", "me quedo", "quedarse",
     "quedate en la pagina", "no", "cancela", "cancelar", "no salgas",
     "no recargues", "no me quedo aqui"});
constexpr auto kDiscardEs = std::to_array<std::string_view>(
    {"descarta", "descartar", "descartalo", "borra", "borrar", "borralo",
     "elimina", "eliminar", "eliminalo", "no", "no lo conserves",
     "no lo guardes", "cancela", "cancelar"});
constexpr auto kSaveEs = std::to_array<std::string_view>(
    {"guarda", "guardar", "si", "guarda la contrasena", "guardar contrasena",
     "si guarda", "vale", "ok", "okay"});
constexpr auto kNeverEs = std::to_array<std::string_view>(
    {"nunca", "nunca guardar", "no guardar nunca", "nunca la guardes",
     "nunca para este sitio"});
constexpr auto kNotNowEs = std::to_array<std::string_view>(
    {"ahora no", "no", "no gracias", "cancela", "cancelar", "luego",
     "mas tarde", "ahora no gracias"});
constexpr auto kDictationLeadsEs = std::to_array<std::string_view>(
    {"responde ", "respuesta ", "escribe ", "contesta ", "mi respuesta es ",
     "la respuesta es "});
constexpr auto kPoliteBeforeEs =
    std::to_array<std::string_view>({"por favor ", "eh ", "em ", "pues "});
constexpr auto kPoliteAfterEs = std::to_array<std::string_view>(
    {" por favor", " gracias", " porfa", " muchas gracias"});

// The answers a language takes to the prompts.
struct Answers {
  Phrases yes;
  Phrases no;
  Phrases block;
  Phrases dismiss;
  Phrases allow;
  Phrases close;
  Phrases leave;
  Phrases stay;
  Phrases discard;
  Phrases save;
  Phrases never;
  Phrases not_now;
  // What a dictated answer starts with ("answer ").
  Phrases dictation;
  Phrases polite_before;
  Phrases polite_after;
};

const Answers& AnswersFor(Language language) {
  static constexpr Answers kEnglish{
      kYes,   kNo,     kBlock,          kDismiss,      kAllowWords,
      kClose, kLeave,  kStay,           kDiscard,      kSave,
      kNever, kNotNow, kDictationLeads, kPoliteBefore, kPoliteAfter};
  static constexpr Answers kSpanish{
      kYesEs,   kNoEs,     kBlockEs,          kDismissEs,      kAllowWordsEs,
      kCloseEs, kLeaveEs,  kStayEs,           kDiscardEs,      kSaveEs,
      kNeverEs, kNotNowEs, kDictationLeadsEs, kPoliteBeforeEs, kPoliteAfterEs};
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish:
      return kSpanish;
  }
  return kEnglish;
}

}  // namespace

std::vector<std::string_view> PromptAnswerPhrases(Language language) {
  const Answers& a = AnswersFor(language);
  std::vector<std::string_view> all;
  for (Phrases phrases :
       {a.yes, a.no, a.block, a.dismiss, a.allow, a.close, a.leave, a.stay,
        a.discard, a.save, a.never, a.not_now}) {
    all.insert(all.end(), phrases.begin(), phrases.end());
  }
  return all;
}

namespace {

bool In(Phrases phrases, std::string_view phrase) {
  return std::find(phrases.begin(), phrases.end(), phrase) != phrases.end();
}

// |heard| folded, without "please" at either end.
std::string Folded(std::string_view heard, const Answers& answers) {
  std::string folded = FoldForComparison(
      heard.substr(0, std::min(heard.size(), kMaxAnswerScanBytes)));
  for (std::string_view polite : answers.polite_before) {
    while (folded.rfind(polite, 0) == 0) {
      folded.erase(0, polite.size());
    }
  }
  for (std::string_view polite : answers.polite_after) {
    if (folded.size() > polite.size() &&
        folded.compare(folded.size() - polite.size(), polite.size(), polite) ==
            0) {
      folded.resize(folded.size() - polite.size());
    }
  }
  return folded;
}

// Lower-case letters and single spaces of the browser's own words. In
// Spanish, its letters with accents too ("usar tu cámara"): any character
// beyond ASCII of the text made speech-safe first.
std::string Words(std::string_view text, size_t max, Language language) {
  const bool beyond_ascii = language != Language::kEnglish;
  const std::string safe =
      beyond_ascii ? SpeechSafeText(text, max) : std::string();
  if (beyond_ascii) {
    text = safe;
  }
  std::string out;
  for (char c : text) {
    if (out.size() >= max) {
      break;
    }
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if ((c >= 'a' && c <= 'z') ||
        (beyond_ascii && (static_cast<unsigned char>(c) & 0x80))) {
      out.push_back(c);
    } else if (!out.empty() && out.back() != ' ') {
      out.push_back(' ');
    }
  }
  if (beyond_ascii) {
    // Never part of a character: a cut one is dropped whole.
    size_t lead = out.size();
    while (lead > 0 &&
           (static_cast<unsigned char>(out[lead - 1]) & 0xC0) == 0x80) {
      --lead;
    }
    if (lead > 0 && (static_cast<unsigned char>(out[lead - 1]) & 0x80)) {
      const unsigned char first = static_cast<unsigned char>(out[lead - 1]);
      const size_t length = first >= 0xF0   ? 4
                            : first >= 0xE0 ? 3
                            : first >= 0xC0 ? 2
                                            : 1;
      if (out.size() - (lead - 1) < length) {
        out.resize(lead - 1);
      }
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// The site's name; "This page" without one (lower case where Spanish says
// it inside a sentence: "¿Salir de esta página?").
std::string Site(const BrowserPrompt& prompt, bool inside = false) {
  const std::string site = activation::SiteForSpeech(prompt.site);
  if (!site.empty()) {
    return site;
  }
  if (inside && prompt.language == Language::kSpanish) {
    return "esta página";
  }
  return Localized(prompt.language, "This page");
}

// The dictated answer: what follows "answer" or "type" in |heard|, as said.
std::optional<std::string> Dictated(std::string_view heard,
                                    const std::string& folded,
                                    const Answers& answers) {
  size_t words = 0;
  for (std::string_view lead : answers.dictation) {
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

// The gesture's word, another one, one not for sure, or none: heard in the
// prompt's language.
std::optional<PromptAnswer> WordAnswer(const PendingPrompt& prompt,
                                       std::string_view heard) {
  const HeardWord word = HearConfirmationWord(heard, prompt.language);
  if (word.kind == HeardWord::Kind::kNone) {
    return std::nullopt;
  }
  if (prompt.word.empty()) {
    return PromptAnswer::kCantAllow;
  }
  if (word.kind == HeardWord::Kind::kUnsure) {
    return PromptAnswer::kSayAgain;
  }
  return ConfirmationWords(prompt.language)[word.index] == prompt.word
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

std::string_view WordOnScreen(Language language) {
  return Translated(language, kWordOnScreen);
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

void NoteWordText(PendingPrompt& prompt, WordText text) {
  if (prompt.word.empty() || text.said.empty() || text.said == text.without) {
    return;
  }
  std::erase(prompt.word_texts, text);
  prompt.word_texts.push_back(std::move(text));
  if (prompt.word_texts.size() > kMaxWordTexts) {
    prompt.word_texts.erase(prompt.word_texts.begin());
  }
}

const std::string* WordTextTwin(const PendingPrompt& prompt,
                                std::string_view said) {
  if (prompt.word.empty()) {
    return nullptr;
  }
  for (const WordText& text : prompt.word_texts) {
    if (text.said == said) {
      return &text.without;
    }
  }
  return nullptr;
}

namespace {

std::string PromptSpeechOnly(const BrowserPrompt& prompt,
                             std::string_view word);

}  // namespace

std::string PromptSpeech(const BrowserPrompt& prompt, std::string_view word) {
  std::string out = PromptSpeechOnly(prompt, word);
  if (out.empty()) {
    return out;
  }
  if (!word.empty() && prompt.explain_shown_only && NeedsGesture(prompt.kind)) {
    // Why the word was shown, not said: once per window.
    out += " " + Said(prompt.language,
                      "I don't say the word aloud while another program "
                      "could hear it.",
                      {});
  }
  const std::string preface = SpeechSafeText(prompt.preface, 200);
  return preface.empty() ? out : preface + " " + out;
}

namespace {

std::string PromptSpeechOnly(const BrowserPrompt& prompt,
                             std::string_view word) {
  const Language language = prompt.language;
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
      const std::string what = Words(prompt.what, kMaxWhatChars, language);
      std::string out =
          what.empty()
              ? Said(language, "{0} is asking for a permission.", {site})
              : Said(language, "{0} wants to {1}.", {site, what});
      if (!word.empty()) {
        out += " " + Said(language,
                          "To allow it, say {0}. To block it, say no.", {word});
      } else if (prompt.screen_shared) {
        out += " " + Said(language,
                          "I can't take the word while your screen is "
                          "shared, so to allow it, use the mouse or "
                          "keyboard. To block it, say no.",
                          {});
      } else {
        out += " " + Said(language,
                          "To block it, say no. To allow it, use the mouse "
                          "or keyboard.",
                          {});
      }
      return out;
    }
    case PromptKind::kAlert:
      if (prompt.flood) {
        return Said(language, "{0} shows another message. Say OK to close it.",
                    {site});
      }
      return says.empty()
                 ? Said(language, "{0} shows a message. Say OK to close it.",
                        {site})
                 : Said(language, "{0} says: {1} Say OK to close it.",
                        {site, says});
    case PromptKind::kConfirm:
      if (prompt.flood) {
        return Said(language, "{0} asks another question. Say yes or no.",
                    {site});
      }
      return says.empty()
                 ? Said(language, "{0} asks you to confirm. Say yes or no.",
                        {site})
                 : Said(language, "{0} asks: {1} Say yes or no.", {site, says});
    case PromptKind::kTextPrompt:
      return prompt.flood || says.empty()
                 ? Said(language,
                        "{0} asks for an answer. Say answer, then your "
                        "answer, or say no to cancel.",
                        {site})
                 : Said(language,
                        "{0} asks: {1} Say answer, then your answer, or say "
                        "no to cancel.",
                        {site, says});
    case PromptKind::kBeforeUnload: {
      const std::string named = Site(prompt, /*inside=*/true);
      return prompt.reload
                 ? Said(language,
                        "Reload {0}? Changes you made may not be saved. Say "
                        "reload or stay.",
                        {named})
                 : Said(language,
                        "Leave {0}? Changes you made may not be saved. Say "
                        "leave or stay.",
                        {named});
    }
    case PromptKind::kDangerousDownload: {
      const std::string from = Site(prompt, /*inside=*/true);
      const std::string name =
          SpeechSafeText(prompt.text, kMaxFileNameSpokenChars);
      if (prompt.malicious) {
        return name.empty()
                   ? Said(language,
                          "A file from {0} is dangerous, so it was blocked. "
                          "To delete it, say discard. To keep it anyway, use "
                          "the downloads page.",
                          {from})
                   : Said(language,
                          "The file {0} from {1} is dangerous, so it was "
                          "blocked. To delete it, say discard. To keep it "
                          "anyway, use the downloads page.",
                          {name, from});
      }
      std::string out =
          name.empty()
              ? Said(language, "A file from {0} may be harmful.", {from})
              : Said(language, "The file {0} from {1} may be harmful.",
                     {name, from});
      if (!word.empty()) {
        out += " " + Said(language,
                          "To keep it, say {0}. To delete it, say discard.",
                          {word});
      } else if (prompt.screen_shared) {
        out += " " + Said(language,
                          "I can't take the word while your screen is "
                          "shared. To delete it, say discard. To keep it, "
                          "use the downloads page.",
                          {});
      } else {
        out += " " + Said(language,
                          "To delete it, say discard. To keep it, use the "
                          "downloads page.",
                          {});
      }
      return out;
    }
    case PromptKind::kSavePassword:
      return Said(language,
                  "Save your password for {0}? Say save, never, or not now.",
                  {Site(prompt, /*inside=*/true)});
    case PromptKind::kConfirmSites: {
      // The sites, each already a cleaned host ("a.example and b.example");
      // cleaned again here.
      const std::string sites =
          SpeechSafeText(prompt.text, kMaxDialogSpokenChars);
      std::string out =
          sites.empty()
              ? Said(language, "Confirm the sites this routine uses?", {})
              : Said(language, "Confirm the sites this routine uses: {0}?",
                     {sites});
      if (!word.empty()) {
        out += " " + Said(language,
                          "To confirm them, say {0}. To leave them "
                          "unconfirmed, say no.",
                          {word});
      } else if (prompt.screen_shared) {
        out += " " + Said(language,
                          "I can't take the word while your screen is "
                          "shared. Confirm them in the Agent studio, or say "
                          "no.",
                          {});
      } else {
        out += " " + Said(language,
                          "Confirm them in the Agent studio, or say no.", {});
      }
      return out;
    }
  }
  return std::string();
}

}  // namespace

std::string PromptTimeoutSpeech(PromptKind kind) {
  return PromptTimeoutSpeech(kind, Language::kEnglish);
}

std::string PromptTimeoutSpeech(PromptKind kind, Language language) {
  switch (kind) {
    case PromptKind::kPermission:
      return Said(language, "No answer, so I dismissed the permission request.",
                  {});
    case PromptKind::kDangerousDownload:
      return Said(language,
                  "No answer, so the file stays blocked. You can decide on "
                  "the downloads page.",
                  {});
    case PromptKind::kConfirmSites:
      return Said(language, "No answer, so the sites stay unconfirmed.", {});
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
  return NotCleanSpeech(word, Language::kEnglish);
}

std::string NotCleanSpeech(std::string_view word, Language language) {
  return Said(language,
              "I couldn't be sure that was you. When it's quiet and I've "
              "finished speaking, say {0}.",
              {word});
}

std::string WordRetiredPreface(WordRetired why) {
  return WordRetiredPreface(why, Language::kEnglish);
}

std::string WordRetiredPreface(WordRetired why, Language language) {
  switch (why) {
    case WordRetired::kNotClean:
      return Said(language,
                  "I couldn't be sure that was you, and something else may "
                  "have heard the word, so I changed it.",
                  {});
    case WordRetired::kSaidAloud:
      return Said(language,
                  "Something else may have heard you say the word, so I "
                  "changed it.",
                  {});
    case WordRetired::kAudioCaptured:
      return Said(language,
                  "Something started using a microphone, so I changed the "
                  "word.",
                  {});
    case WordRetired::kScreenShared:
      return std::string();
    case WordRetired::kScreenFreed:
      return Said(language, "Your screen is no longer shared.", {});
  }
  return std::string();
}

bool WordSaidInPassing(const PendingPrompt& prompt, std::string_view heard) {
  if (prompt.word.empty()) {
    return false;
  }
  switch (HearConfirmationWord(heard, prompt.language).kind) {
    case HeardWord::Kind::kWord:
      return false;
    case HeardWord::Kind::kUnsure:
      return true;
    case HeardWord::Kind::kNone:
      return HoldsConfirmationWord(heard, prompt.word, prompt.language);
  }
  return false;
}

std::string RetiredWordSpeech(PromptKind kind, std::string_view word) {
  return RetiredWordSpeech(kind, word, Language::kEnglish);
}

std::string RetiredWordSpeech(PromptKind kind,
                              std::string_view word,
                              Language language) {
  switch (kind) {
    case PromptKind::kDangerousDownload:
      return Said(language, "That word changed. To keep it, say {0}.", {word});
    case PromptKind::kConfirmSites:
      return Said(language, "That word changed. To confirm them, say {0}.",
                  {word});
    default:
      return Said(language, "That word changed. To allow it, say {0}.", {word});
  }
}

PromptReply ParsePromptReply(const PendingPrompt& prompt,
                             std::string_view heard) {
  const Answers& answers = AnswersFor(prompt.language);
  const std::string folded = Folded(heard, answers);
  if (folded.empty()) {
    return PromptReply();
  }
  switch (prompt.kind) {
    case PromptKind::kPermission:
      if (In(answers.block, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      if (In(answers.dismiss, folded)) {
        return Reply(PromptAnswer::kDecline);
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(answers.allow, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kAlert:
      return In(answers.close, folded) ? Reply(PromptAnswer::kAccept)
                                       : PromptReply();
    case PromptKind::kConfirm:
      if (In(answers.yes, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(answers.no, folded) ? Reply(PromptAnswer::kDecline)
                                    : PromptReply();
    case PromptKind::kTextPrompt: {
      if (std::optional<std::string> text = Dictated(heard, folded, answers)) {
        PromptReply reply = Reply(PromptAnswer::kDictate);
        reply.text = std::move(*text);
        return reply;
      }
      if (In(answers.yes, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(answers.no, folded) ? Reply(PromptAnswer::kDecline)
                                    : PromptReply();
    }
    case PromptKind::kBeforeUnload:
      if (In(answers.leave, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      return In(answers.stay, folded) ? Reply(PromptAnswer::kDecline)
                                      : PromptReply();
    case PromptKind::kDangerousDownload:
      if (In(answers.discard, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      if (prompt.malicious) {
        // Never kept by voice: no word was offered.
        if (HearConfirmationWord(heard, prompt.language).kind !=
                HeardWord::Kind::kNone ||
            In(answers.allow, folded)) {
          return Reply(PromptAnswer::kCantAllow);
        }
        return PromptReply();
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(answers.allow, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kConfirmSites:
      if (In(answers.no, folded) || In(answers.dismiss, folded)) {
        return Reply(PromptAnswer::kDecline);
      }
      if (std::optional<PromptAnswer> word = WordAnswer(prompt, heard)) {
        return Reply(*word);
      }
      if (In(answers.yes, folded) || In(answers.allow, folded)) {
        return Reply(prompt.word.empty() ? PromptAnswer::kCantAllow
                                         : PromptAnswer::kNeedWord);
      }
      return PromptReply();
    case PromptKind::kSavePassword:
      if (In(answers.save, folded)) {
        return Reply(PromptAnswer::kAccept);
      }
      if (In(answers.never, folded)) {
        return Reply(PromptAnswer::kRefuse);
      }
      return In(answers.not_now, folded) ? Reply(PromptAnswer::kDecline)
                                         : PromptReply();
  }
  return PromptReply();
}

std::string ReplySpeech(const PendingPrompt& prompt, const PromptReply& reply) {
  const Language language = prompt.language;
  const bool download = prompt.kind == PromptKind::kDangerousDownload;
  const bool sites = prompt.kind == PromptKind::kConfirmSites;
  switch (reply.answer) {
    case PromptAnswer::kNone:
      return std::string();
    case PromptAnswer::kAllow:
      return download ? Said(language, "Kept the file.", {})
             : sites  ? Said(language, "Confirmed.", {})
                      : Said(language, "Allowed.", {});
    case PromptAnswer::kWrongWord:
      return download ? Said(language,
                             "That wasn't the word, so I didn't keep the "
                             "file.",
                             {})
                      : Said(language,
                             "That wasn't the word, so I turned it down.", {});
    case PromptAnswer::kRefuse:
      switch (prompt.kind) {
        case PromptKind::kDangerousDownload:
          return Said(language, "Deleted the file.", {});
        case PromptKind::kSavePassword:
          return Said(language,
                      "I won't offer to save passwords for this site.", {});
        default:
          return Said(language, "Blocked.", {});
      }
    case PromptAnswer::kAccept:
      switch (prompt.kind) {
        case PromptKind::kAlert:
          return Said(language, "Closed.", {});
        case PromptKind::kConfirm:
          return Said(language, "Yes.", {});
        case PromptKind::kTextPrompt:
          return Said(language, "Sent.", {});
        case PromptKind::kBeforeUnload:
          return Said(language, "Leaving.", {});
        case PromptKind::kSavePassword:
          return Said(language, "Saved.", {});
        default:
          return Said(language, "Okay.", {});
      }
    case PromptAnswer::kDecline:
      switch (prompt.kind) {
        case PromptKind::kConfirm:
          return Said(language, "No.", {});
        case PromptKind::kTextPrompt:
          return Said(language, "Cancelled.", {});
        case PromptKind::kBeforeUnload:
          return Said(language, "Staying on the page.", {});
        case PromptKind::kSavePassword:
          return Said(language, "Not now.", {});
        case PromptKind::kPermission:
          return Said(language, "Dismissed.", {});
        case PromptKind::kConfirmSites:
          return Said(language, "Left unconfirmed.", {});
        default:
          return Said(language, "Okay.", {});
      }
    case PromptAnswer::kDictate:
      return Said(language,
                  "Your answer: {0}. Say yes to send it, or no to cancel.",
                  {SpeechSafeText(reply.text, kMaxDialogSpokenChars)});
    case PromptAnswer::kNeedWord:
      return download ? Said(language, "To keep it, say {0}.", {prompt.word})
             : sites
                 ? Said(language, "To confirm them, say {0}.", {prompt.word})
                 : Said(language, "To allow it, say {0}.", {prompt.word});
    case PromptAnswer::kSayAgain:
      return Said(language, "I didn't catch that. Say the word again.", {});
    case PromptAnswer::kCantAllow:
      if (download && prompt.malicious) {
        return Said(language,
                    "I can't keep a dangerous file by voice. Use the "
                    "downloads page, or say discard.",
                    {});
      }
      if (prompt.screen_shared) {
        return download ? Said(language,
                               "I can't take the word while your screen is "
                               "shared. Use the downloads page, or say "
                               "discard.",
                               {})
               : sites  ? Said(language,
                               "I can't take the word while your screen is "
                                "shared. Confirm them in the Agent studio, or "
                                "say no.",
                               {})
                        : Said(language,
                               "I can't take the word while your screen is "
                                "shared. Use the mouse or keyboard, or say "
                                "no.",
                               {});
      }
      return download ? Said(language,
                             "I can't keep it by voice right now. Use the "
                             "downloads page, or say discard.",
                             {})
                      : Said(language,
                             "I can't allow that by voice right now. Use the "
                             "mouse or keyboard, or say no.",
                             {});
  }
  return std::string();
}

std::string ReplySpeechWithoutWord(const PendingPrompt& prompt,
                                   const PromptReply& reply) {
  PendingPrompt twin;
  twin.kind = prompt.kind;
  twin.language = prompt.language;
  twin.malicious = prompt.malicious;
  twin.screen_shared = prompt.screen_shared;
  twin.word = prompt.word.empty() ? std::string()
                                  : std::string(WordOnScreen(prompt.language));
  return ReplySpeech(twin, reply);
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
