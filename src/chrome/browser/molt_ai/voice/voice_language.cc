// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_language.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

std::optional<Language> StoredVoiceLanguage(std::string_view stored) {
  if (stored == "en") {
    return Language::kEnglish;
  }
  if (stored == "es") {
    return Language::kSpanish;
  }
  return std::nullopt;
}

Language VoiceLanguageFor(std::string_view stored, std::string_view ui_locale) {
  if (const std::optional<Language> chosen = StoredVoiceLanguage(stored)) {
    return *chosen;
  }
  return LanguageFromTag(ui_locale).value_or(Language::kEnglish);
}

bool ListeningAvailable(Language language) {
  switch (language) {
    case Language::kEnglish:
      return true;
    case Language::kSpanish:
      return false;
  }
  return false;
}

std::string_view ListeningUnavailableNotice(Language language) {
  switch (language) {
    case Language::kEnglish:
      return "";
    case Language::kSpanish:
      return "Todavía no puedo escuchar en español. Puedes escribir las "
             "órdenes en español en el panel del modo Agente, o elegir el "
             "inglés para el modo de voz en la configuración.";
  }
  return "";
}

std::string_view LanguageDisplayName(Language language) {
  switch (language) {
    case Language::kEnglish:
      return "English";
    case Language::kSpanish:
      return "Español";
  }
  return "English";
}

std::string PlatformLanguageTag(Language language, std::string_view ui_locale) {
  switch (language) {
    case Language::kEnglish:
      return "en-US";
    case Language::kSpanish:
      if (LanguageFromTag(ui_locale) == Language::kSpanish &&
          ui_locale.size() <= 16) {
        std::string tag(ui_locale);
        for (char& c : tag) {
          if (c == '_') {
            c = '-';
          }
        }
        return tag;
      }
      return "es";
  }
  return "en-US";
}

namespace {

// |tag| lower-cased, with '_' as '-'.
std::string NormalTag(std::string_view tag) {
  std::string out;
  for (char c : tag.substr(0, 32)) {
    if (c == '_') {
      c = '-';
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::optional<size_t> PickPlatformVoice(
    const std::vector<PlatformVoice>& voices,
    Language language,
    std::string_view tag) {
  const std::string wanted = NormalTag(tag);
  std::optional<size_t> any;
  for (size_t i = 0; i < voices.size(); ++i) {
    const PlatformVoice& voice = voices[i];
    if (voice.remote || LanguageFromTag(voice.lang) != language) {
      continue;
    }
    if (NormalTag(voice.lang) == wanted) {
      return i;
    }
    if (!any) {
      any = i;
    }
  }
  return any;
}

}  // namespace voice
}  // namespace molt_ai
