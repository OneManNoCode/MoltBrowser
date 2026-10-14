// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/agent_safety.h"

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/values.h"
#include "url/gurl.h"

namespace molt_ai {
namespace agent_safety {

namespace {

char AsciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    c = AsciiLower(c);
  return out;
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.substr(s.size() - suffix.size()) == suffix;
}

// Splits |s| on ASCII whitespace.
std::vector<std::string> SplitTokens(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (IsAsciiSpace(c)) {
      if (!cur.empty())
        out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty())
    out.push_back(std::move(cur));
  return out;
}

// Replaces every case-insensitive occurrence of |needle| (lowercase) in
// |haystack| with |replacement|.
std::string ReplaceAllCaseInsensitive(const std::string& haystack,
                                      std::string_view needle,
                                      std::string_view replacement) {
  if (needle.empty())
    return haystack;
  const std::string lower = ToLower(haystack);
  std::string out;
  size_t pos = 0;
  while (true) {
    size_t hit = lower.find(needle, pos);
    if (hit == std::string::npos)
      break;
    out.append(haystack, pos, hit - pos);
    out.append(replacement);
    pos = hit + needle.size();
  }
  out.append(haystack, pos, std::string::npos);
  return out;
}

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// Non-ASCII angle-bracket look-alikes (UTF-8) that a page could use to fake
// a fence marker. Folded to ASCII before the run-collapsing pass.
constexpr std::pair<std::string_view, char> kAngleLookalikes[] = {
    {"\xEF\xBC\x9C", '<'},  // U+FF1C FULLWIDTH LESS-THAN SIGN
    {"\xEF\xBC\x9E", '>'},  // U+FF1E FULLWIDTH GREATER-THAN SIGN
    {"\xE2\x80\xB9", '<'},  // U+2039 SINGLE LEFT-POINTING ANGLE QUOTATION
    {"\xE2\x80\xBA", '>'},  // U+203A SINGLE RIGHT-POINTING ANGLE QUOTATION
    {"\xC2\xAB", '<'},      // U+00AB LEFT-POINTING DOUBLE ANGLE QUOTATION
    {"\xC2\xBB", '>'},      // U+00BB RIGHT-POINTING DOUBLE ANGLE QUOTATION
    {"\xE3\x80\x88", '<'},  // U+3008 LEFT ANGLE BRACKET
    {"\xE3\x80\x89", '>'},  // U+3009 RIGHT ANGLE BRACKET
};

// The probe runs as `(<this>)(<JSON list of selectors>)` in an isolated
// world. Isolated-world prototypes are pristine, so el.type / el.form /
// closest() cannot be spoofed by page script. `value` is only read for
// button-like inputs: reading it on a text / password field would copy what
// the user typed into the probe result.
constexpr char kElementProbeFn[] = R"JS((sels) => sels.map((sel) => {
  try {
    const el = document.querySelector(sel);
    if (!el) return {found: false};
    const out = {found: true, type: String(el.type || ''),
                 autocomplete: String(el.getAttribute('autocomplete') || ''),
                 submit: false, nav: '', label: ''};
    const sub = el.closest('button,input');
    if (sub && sub.form && (sub.type === 'submit' || sub.type === 'image')) {
      out.submit = true;
      out.nav = String(sub.hasAttribute('formaction') ? sub.formAction
                                                      : sub.form.action);
    }
    const a = el.closest('a[href],area[href]');
    if (!out.nav && a) out.nav = String(a.href || '');
    const buttonish = el.type === 'submit' || el.type === 'button' ||
                      el.type === 'reset';
    out.label = String(el.getAttribute('aria-label') ||
                       el.getAttribute('placeholder') || el.textContent ||
                       (buttonish ? el.value : '') || '')
                    .replace(/\s+/g, ' ').trim().slice(0, 60);
    return out;
  } catch (e) {
    return {found: false};
  }
}))JS";

}  // namespace

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool IsAllowedNavigationURL(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS();
}

