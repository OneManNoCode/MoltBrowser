// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Prints the generated part of docs/VOICE_COMMANDS.md from the grammar's
// tables (src/chrome/browser/molt_ai/voice/voice_command_phrases_en.cc).
// Built and run by tools/voice/voice_commands_doc.py; not part of the
// browser.

#include <cstdio>
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

std::string_view ContextNote(PatternContext context) {
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

int main() {
  const std::vector<CommandInfo>& infos =
      molt_ai::voice::CommandInfos(Language::kEnglish);
  const std::vector<CommandPattern>& patterns =
      molt_ai::voice::CommandPatterns(Language::kEnglish);
  std::string_view section;
  for (const CommandInfo& info : infos) {
    if (info.section != section) {
      section = info.section;
      std::printf("\n## %.*s\n", static_cast<int>(section.size()),
                  section.data());
    }
    ParseContext context;
    context.expecting_yes_no = info.context == PatternContext::kYesNo;
    context.expecting_choice = info.context == PatternContext::kChoice;
    context.text_field_focused = info.context == PatternContext::kTextField;
    // The reference only shows examples that work.
    if (ParseVoiceCommand(info.example, context).kind != info.kind) {
      std::fprintf(stderr, "example \"%.*s\" does not parse to its command\n",
                   static_cast<int>(info.example.size()), info.example.data());
      return 1;
    }
    std::printf("\n**%.*s** For example: \"%.*s\".\n\n",
                static_cast<int>(info.summary.size()), info.summary.data(),
                static_cast<int>(info.example.size()), info.example.data());
    for (const CommandPattern& pattern : patterns) {
      if (pattern.kind != info.kind)
        continue;
      const std::string readable = Readable(pattern.pattern);
      const std::string_view note = ContextNote(pattern.context);
      std::printf("- `%s`%.*s\n", readable.c_str(),
                  static_cast<int>(note.size()), note.data());
    }
  }
  return 0;
}
