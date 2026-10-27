// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Checks the //base stand-in of the HSTS generator host tool against
// //base's behaviour on what the generator feeds it (compile_check_test.py
// builds and runs it with the host's clang++).

#include <cstdio>
#include <string>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/fixed_flat_set.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"

static int failures = 0;
#define EXPECT(cond)                                         \
  do {                                                       \
    if (!(cond)) {                                           \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #cond); \
      ++failures;                                            \
    }                                                        \
  } while (0)

int main() {
  // JSON with //base's Chromium extensions: comments, \x escapes.
  std::optional<base::DictValue> d = base::JSONReader::ReadDict(
      "// header\n{\n  /* c */ \"entries\": [\n"
      "    { \"name\": \"a\\x41\", \"include_subdomains\": true },\n"
      "    { \"name\": \"b\\u00e9\", \"n\": 2147483648 }  // tail\n"
      "  ]\n}\n",
      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  EXPECT(d.has_value());
  const base::ListValue* l = d ? d->FindList("entries") : nullptr;
  EXPECT(l && l->size() == 2);
  if (l && l->size() == 2) {
    const base::DictValue* e0 = (*l)[0].GetIfDict();
    EXPECT(e0 && *e0->FindString("name") == "aA");
    EXPECT(e0 && e0->FindBool("include_subdomains").value_or(false));
    const base::DictValue* e1 = (*l)[1].GetIfDict();
    EXPECT(e1 && *e1->FindString("name") == "b\xc3\xa9");
    EXPECT(e1 && e1->Find("n") && e1->Find("n")->is_double());
    int keys = 0;
    for (auto kv : *e1) {
      keys += kv.first == "name" || kv.first == "n";
    }
    EXPECT(keys == 2);
  }
  // Without the comment option, or with a trailing comma, it is refused.
  EXPECT(!base::JSONReader::ReadDict("// x\n{}", base::JSON_PARSE_RFC));
  EXPECT(!base::JSONReader::ReadDict("{\"a\": [1,],}",
                                     base::JSON_PARSE_CHROMIUM_EXTENSIONS));
  EXPECT(!base::JSONReader::ReadDict("[1]",
                                     base::JSON_PARSE_CHROMIUM_EXTENSIONS));
  // \v needs its own option; a raw newline in a string is an extension.
  EXPECT(!base::JSONReader::ReadDict("{\"a\": \"\\v\"}",
                                     base::JSON_PARSE_CHROMIUM_EXTENSIONS));
  EXPECT(base::JSONReader::ReadDict("{\"a\": \"x\ny\"}",
                                    base::JSON_PARSE_CHROMIUM_EXTENSIONS));
  EXPECT(!base::JSONReader::ReadDict("{\"a\": \"x\ny\"}",
                                     base::JSON_PARSE_RFC));

  // Strict base64.
  std::string out;
  EXPECT(base::Base64Decode("aGVsbG8=", &out) && out == "hello");
  EXPECT(!base::Base64Decode("aGVsbG8", &out));
  EXPECT(!base::Base64Decode("aGVs bG8=", &out));
  EXPECT(base::Base64Decode("")->empty());

  // CollapseWhitespaceASCII as the PEM reader uses it.
  EXPECT(base::CollapseWhitespaceASCII("\n  abc\n def  g \n", true) ==
         "abcdef g");
  EXPECT(base::CollapseWhitespaceASCII("  a   b  ", false) == "a b");

  // Splitting and the rest of the string helpers.
  auto parts = base::SplitStringPiece(" x , y,,z ", ",",
                                      base::TRIM_WHITESPACE,
                                      base::SPLIT_WANT_ALL);
  EXPECT(parts.size() == 4 && parts[0] == "x" && parts[2].empty());
  EXPECT(base::SplitStringPiece("", "\n", base::KEEP_WHITESPACE,
                                base::SPLIT_WANT_ALL)
             .empty());
  EXPECT(base::StartsWith("SHA256/xyz", "sha256/",
                          base::CompareCase::INSENSITIVE_ASCII));
  EXPECT(*base::RemovePrefix("sha256/q", "SHA256/",
                             base::CompareCase::INSENSITIVE_ASCII) == "q");
  uint64_t v = 0;
  EXPECT(base::StringToUint64("1790948490", &v) && v == 1790948490u);
  EXPECT(!base::StringToUint64(" 1", &v) && !base::StringToUint64("-1", &v));
  std::string trimmed;
  base::TrimString("\n\nab\n", "\n", &trimmed);
  EXPECT(trimmed == "ab");
  EXPECT(base::StringPrintf("0x%02x,", 0xa) == "0x0a,");

  static constexpr auto kSet =
      base::MakeFixedFlatSet<std::string_view>({"b", "a", "c"});
  EXPECT(kSet.contains("a") && !kSet.contains("d"));

  const char* argv[] = {"tool", "--hsts", "in.json", "--v=1", "out.h"};
  base::CommandLine::Init(5, argv);
  const base::CommandLine& cl = *base::CommandLine::ForCurrentProcess();
  EXPECT(cl.HasSwitch("hsts") && cl.GetSwitchValueASCII("v") == "1");
  EXPECT(cl.GetArgs().size() == 2 && cl.GetArgs()[1] == "out.h");

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