std::vector<std::string> ExtractExplicitURLs(std::string_view text) {
  std::vector<std::string> out;
  const std::string lower = ToLower(text);
  size_t pos = 0;
  while (pos < lower.size()) {
    size_t http = lower.find("http://", pos);
    size_t https = lower.find("https://", pos);
    size_t start = std::min(http, https);
    if (start == std::string::npos)
      break;
    size_t end = start;
    while (end < text.size()) {
      char c = text[end];
      if (IsAsciiSpace(c) || c == '"' || c == '\'' || c == '<' || c == '>' ||
          c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
          c == '}' || c == '`') {
        break;
      }
      ++end;
    }
    std::string_view url = text.substr(start, end - start);
    while (!url.empty() && (url.back() == '.' || url.back() == ',' ||
                            url.back() == ';' || url.back() == ':' ||
                            url.back() == '!' || url.back() == '?')) {
      url.remove_suffix(1);
    }
    // Skip a bare scheme ("https://" followed by nothing usable).
    const size_t scheme_len = (start == https) ? 8u : 7u;
    if (url.size() > scheme_len)
      out.emplace_back(url);
    pos = std::max(end, start + 1);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Sensitive form fields
// ---------------------------------------------------------------------------

FieldKind ClassifyField(std::string_view input_type,
                        std::string_view autocomplete) {
  std::string type = ToLower(input_type);
  // Trim surrounding whitespace.
  while (!type.empty() && IsAsciiSpace(type.back()))
    type.pop_back();
  while (!type.empty() && IsAsciiSpace(type.front()))
    type.erase(type.begin());

  bool password = type == "password";
  bool card = false;
  bool otp = false;
  // autocomplete is a space-separated token list, e.g.
  // "section-checkout billing cc-number".
  for (const std::string& raw : SplitTokens(autocomplete)) {
    const std::string token = ToLower(raw);
    if (token == "current-password" || token == "new-password")
      password = true;
    else if (StartsWith(token, "cc-"))
      card = true;
    else if (token == "one-time-code")
      otp = true;
  }
  if (password)
    return FieldKind::kPassword;
  if (card)
    return FieldKind::kPaymentCard;
  if (otp)
    return FieldKind::kOneTimeCode;
  return FieldKind::kOrdinary;
}

bool IsSensitive(FieldKind kind) {
  return kind != FieldKind::kOrdinary;
}

std::string_view FieldKindToString(FieldKind kind) {
  switch (kind) {
    case FieldKind::kOrdinary:
      return "";
    case FieldKind::kPassword:
      return "password";
    case FieldKind::kPaymentCard:
      return "payment_card";
    case FieldKind::kOneTimeCode:
      return "one_time_code";
  }
  return "";
}

FieldKind FieldKindFromString(std::string_view name) {
  if (name == "password")
    return FieldKind::kPassword;
  if (name == "payment_card")
    return FieldKind::kPaymentCard;
  if (name == "one_time_code")
    return FieldKind::kOneTimeCode;
  return FieldKind::kOrdinary;
}

std::string_view FieldKindLabel(FieldKind kind) {
  switch (kind) {
    case FieldKind::kOrdinary:
      return "form";
    case FieldKind::kPassword:
      return "password";
    case FieldKind::kPaymentCard:
      return "payment card";
    case FieldKind::kOneTimeCode:
      return "one-time code";
  }
  return "form";
}

FieldKind ClassifySelector(std::string_view selector) {
  // Tokenize into CSS identifier-ish runs ([a-z0-9_-]) so attribute names,
  // ids, classes and quoted attribute values each become one token.
  const std::string lower = ToLower(selector);
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : lower) {
    bool ident = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
    if (ident) {
      cur.push_back(c);
    } else if (!cur.empty()) {
      tokens.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty())
    tokens.push_back(std::move(cur));

  for (const std::string& t : tokens) {
    // Passwords: `password`, `passwd`, `pwd`, `user_password`,
    // `loginpasswd`, `current-password`, `new-password`.
    if (t == "pwd" || EndsWith(t, "password") || EndsWith(t, "passwd"))
      return FieldKind::kPassword;
    // Payment cards: autocomplete tokens (cc-number, cc-csc, cc-exp...)
    // and the common field names.
    if (StartsWith(t, "cc-") || t == "cardnumber" || t == "card-number" ||
        t == "card_number" || t == "creditcard" || t == "credit-card" ||
        t == "cvv" || t == "cvc") {
      return FieldKind::kPaymentCard;
    }
    // One-time codes.
    if (t == "one-time-code" || t == "otp")
      return FieldKind::kOneTimeCode;
  }
  return FieldKind::kOrdinary;
}

bool SelectorLooksSensitive(std::string_view selector) {
  return IsSensitive(ClassifySelector(selector));
}

// ---------------------------------------------------------------------------
// Element probe
// ---------------------------------------------------------------------------

std::string BuildElementProbeScript(const std::vector<std::string>& selectors) {
  base::ListValue list;
  for (const std::string& sel : selectors)
    list.Append(sel);
  std::string json;
  base::JSONWriter::Write(list, &json);
  return std::string("(") + kElementProbeFn + ")(" + json + ")";
}

std::vector<ElementProbe> ParseElementProbes(const base::Value& result,
                                             size_t expected) {
  std::vector<ElementProbe> out(expected);
  if (!result.is_list())
    return out;
  const base::ListValue& list = result.GetList();
  for (size_t i = 0; i < expected && i < list.size(); ++i) {
    if (!list[i].is_dict())
      continue;
    const base::DictValue& d = list[i].GetDict();
    ElementProbe& p = out[i];
    p.found = d.FindBool("found").value_or(false);
    if (!p.found)
      continue;
    const std::string* type = d.FindString("type");
    const std::string* autocomplete = d.FindString("autocomplete");
    p.field_kind = ClassifyField(type ? *type : std::string(),
                                 autocomplete ? *autocomplete : std::string());
    p.submits_form = d.FindBool("submit").value_or(false);
    if (const std::string* nav = d.FindString("nav"))
      p.navigates_to = *nav;
    if (const std::string* label = d.FindString("label"))
      p.label = *label;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Untrusted page content
// ---------------------------------------------------------------------------

std::string SanitizeUntrustedText(std::string_view text) {
  // 1. Fold non-ASCII angle-bracket look-alikes to ASCII.
  std::string folded(text);
  for (const auto& [from, to] : kAngleLookalikes)
    ReplaceAll(folded, from, std::string_view(&to, 1));

  // 2. Drop control characters; cap '<' / '>' runs at two so the fence
  //    markers (which open with "<<<") can never appear in page text.
  std::string out;
  out.reserve(folded.size());
  for (size_t i = 0; i < folded.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(folded[i]);
    if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f)
      continue;
    if (c == '<' || c == '>') {
      size_t j = i;
      while (j < folded.size() && folded[j] == folded[i])
        ++j;
      out.append(std::min<size_t>(j - i, 2), static_cast<char>(c));
      i = j - 1;
      continue;
    }
    out.push_back(static_cast<char>(c));
  }

  // 3. Defang chat-template control tokens (<|im_start|>, <|eot_id|>,
  //    <|start_header_id|>, ...) and the fence's own wording.
  ReplaceAll(out, "<|", "< |");
  ReplaceAll(out, "|>", "| >");
  return ReplaceAllCaseInsensitive(out, "untrusted_page_content",
                                   "untrusted page content");
}

std::string FenceUntrustedText(std::string_view text) {
  return std::string(kUntrustedBegin) + "\n" + SanitizeUntrustedText(text) +
         "\n" + kUntrustedEnd;
}

// ---------------------------------------------------------------------------
// RUN_JS
// ---------------------------------------------------------------------------

std::string BuildRunJsSource(std::string_view user_js,
                             std::string_view vars_json) {
  std::string out = "{const vars=Object.freeze(";
  out.append(vars_json.empty() ? std::string_view("{}") : vars_json);
  out.append(");\n");
  out.append(user_js);
  // Newline first so a trailing `// comment` in the body cannot swallow the
  // closing brace.
  out.append("\n}");
  return out;
}

}  // namespace agent_safety
}  // namespace molt_ai
