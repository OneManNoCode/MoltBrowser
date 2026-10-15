// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/dom/dom_interpreter.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace molt_ai {

// The extraction below used to be a set of std::regex patterns run over the
// page's outerHTML. libc++'s backtracking matcher is quadratic on inputs
// with many unclosed tags (64 KiB took 16 s) and throws regex_error
// (error_complexity) on others, e.g. `<a href="` followed by ~16K quote
// characters; under Chromium's -fno-exceptions that throw aborts the
// browser. Each pattern is now matched by a linear-time scanner that
// returns exactly what the regex returned (S5); the patterns are quoted
// above each scanner. The regex semantics reproduced are libc++'s
// ECMAScript grammar in the default "C" locale:
//   .     any byte except \n and \r
//   \s    one of space \t \n \v \f \r
//   icase ASCII letters only
// Matching is leftmost-first: the earliest start wins, and from a start,
// greedy repeats prefer the longest and lazy ones the shortest run that
// lets the rest of the pattern match. Successive matches (sregex_iterator,
// regex_replace) resume at the end of the previous match.

namespace {

constexpr size_t kNpos = std::string_view::npos;

bool IsLineBreak(char c) {
  return c == '\n' || c == '\r';
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

bool IsQuote(char c) {
  return c == '"' || c == '\'';
}

char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Whether |lit| (lowercase) occurs at |pos|, ASCII case-insensitively.
bool MatchesAt(std::string_view s, size_t pos, std::string_view lit) {
  if (pos > s.size() || s.size() - pos < lit.size()) {
    return false;
  }
  for (size_t i = 0; i < lit.size(); ++i) {
    if (LowerAscii(s[pos + i]) != lit[i]) {
      return false;
    }
  }
  return true;
}

// Finds the first position >= |from| where a literal (case-insensitive) or
// a character class matches. Remembers its last answer, so calls with
// non-decreasing |from| scan the text once in total; any other call order
// is still answered correctly.
class Finder {
 public:
  Finder(std::string_view s, std::string_view literal)
      : s_(s), literal_(literal) {}
  Finder(std::string_view s, bool (*pred)(char)) : s_(s), pred_(pred) {}

  size_t Next(size_t from) {
    if (cached_from_ != kNpos && from >= cached_from_ &&
        (cached_ == kNpos || cached_ >= from)) {
      return cached_;
    }
    cached_from_ = from;
    cached_ = Scan(from);
    return cached_;
  }

 private:
  size_t Scan(size_t from) const {
    if (pred_) {
      for (size_t i = from; i < s_.size(); ++i) {
        if (pred_(s_[i])) {
          return i;
        }
      }
      return kNpos;
    }
    if (from > s_.size()) {
      return kNpos;
    }
    for (size_t i = from; s_.size() - i >= literal_.size(); ++i) {
      if (MatchesAt(s_, i, literal_)) {
        return i;
      }
    }
    return kNpos;
  }

  std::string_view s_;
  std::string_view literal_;
  bool (*pred_)(char) = nullptr;
  size_t cached_from_ = kNpos;
  size_t cached_ = kNpos;
};

bool IsGt(char c) {
  return c == '>';
}

bool IsQuoteOrLineBreak(char c) {
  return IsQuote(c) || IsLineBreak(c);
}

struct ElementMatch {
  std::string_view attrs;  // between the tag name and '>'
  std::string_view inner;  // between '>' and the closing tag
};

// `<open[^>]*>(.*?)</close>` (icase): every match, or only the first.
//   [^>]*>  runs to the first '>' (it can't consume one), so a start
//           without a '>' after it fails, and so does every later start;
//   (.*?)   ends at the first closing tag, which must come before the next
//           line break. If it doesn't, every other start before that same
//           '>' fails the same way, so the search resumes after it.
std::vector<ElementMatch> FindElements(std::string_view s,
                                       std::string_view open,
                                       std::string_view close,
                                       bool first_only) {
  std::vector<ElementMatch> out;
  Finder next_open(s, open);
  Finder next_close(s, close);
  Finder next_gt(s, &IsGt);
  Finder next_break(s, &IsLineBreak);
  size_t from = 0;
  while (true) {
    const size_t p = next_open.Next(from);
    if (p == kNpos) {
      break;
    }
    const size_t gt = next_gt.Next(p + open.size());
    if (gt == kNpos) {
      break;
    }
    const size_t inner = gt + 1;
    const size_t end = next_close.Next(inner);
    const size_t line_break = next_break.Next(inner);
    if (end != kNpos && (line_break == kNpos || line_break > end)) {
      out.push_back({s.substr(p + open.size(), gt - p - open.size()),
                     s.substr(inner, end - inner)});
      if (first_only) {
        break;
      }
      from = end + close.size();
    } else {
      from = gt + 1;
    }
  }
  return out;
}

// `name=["'](.*?)["']` (icase), first match, where |name| is e.g. "type=":
// the value runs from the opening quote to the next quote of either kind,
// which must come before the next line break.
std::optional<std::string_view> FirstQuotedValue(std::string_view s,
                                                 std::string_view name) {
  Finder next_name(s, name);
  Finder next_end(s, &IsQuoteOrLineBreak);
  size_t from = 0;
  while (true) {
    const size_t at = next_name.Next(from);
    if (at == kNpos) {
      return std::nullopt;
    }
    const size_t quote = at + name.size();
    if (quote < s.size() && IsQuote(s[quote])) {
      const size_t value = quote + 1;
      const size_t end = next_end.Next(value);
      if (end != kNpos && IsQuote(s[end])) {
        return s.substr(value, end - value);
      }
    }
    from = at + 1;
  }
}

// regex_replace(s, `<open[\s\S]*?</close>` (icase), ""): drops each block
// from an opening tag to the first closing tag after it. A start with no
// closing tag after it has no match, and neither has any later start.
std::string RemoveBlocks(std::string_view s,
                         std::string_view open,
                         std::string_view close) {
  std::string out;
  out.reserve(s.size());
  Finder next_open(s, open);
  Finder next_close(s, close);
  size_t from = 0;
  while (true) {
    const size_t p = next_open.Next(from);
    if (p == kNpos) {
      break;
    }
    const size_t end = next_close.Next(p + open.size());
    if (end == kNpos) {
      break;
    }
    out.append(s.substr(from, p - from));
    from = end + close.size();
  }
  out.append(s.substr(from));
  return out;
}

}  // namespace

DOMInterpreter::DOMInterpreter() = default;
DOMInterpreter::~DOMInterpreter() = default;

// static
std::string DOMInterpreter::CapHtmlForParsing(std::string html) {
  if (html.size() <= kMaxParseHtmlBytes) {
    return html;
  }
  size_t cut = kMaxParseHtmlBytes;
  // Don't split a UTF-8 sequence: back up over continuation bytes.
  while (cut > 0 && (static_cast<unsigned char>(html[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  html.resize(cut);
  return html;
}

StructuredPage DOMInterpreter::ParseHTML(const std::string& html,
                                          const std::string& url) const {
  StructuredPage page;
  page.page_url = url;
  page.was_truncated = false;

  // Title: <title[^>]*>(.*?)</title>, first match.
  std::vector<ElementMatch> title =
      FindElements(html, "<title", "</title>", /*first_only=*/true);
  if (!title.empty()) {
    page.page_title = std::string(title[0].inner);
  }

  // Meta description, first match of
  // <meta\s+name=["']description["']\s+content=["'](.*?)["']
  // Each \s+ is followed by a non-space literal, so it takes the whole run
  // of whitespace; the rest is fixed text.
  {
    std::string_view s(html);
    Finder next_meta(s, "<meta");
    Finder next_end(s, &IsQuoteOrLineBreak);
    size_t from = 0;
    while (true) {
      const size_t p = next_meta.Next(from);
      if (p == kNpos) {
        break;
      }
      size_t i = p + 5;
      auto spaces = [&]() {
        const size_t begin = i;
        while (i < s.size() && IsSpace(s[i])) {
          ++i;
        }
        return i > begin;
      };
      auto literal = [&](std::string_view lit) {
        if (!MatchesAt(s, i, lit)) {
          return false;
        }
        i += lit.size();
        return true;
      };
      auto quote = [&]() {
        if (i >= s.size() || !IsQuote(s[i])) {
          return false;
        }
        ++i;
        return true;
      };
      if (spaces() && literal("name=") && quote() && literal("description") &&
          quote() && spaces() && literal("content=") && quote()) {
        const size_t end = next_end.Next(i);
        if (end != kNpos && IsQuote(s[end])) {
          page.meta_description = std::string(s.substr(i, end - i));
          break;
        }
      }
      from = p + 1;
    }
  }

  // Extract headings (h1-h6)
  for (int level = 1; level <= 6; ++level) {
    std::string tag = "h" + std::to_string(level);
    auto headings = ExtractByTag(html, tag);
    for (auto& h : headings) {
      page.headings.push_back(StripHTMLTags(h));
    }
  }

  // Extract links
  page.links = ExtractLinks(html);

  // Extract buttons
  page.buttons = ExtractButtons(html);

  // Extract forms
  page.forms = ExtractForms(html);

  // Extract main content
  page.main_content = ExtractMainContent(html);

  // Calculate metrics
  page.total_text_length = static_cast<int>(page.main_content.length());
  page.estimated_tokens = page.total_text_length / 4;  // Rough estimate

  // Truncate if needed
  if (page.estimated_tokens > kMaxOutputTokens) {
    page.main_content = TruncateToTokenLimit(page.main_content, kMaxOutputTokens);
    page.was_truncated = true;
    page.estimated_tokens = kMaxOutputTokens;
  }

  return page;
}

std::vector<ButtonInfo> DOMInterpreter::ExtractButtons(
    const std::string& html) const {
  std::vector<ButtonInfo> buttons;

  // <button([^>]*)>(.*?)</button>
  for (const ElementMatch& m :
       FindElements(html, "<button", "</button>", /*first_only=*/false)) {
    ButtonInfo btn;
    btn.text = StripHTMLTags(std::string(m.inner));
    // type=["'](.*?)["'] etc. on the attributes.
    std::optional<std::string_view> type = FirstQuotedValue(m.attrs, "type=");
    btn.type = type ? std::string(*type) : "button";
    if (auto id = FirstQuotedValue(m.attrs, "id=")) {
      btn.id = std::string(*id);
    }
    if (auto aria = FirstQuotedValue(m.attrs, "aria-label=")) {
      btn.aria_label = std::string(*aria);
    }
    btn.is_submit = (btn.type == "submit");
    buttons.push_back(btn);
  }

  // <input([^>]*type=["'](submit|button)["'][^>]*)>
  // The first [^>]* is greedy, so of the type=… attributes between the tag
  // name and the first '>', the last one that fits counts. A start without
  // one fails, and so does every other start before that '>'.
  std::string_view s(html);
  Finder next_input(s, "<input");
  Finder next_gt(s, &IsGt);
  size_t from = 0;
  while (true) {
    const size_t p = next_input.Next(from);
    if (p == kNpos) {
      break;
    }
    const size_t attrs = p + 6;
    const size_t gt = next_gt.Next(attrs);
    if (gt == kNpos) {
      break;
    }
    // type= (5) + quote + submit|button (6) + quote = 13 bytes before '>'.
    std::optional<size_t> type_at;
    for (size_t x = attrs; x + 13 <= gt; ++x) {
      if (MatchesAt(s, x, "type=") && IsQuote(s[x + 5]) &&
          (MatchesAt(s, x + 6, "submit") || MatchesAt(s, x + 6, "button")) &&
          IsQuote(s[x + 12])) {
        type_at = x;
      }
    }
    if (type_at) {
      ButtonInfo btn;
      std::string_view attr_text = s.substr(attrs, gt - attrs);
      btn.type = std::string(s.substr(*type_at + 6, 6));
      if (auto value = FirstQuotedValue(attr_text, "value=")) {
        btn.text = std::string(*value);
      }
      btn.is_submit = (btn.type == "submit");
      buttons.push_back(btn);
    }
    from = gt + 1;
  }

  return buttons;
}

std::vector<LinkInfo> DOMInterpreter::ExtractLinks(
    const std::string& html) const {
  std::vector<LinkInfo> links;

  // <a\s([^>]*href=["'](.*?)["'][^>]*)>(.*?)</a>
  // From a start "<a" + one whitespace byte, the regex tries:
  //  - for the leading [^>]* (greedy, up to the first '>'), the positions
  //    of href=" from the last one before that '>' backwards;
  //  - for the href value (.*?), each next quote, up to the first line
  //    break after the value starts (the value may run past a '>');
  //  - after that quote, [^>]*> reaches the first '>' after it, and the
  //    link text (.*?) the first "</a>" after that '>', with no line break
  //    in between.
  // So a closing quote at |v| works iff that last condition holds; it
  // doesn't depend on the start, which is what keeps this linear: the
  // positions where it holds are listed once, and every start before the
  // same '>' as a failed one fails too.
  std::string_view s(html);
  std::vector<size_t> line_breaks;
  std::vector<size_t> good_quotes;
  {
    Finder next_gt(s, &IsGt);
    Finder next_close(s, "</a>");
    Finder next_break(s, &IsLineBreak);
    for (size_t v = 0; v < s.size(); ++v) {
      if (IsLineBreak(s[v])) {
        line_breaks.push_back(v);
      }
      if (!IsQuote(s[v])) {
        continue;
      }
      const size_t gt = next_gt.Next(v + 1);
      if (gt == kNpos) {
        continue;
      }
      const size_t end = next_close.Next(gt + 1);
      const size_t line_break = next_break.Next(gt + 1);
      if (end != kNpos && (line_break == kNpos || line_break > end)) {
        good_quotes.push_back(v);
      }
    }
  }
  auto first_at_or_after = [](const std::vector<size_t>& v, size_t x) {
    auto it = std::lower_bound(v.begin(), v.end(), x);
    return it == v.end() ? kNpos : *it;
  };

  Finder next_a(s, "<a");
  Finder next_gt(s, &IsGt);
  Finder next_href(s, "href=");
  size_t from = 0;
  while (true) {
    const size_t p = next_a.Next(from);
    if (p == kNpos) {
      break;
    }
    if (p + 2 >= s.size() || !IsSpace(s[p + 2])) {
      from = p + 1;
      continue;
    }
    const size_t attrs = p + 3;
    const size_t tag_gt = next_gt.Next(attrs);
    const size_t limit = tag_gt == kNpos ? s.size() : tag_gt;
    // href=" candidates inside the tag, tried last first.
    std::vector<size_t> hrefs;
    for (size_t h = next_href.Next(attrs); h != kNpos && h + 5 < limit;
         h = next_href.Next(h + 1)) {
      if (IsQuote(s[h + 5])) {
        hrefs.push_back(h);
      }
    }
    bool matched = false;
    for (auto it = hrefs.rbegin(); it != hrefs.rend(); ++it) {
      const size_t value = *it + 6;
      const size_t quote = first_at_or_after(good_quotes, value);
      const size_t line_break = first_at_or_after(line_breaks, value);
      if (quote == kNpos || (line_break != kNpos && line_break < quote)) {
        continue;
      }
      const size_t gt = next_gt.Next(quote + 1);
      Finder close_after(s, "</a>");
      const size_t end = close_after.Next(gt + 1);
      LinkInfo link;
      link.href = std::string(s.substr(value, quote - value));
      link.text = StripHTMLTags(std::string(s.substr(gt + 1, end - gt - 1)));
      if (auto aria = FirstQuotedValue(s.substr(attrs, gt - attrs),
                                       "aria-label=")) {
        link.aria_label = std::string(*aria);
      }
      // Determine if it's a navigation link
      link.is_navigation = (link.href.find('#') != 0 &&
                            link.href.find("javascript:") != 0 &&
                            !link.href.empty());
      links.push_back(link);
      from = end + 4;
      matched = true;
      break;
    }
    if (!matched) {
      if (tag_gt == kNpos) {
        break;
      }
      from = tag_gt + 1;
    }
  }

  return links;
}

std::vector<FormInfo> DOMInterpreter::ExtractForms(
    const std::string& html) const {
  std::vector<FormInfo> forms;

  // <form([^>]*)>([\s\S]*?)</form>: the body may span lines, so a start
  // fails only when there's no '>' or no </form> after it, and then so
  // does every later start.
  std::string_view s(html);
  Finder next_form(s, "<form");
  Finder next_close(s, "</form>");
  Finder next_gt(s, &IsGt);
  size_t from = 0;
  while (true) {
    const size_t p = next_form.Next(from);
    if (p == kNpos) {
      break;
    }
    const size_t gt = next_gt.Next(p + 5);
    if (gt == kNpos) {
      break;
    }
    const size_t end = next_close.Next(gt + 1);
    if (end == kNpos) {
      break;
    }
    std::string_view attrs = s.substr(p + 5, gt - p - 5);
    std::string_view body = s.substr(gt + 1, end - gt - 1);
    from = end + 7;

    FormInfo form;
    if (auto action = FirstQuotedValue(attrs, "action=")) {
      form.action = std::string(*action);
    }
    if (auto method = FirstQuotedValue(attrs, "method=")) {
      form.method = std::string(*method);
    }

    // <input([^>]*)> in the body.
    Finder next_input(body, "<input");
    Finder body_gt(body, &IsGt);
    size_t in_from = 0;
    while (true) {
      const size_t ip = next_input.Next(in_from);
      if (ip == kNpos) {
        break;
      }
      const size_t igt = body_gt.Next(ip + 6);
      if (igt == kNpos) {
        break;
      }
      std::string_view input_attrs = body.substr(ip + 6, igt - ip - 6);
      in_from = igt + 1;

      FormField field;
      if (auto name = FirstQuotedValue(input_attrs, "name=")) {
        field.name = std::string(*name);
      }
      std::optional<std::string_view> type =
          FirstQuotedValue(input_attrs, "type=");
      field.type = type ? std::string(*type) : "text";
      if (auto placeholder = FirstQuotedValue(input_attrs, "placeholder=")) {
        field.placeholder = std::string(*placeholder);
      }
      field.is_required = (input_attrs.find("required") != kNpos);

      form.fields.push_back(field);
    }

    forms.push_back(form);
  }

  return forms;
}

std::string DOMInterpreter::ExtractMainContent(const std::string& html) const {
  // Remove script and style tags, then nav, header and footer elements
  // (typically non-content): each `<x[\s\S]*?</x>` in turn.
  std::string content = RemoveBlocks(html, "<script", "</script>");
  content = RemoveBlocks(content, "<style", "</style>");
  content = RemoveBlocks(content, "<nav", "</nav>");
  content = RemoveBlocks(content, "<header", "</header>");
  content = RemoveBlocks(content, "<footer", "</footer>");

  // Strip remaining HTML tags
  content = StripHTMLTags(content);

  // Clean whitespace
  content = CleanWhitespace(content);

  return content;
}

std::string DOMInterpreter::TruncateToTokenLimit(const std::string& content,
                                                   int max_tokens) {
  // Rough estimate: 1 token ≈ 4 characters
  size_t max_chars = static_cast<size_t>(max_tokens) * 4;
  if (content.length() <= max_chars) {
    return content;
  }
  return content.substr(0, max_chars) + "\n[Content truncated...]";
}

std::string DOMInterpreter::StripHTMLTags(const std::string& html) const {
  // regex_replace(html, `<[^>]*>`, ""): each '<' up to the first '>' after
  // it goes; a '<' with no '>' after it stays, as does everything after.
  std::string out;
  out.reserve(html.size());
  size_t from = 0;
  while (true) {
    const size_t lt = html.find('<', from);
    if (lt == std::string::npos) {
      break;
    }
    const size_t gt = html.find('>', lt + 1);
    if (gt == std::string::npos) {
      break;
    }
    out.append(html, from, lt - from);
    from = gt + 1;
  }
  out.append(html, from, std::string::npos);
  return out;
}

std::string DOMInterpreter::CleanWhitespace(const std::string& text) const {
  // Collapse each run of \s into a single space (regex_replace `\s+`).
  std::string result;
  result.reserve(text.size());
  bool in_space = false;
  for (char c : text) {
    if (IsSpace(c)) {
      if (!in_space) {
        result.push_back(' ');
      }
      in_space = true;
    } else {
      result.push_back(c);
      in_space = false;
    }
  }

  // Trim leading/trailing whitespace
  size_t start = result.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = result.find_last_not_of(" \t\n\r");
  return result.substr(start, end - start + 1);
}

std::vector<std::string> DOMInterpreter::ExtractByTag(
    const std::string& html, const std::string& tag) const {
  // <tag[^>]*>(.*?)</tag>, every match.
  std::vector<std::string> results;
  for (const ElementMatch& m : FindElements(html, "<" + tag, "</" + tag + ">",
                                            /*first_only=*/false)) {
    results.push_back(std::string(m.inner));
  }
  return results;
}

// ---- StructuredPage serialization ----

std::string StructuredPage::ToJSON() const {
  std::ostringstream json;
  json << "{\n";
  json << "  \"page_title\": \"" << page_title << "\",\n";
  json << "  \"page_url\": \"" << page_url << "\",\n";
  json << "  \"meta_description\": \"" << meta_description << "\",\n";

  json << "  \"headings\": [";
  for (size_t i = 0; i < headings.size(); ++i) {
    json << "\"" << headings[i] << "\"";
    if (i < headings.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"buttons\": [";
  for (size_t i = 0; i < buttons.size(); ++i) {
    json << "{\"text\": \"" << buttons[i].text << "\", "
         << "\"type\": \"" << buttons[i].type << "\"}";
    if (i < buttons.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"links\": [";
  for (size_t i = 0; i < links.size() && i < 20; ++i) {
    json << "{\"text\": \"" << links[i].text << "\", "
         << "\"href\": \"" << links[i].href << "\"}";
    if (i < links.size() - 1 && i < 19) json << ", ";
  }
  json << "],\n";

  json << "  \"forms\": [";
  for (size_t i = 0; i < forms.size(); ++i) {
    json << "{\"action\": \"" << forms[i].action << "\", "
         << "\"fields\": " << forms[i].fields.size() << "}";
    if (i < forms.size() - 1) json << ", ";
  }
  json << "],\n";

  json << "  \"main_content\": \"" << main_content.substr(0, 500) << "\",\n";
  json << "  \"estimated_tokens\": " << estimated_tokens << ",\n";
  json << "  \"was_truncated\": " << (was_truncated ? "true" : "false") << "\n";
  json << "}\n";

  return json.str();
}

std::string StructuredPage::ToCompactText() const {
  std::ostringstream text;
  text << "PAGE: " << page_title << "\n";
  text << "URL: " << page_url << "\n";

  if (!headings.empty()) {
    text << "HEADINGS: ";
    for (size_t i = 0; i < headings.size() && i < 10; ++i) {
      text << headings[i];
      if (i < headings.size() - 1) text << " | ";
    }
    text << "\n";
  }

  if (!buttons.empty()) {
    text << "BUTTONS: ";
    for (size_t i = 0; i < buttons.size() && i < 10; ++i) {
      text << "[" << buttons[i].text << "]";
      if (i < buttons.size() - 1) text << " ";
    }
    text << "\n";
  }

  text << "CONTENT:\n" << main_content.substr(0, 2000) << "\n";

  return text.str();
}

}  // namespace molt_ai
