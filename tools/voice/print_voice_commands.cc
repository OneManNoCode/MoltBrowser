// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Prints the generated part of docs/VOICE_COMMANDS.md (no argument, or
// "en") or docs/VOICE_COMMANDS.es.md ("es") from the grammar's tables
// (src/chrome/browser/molt_ai/voice/voice_command_phrases_<lang>.cc).
// Built and run by tools/voice/voice_commands_doc.py; not part of the
// browser.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"

namespace {

using molt_ai::voice::CommandInfo;
using molt_ai::voice::CommandPattern;
using molt_ai::voice::Language;
using molt_ai::voice::ParseContext;
using molt_ai::voice::ParseVoiceCommand;
using molt_ai::voice::PatternContext;

// "(a|b c)" -> "(a | b c)": easier to read aloud by a screen reader too.
std::string Readable(std::string_view pattern) {
  std::string out;
  for (char c : pattern) {
    if (c == '|') {
      out += " | ";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string_view ContextNote(PatternContext context, Language language) {
  if (language == Language::kSpanish) {
    switch (context) {
      case PatternContext::kAlways:
        return "";
      case PatternContext::kYesNo:
        return " (cuando el navegador te ha hecho una pregunta de sí o no)";
      case PatternContext::kChoice:
        return " (justo después de que el navegador lea una lista, o con los "
               "números a la vista)";
      case PatternContext::kTextField:
        return " (mientras escribes en un campo)";
    }
    return "";
  }
  switch (context) {
    case PatternContext::kAlways:
      return "";
    case PatternContext::kYesNo:
      return " (when the browser has asked a yes-or-no question)";
    case PatternContext::kChoice:
      return " (right after the browser read a list, or with numbers shown)";
    case PatternContext::kTextField:
      return " (while you are typing in a field)";
  }
  return "";
}

}  // namespace

int main(int argc, char** argv) {
  Language language = Language::kEnglish;
  if (argc > 1 && std::strcmp(argv[1], "es") == 0) {
    language = Language::kSpanish;
  } else if (argc > 1 && std::strcmp(argv[1], "en") != 0) {
    std::fprintf(stderr, "usage: print_voice_commands [en|es]\n");
    return 2;
  }
  const std::string_view for_example =
      language == Language::kSpanish ? "Por ejemplo" : "For example";
  const std::vector<CommandInfo>& infos =
      molt_ai::voice::CommandInfos(language);
  const std::vector<CommandPattern>& patterns =
      molt_ai::voice::CommandPatterns(language);
  std::string_view section;
  for (const CommandInfo& info : infos) {
    if (info.section != section) {
      section = info.section;
      std::printf("\n## %.*s\n", static_cast<int>(section.size()),
                  section.data());
    }
    ParseContext context;
    context.language = language;
    context.expecting_yes_no = info.context == PatternContext::kYesNo;
    context.expecting_choice = info.context == PatternContext::kChoice;
    context.text_field_focused = info.context == PatternContext::kTextField;
    // The reference only shows examples that work.
    if (ParseVoiceCommand(info.example, context).kind != info.kind) {
      std::fprintf(stderr, "example \"%.*s\" does not parse to its command\n",
                   static_cast<int>(info.example.size()), info.example.data());
      return 1;
    }
    std::printf("\n**%.*s** %.*s: \"%.*s\".\n\n",
                static_cast<int>(info.summary.size()), info.summary.data(),
                static_cast<int>(for_example.size()), for_example.data(),
                static_cast<int>(info.example.size()), info.example.data());
    for (const CommandPattern& pattern : patterns) {
      if (pattern.kind != info.kind)
        continue;
      const std::string readable = Readable(pattern.pattern);
      const std::string_view note = ContextNote(pattern.context, language);
      std::printf("- `%s`%.*s\n", readable.c_str(),
                  static_cast<int>(note.size()), note.data());
    }
  }
  return 0;
}
