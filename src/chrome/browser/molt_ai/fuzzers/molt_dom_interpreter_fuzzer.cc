// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// DOMInterpreter (dom/dom_interpreter.h): a web page's HTML (its outerHTML,
// capped) to the structured summary the AI features read. It replaced
// std::regex with linear scanners; a quadratic case is a hang on a hostile
// page, so run this with -timeout.
//
// The whole input is the HTML. Checks: no memory or UB error or timeout;
// the main content has no tag left (no '<' with a '>' after it), no
// whitespace runs and no surrounding whitespace, and is cut to the token
// limit; extracted links, buttons and forms are the ones ParseHTML()
// reports; the serializations work on all of it.

#include <cstddef>
#include <cstdint>
#include <string>

#include "base/check.h"
#include "chrome/browser/molt_ai/dom/dom_interpreter.h"

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string html = molt_ai::DOMInterpreter::CapHtmlForParsing(
      std::string(reinterpret_cast<const char*>(data), size));
  CHECK(html.size() <= molt_ai::DOMInterpreter::kMaxParseHtmlBytes);

  molt_ai::DOMInterpreter interpreter;
  const molt_ai::StructuredPage page =
      interpreter.ParseHTML(html, "https://example.com/");

  const std::string& content = page.main_content;
  static constexpr char kTruncated[] = "\n[Content truncated...]";
  std::string body = content;
  if (page.was_truncated) {
    CHECK(body.size() >= sizeof(kTruncated) - 1 &&
          body.compare(body.size() - (sizeof(kTruncated) - 1),
                       std::string::npos, kTruncated) == 0);
    body.erase(body.size() - (sizeof(kTruncated) - 1));
    CHECK(body.size() == 4000u * 4);
    CHECK(page.estimated_tokens == 4000);
  } else {
    CHECK(content.size() <= 4000u * 4 + 3);
    // Tags stripped: a '<' may remain only if no '>' follows it.
    const size_t lt = content.find('<');
    if (lt != std::string::npos) {
      CHECK(content.find('>', lt) == std::string::npos);
    }
    // Whitespace collapsed and trimmed.
    if (!content.empty()) {
      CHECK(!IsSpace(content.front()) && !IsSpace(content.back()));
    }
    for (size_t i = 1; i < content.size(); ++i) {
      CHECK(!(IsSpace(content[i]) && IsSpace(content[i - 1])));
    }
  }

  // The per-kind extractors agree with ParseHTML().
  CHECK(interpreter.ExtractLinks(html).size() == page.links.size());
  CHECK(interpreter.ExtractButtons(html).size() == page.buttons.size());
  CHECK(interpreter.ExtractForms(html).size() == page.forms.size());
  CHECK(interpreter.ExtractMainContent(html).size() >= body.size());
  for (const molt_ai::LinkInfo& link : page.links) {
    if (link.is_navigation) CHECK(!link.href.empty());
  }

  const std::string json = page.ToJSON();
  const std::string text = page.ToCompactText();
  CHECK(!json.empty() && !text.empty());
  const std::string truncated =
      molt_ai::DOMInterpreter::TruncateToTokenLimit(html, 16);
  CHECK(truncated.size() <= 64 + sizeof(kTruncated));
  return 0;
}
