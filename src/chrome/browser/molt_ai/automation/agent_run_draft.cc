// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/agent_run_draft.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {
namespace agent_draft {

RunElement::RunElement() = default;
RunElement::~RunElement() = default;
RunElement::RunElement(const RunElement&) = default;
RunElement& RunElement::operator=(const RunElement&) = default;
RunElement::RunElement(RunElement&&) = default;
RunElement& RunElement::operator=(RunElement&&) = default;

RunStep::RunStep() = default;
RunStep::~RunStep() = default;
RunStep::RunStep(const RunStep&) = default;
RunStep& RunStep::operator=(const RunStep&) = default;
RunStep::RunStep(RunStep&&) = default;
RunStep& RunStep::operator=(RunStep&&) = default;

Run::Run() = default;
Run::~Run() = default;
Run::Run(const Run&) = default;
Run& Run::operator=(const Run&) = default;
Run::Run(Run&&) = default;
Run& Run::operator=(Run&&) = default;

Draft::Draft() = default;
Draft::~Draft() = default;
Draft::Draft(Draft&&) = default;
Draft& Draft::operator=(Draft&&) = default;

namespace {

constexpr uint32_t kReplacementChar = 0xFFFD;
constexpr uint32_t kEllipsis = 0x2026;

// The goal is scanned for secrets up to this size; a longer goal makes
// every typed value withheld (and the draft's name generic).
constexpr size_t kMaxGoalScanBytes = 4096;

// FILL_FORM's cap (agent_actions::kMaxFormFields).
constexpr size_t kMaxFormFields = 8;

// ---- UTF-8 ------------------------------------------------------------------

// Strict UTF-8: the length of the character at |s|[i] (and |*cp|), or 0 when
// it isn't one (overlong, surrogate, past U+10FFFF, cut off).
size_t DecodeUtf8At(std::string_view s, size_t i, uint32_t* cp) {
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    *cp = b0;
    return 1;
  }
  size_t len = 0;
  uint32_t c = 0;
  uint32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    c = b0 & 0x1F;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    c = b0 & 0x0F;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    c = b0 & 0x07;
    min = 0x10000;
  } else {
    return 0;
  }
  if (len > s.size() - i)
    return 0;
  for (size_t k = 1; k < len; ++k) {
    const unsigned char b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80)
      return 0;
    c = (c << 6) | (b & 0x3F);
  }
  if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
    return 0;
  *cp = c;
  return len;
}

bool IsValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = DecodeUtf8At(s, i, &cp);
    if (!n)
      return false;
    i += n;
  }
  return true;
}

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  }
  return n;
}

// C0 controls and DEL.
bool IsControl(uint32_t cp) {
  return cp < 0x20 || cp == 0x7F;
}

// Characters that reorder, hide or break text: C1 controls, U+061C,
// U+180E, U+200B-U+200F, U+2028-U+202E, U+2060-U+2064, U+2066-U+2069,
// U+FEFF (agent_plan.cc's list).
bool IsHidden(uint32_t cp) {
  return (cp >= 0x80 && cp <= 0x9F) || cp == 0x061C || cp == 0x180E ||
         (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) ||
         cp == 0xFEFF;
}

// |s| without controls, hidden characters and invalid bytes: what a
// keyword check reads, so "pass" U+200B "word" is still "password".
std::string StripHidden(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = DecodeUtf8At(s, i, &cp);
    if (!n) {
      ++i;
      continue;
    }
    if (!IsControl(cp) && !IsHidden(cp))
      out.append(s.substr(i, n));
    i += n;
  }
  return out;
}

// ---- ASCII helpers ------------------------------------------------------------

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}
bool IsLower(char c) {
  return c >= 'a' && c <= 'z';
}
bool IsUpper(char c) {
  return c >= 'A' && c <= 'Z';
}
bool IsAlpha(char c) {
  return IsLower(c) || IsUpper(c);
}
bool IsAlnum(char c) {
  return IsAlpha(c) || IsDigit(c);
}
bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (IsUpper(c))
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view s, std::string_view needle) {
  return s.find(needle) != std::string_view::npos;
}

bool HasTemplateBraces(std::string_view s) {
  return Contains(s, "{{") || Contains(s, "}}");
}

// Splits on ASCII whitespace (and on |more|, if given).
std::vector<std::string_view> SplitTokens(std::string_view s,
                                          std::string_view more = {}) {
  std::vector<std::string_view> out;
  size_t start = 0;
  bool in_token = false;
  for (size_t i = 0; i <= s.size(); ++i) {
    const bool sep = i == s.size() || IsSpace(s[i]) ||
                     more.find(s[i]) != std::string_view::npos;
    if (sep) {
      if (in_token)
        out.push_back(s.substr(start, i - start));
      in_token = false;
    } else if (!in_token) {
      start = i;
      in_token = true;
    }
  }
  return out;
}

// Lower-case ASCII words ([a-z0-9]+); everything else separates them.
std::vector<std::string> Words(std::string_view s) {
  std::vector<std::string> out;
  std::string word;
  for (char c : s) {
    if (IsAlnum(c)) {
      word.push_back(IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c);
    } else if (!word.empty()) {
      out.push_back(std::move(word));
      word.clear();
    }
  }
  if (!word.empty())
    out.push_back(std::move(word));
  return out;
}

// " w1 w2 ... " — phrases are searched as " a b ".
std::string JoinWords(const std::vector<std::string>& words) {
  std::string out = " ";
  for (const std::string& w : words) {
    out += w;
    out += ' ';
  }
  return out;
}

bool HasWordOrPhrase(const std::string& joined,
                     const std::vector<std::string>& words,
                     std::initializer_list<std::string_view> single,
                     std::initializer_list<std::string_view> phrases) {
  for (const std::string& w : words) {
    for (std::string_view s : single) {
      if (w == s)
        return true;
    }
  }
  for (std::string_view p : phrases) {
    std::string needle = " ";
    needle += p;
    needle += ' ';
    if (Contains(joined, needle))
      return true;
  }
  return false;
}

std::string_view TrimPunct(std::string_view s) {
  constexpr std::string_view kPunct = ".,;:!?()[]{}<>\"'";
  while (!s.empty() && kPunct.find(s.front()) != std::string_view::npos)
    s.remove_prefix(1);
  while (!s.empty() && kPunct.find(s.back()) != std::string_view::npos)
    s.remove_suffix(1);
  return s;
}

std::string PercentDecode(std::string_view s, bool plus_is_space) {
  auto hex = [](char c) -> int {
    if (IsDigit(c))
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 &&
        hex(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
      i += 2;
    } else if (plus_is_space && s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// ---- What looks secret ----------------------------------------------------

bool LuhnValid(std::string_view digits) {
  int sum = 0;
  bool alt = false;
  for (size_t k = digits.size(); k > 0; --k) {
    int n = digits[k - 1] - '0';
    if (alt) {
      n *= 2;
      if (n > 9)
        n -= 9;
    }
    sum += n;
    alt = !alt;
  }
  return sum % 10 == 0;
}

// A run of digits, single spaces or dashes between them, with 13 to 19
// digits that pass the Luhn check: "4111 1111 1111 1111".
bool HasCardNumber(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    if (!IsDigit(s[i])) {
      ++i;
      continue;
    }
    std::string digits;
    size_t j = i;
    while (j < s.size()) {
      if (IsDigit(s[j])) {
        digits.push_back(s[j]);
        ++j;
      } else if ((s[j] == ' ' || s[j] == '-') && j + 1 < s.size() &&
                 IsDigit(s[j + 1])) {
        ++j;
      } else {
        break;
      }
    }
    if (digits.size() >= 13 && digits.size() <= 19 && LuhnValid(digits))
      return true;
    i = j;
  }
  return false;
}

// 123-45-6789.
bool IsSsn(std::string_view t) {
  if (t.size() != 11 || t[3] != '-' || t[6] != '-')
    return false;
  for (size_t k = 0; k < t.size(); ++k) {
    if (k != 3 && k != 6 && !IsDigit(t[k]))
      return false;
  }
  return true;
}

// DE89370400440532013000: two capital letters, two digits, 11-30 capital
// letters or digits.
bool IsIbanShaped(std::string_view t) {
  if (t.size() < 15 || t.size() > 34)
    return false;
  if (!IsUpper(t[0]) || !IsUpper(t[1]) || !IsDigit(t[2]) || !IsDigit(t[3]))
    return false;
  bool digit = false;
  for (char c : t.substr(4)) {
    if (!IsUpper(c) && !IsDigit(c))
      return false;
    digit |= IsDigit(c);
  }
  return digit;
}

// eyJ...xxx.yyy: a JSON Web Token.
bool IsJwt(std::string_view t) {
  if (t.size() < 20 || !StartsWith(t, "eyJ"))
    return false;
  int dots = 0;
  for (char c : t) {
    if (c == '.') {
      ++dots;
    } else if (!IsAlnum(c) && c != '-' && c != '_' && c != '=') {
      return false;
    }
  }
  return dots == 2;
}

// Prefixes of well-known API keys and access tokens.
bool HasKeyPrefix(std::string_view t) {
  constexpr std::string_view kPrefixes[] = {
      "sk-",  "sk_live_", "sk_test_", "rk_live_", "pk_live_", "ghp_",
      "gho_", "ghu_",     "ghs_",     "ghr_",     "github_pat_",
      "glpat-", "xoxb-",  "xoxp-",    "xoxa-",    "xoxs-",    "AKIA",
      "ASIA", "AIza",     "ya29.",    "npm_",     "shpat_"};
  if (t.size() < 16)
    return false;
  for (std::string_view p : kPrefixes) {
    if (StartsWith(t, p))
      return true;
  }
  return false;
}

bool LooksLikeEmail(std::string_view t) {
  const size_t at = t.find('@');
  if (at == std::string_view::npos || at == 0 ||
      t.find('@', at + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view domain = t.substr(at + 1);
  if (domain.find('.') == std::string_view::npos)
    return false;
  for (char c : t) {
    if (!IsAlnum(c) && c != '.' && c != '_' && c != '%' && c != '+' &&
        c != '-' && c != '@') {
      return false;
    }
  }
  return true;
}

bool UrlValueLooksSecret(std::string_view v) {
  if (HasCardNumber(v) || IsJwt(v) || HasKeyPrefix(v))
    return true;
  for (std::string_view t : SplitTokens(v)) {
    if (IsSsn(TrimPunct(t)) || IsJwt(TrimPunct(t)) || HasKeyPrefix(TrimPunct(t)))
      return true;
  }
  return false;
}

// One whitespace-free token (trimmed of punctuation).
bool TokenLooksSecret(std::string_view tok) {
  if (StartsWith(tok, "http://") || StartsWith(tok, "https://"))
    return UrlLooksSecret(tok);
  if (IsSsn(tok) || IsIbanShaped(tok) || IsJwt(tok) || HasKeyPrefix(tok))
    return true;
  if (tok.size() < 8 || LooksLikeEmail(tok))
    return false;
  bool lower = false, upper = false, digit = false, other = false,
       strong_symbol = false, key_chars_only = true;
  constexpr std::string_view kStrong = "!#$%^&*()+=[]{}|\\;'\"<>?`~";
  for (char c : tok) {
    lower |= IsLower(c);
    upper |= IsUpper(c);
    digit |= IsDigit(c);
    // Only ASCII punctuation counts as a symbol: "Zürich2024" isn't a
    // password.
    if (!IsAlnum(c) && static_cast<unsigned char>(c) < 0x80) {
      other = true;
      strong_symbol |= kStrong.find(c) != std::string_view::npos;
    }
    if (!IsAlnum(c) && c != '_' && c != '-' && c != '+' && c != '/' &&
        c != '=' && c != '.') {
      key_chars_only = false;
    }
  }
  const bool letter = lower || upper;
  // "Tr0ub4dor&3": letters, digits and a symbol.
  if (letter && digit && strong_symbol)
    return true;
  // "P@ssw0rd!": both cases, digits and any symbol.
  if (upper && lower && digit && other)
    return true;
  // "Kx9mQ2vLp7Rz": long, both cases and digits.
  if (tok.size() >= 12 && upper && lower && digit)
    return true;
  // An API key or token: long, of key characters, letters and digits.
  if (tok.size() >= 24 && key_chars_only && letter && digit)
    return true;
  return false;
}

// "123456", "123 456", "1234-5678".
bool LooksLikeOneTimeCode(std::string_view text) {
  while (!text.empty() && IsSpace(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back()))
    text.remove_suffix(1);
  size_t digits = 0;
  size_t separators = 0;
  for (size_t k = 0; k < text.size(); ++k) {
    const char c = text[k];
    if (IsDigit(c)) {
      ++digits;
    } else if ((c == ' ' || c == '-') && k > 0 && k + 1 < text.size() &&
               IsDigit(text[k - 1]) && IsDigit(text[k + 1])) {
      ++separators;
    } else {
      return false;
    }
  }
  return digits >= 6 && digits <= 8 && separators <= 1;
}

bool IsSecretParamName(const std::string& name) {
  constexpr std::string_view kExact[] = {
      "pass",      "pwd",        "pin",       "otp",           "code",
      "key",       "auth",       "sid",       "sig",           "jwt",
      "ssn",       "cvv",        "cvc",       "csc",           "card",
      "cardnumber", "card_number", "ccnum",   "cc_number",     "passcode",
      "passphrase", "signature",  "authorization"};
  for (std::string_view e : kExact) {
    if (name == e)
      return true;
  }
  constexpr std::string_view kPart[] = {"password", "passwd", "token",
                                        "secret",   "apikey", "api_key",
                                        "session",  "credential"};
  for (std::string_view p : kPart) {
    if (Contains(name, p))
      return true;
  }
  return false;
}

// "a=1&b=2" (or ";"-separated): true if a parameter carries a secret.
bool ParamsLookSecret(std::string_view params, bool require_equals) {
  for (std::string_view piece : SplitTokens(params, "&;")) {
    const size_t eq = piece.find('=');
    if (eq == std::string_view::npos && require_equals)
      continue;
    const std::string name = LowerAscii(PercentDecode(
        piece.substr(0, eq == std::string_view::npos ? piece.size() : eq),
        true));
    const std::string value =
        eq == std::string_view::npos
            ? std::string()
            : PercentDecode(piece.substr(eq + 1), true);
    if (!value.empty() && IsSecretParamName(name))
      return true;
    if (UrlValueLooksSecret(value) || UrlValueLooksSecret(name))
      return true;
  }
  return false;
}

bool IsLongToken(std::string_view t) {
  if (t.size() < 24)
    return false;
  bool letter = false, digit = false;
  for (char c : t) {
    if (!IsAlnum(c) && c != '-' && c != '_')
      return false;
    letter |= IsAlpha(c);
    digit |= IsDigit(c);
  }
  return letter && digit;
}

// ---- Building the draft ------------------------------------------------------

struct Builder {
  explicit Builder(const Run& r) : run(r) {}

  const Run& run;
  std::vector<std::string> goal_secrets;
  bool goal_unscanned = false;  // too long to scan: withhold every value
  std::set<std::string> variables;
  Script script;
  int withheld = 0;
  int secret_steps = 0;
  Refusal refusal = Refusal::kNone;

  bool Refuse(Refusal r) {
    if (refusal == Refusal::kNone)
      refusal = r;
    return false;
  }

  // A one-line string from outside the browser: valid UTF-8, no control
  // characters (|allow_breaks|: \n and \t), at most |max_chars| characters.
  static bool UsableText(std::string_view s,
                         size_t max_chars,
                         bool allow_breaks) {
    if (!IsValidUtf8(s) || CountChars(s) > max_chars)
      return false;
    for (char ch : s) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if ((c < 0x20 || c == 0x7F) &&
          !(allow_breaks && (c == '\n' || c == '\t'))) {
        return false;
      }
    }
    return true;
  }

  // A page address the draft opens: http(s), within its cap, without a
  // secret. Returns the canonical URL ("" after a refusal).
  std::string CheckAddress(std::string_view url) {
    if (url.size() > kMaxUrlBytes || !UsableText(url, kMaxUrlBytes, false)) {
      Refuse(Refusal::kUnusableStep);
      return std::string();
    }
    if (HasTemplateBraces(url)) {
      Refuse(Refusal::kTemplateText);
      return std::string();
    }
    const GURL g(url);
    if (!g.is_valid() || !agent_safety::IsAllowedNavigationURL(g) ||
        g.host().empty() || g.spec().size() > kMaxUrlBytes ||
        HasTemplateBraces(g.spec())) {
      Refuse(Refusal::kUnusableStep);
      return std::string();
    }
    if (UrlLooksSecret(url) || UrlLooksSecret(g.spec())) {
      Refuse(Refusal::kSecretInAddress);
      return std::string();
    }
    return g.spec();
  }

  static std::string HostOf(const std::string& spec) {
    return LowerAscii(GURL(spec).host());
  }

  static std::string Quoted(std::string_view shown) {
    std::string q(shown);
    std::replace(q.begin(), q.end(), '"', '\'');
    return "\xE2\x80\x9C" + q + "\xE2\x80\x9D";  // “...”
  }

  static std::string KindWord(const std::string& kind) {
    if (kind == "link")
      return "link";
    if (kind == "button")
      return "button";
    if (kind == "input")
      return "field";
    if (kind == "textarea")
      return "text box";
    if (kind == "select")
      return "list";
    if (kind == "checkbox")
      return "checkbox";
    if (kind == "radio")
      return "option";
    if (kind == "editable")
      return "text area";
    return "element";
  }

  bool ContainsGoalSecret(std::string_view text) const {
    for (const std::string& t : goal_secrets) {
      if (Contains(text, t))
        return true;
    }
    return false;
  }

  bool AddNavigation(std::string_view url, std::string description_suffix) {
    const std::string spec = CheckAddress(url);
    if (spec.empty())
      return false;
    Step s;
    s.type = StepType::NAVIGATE;
    s.target = spec;
    s.description = description_suffix.empty()
                        ? "Open " + HostOf(spec)
                        : std::move(description_suffix);
    script.steps.push_back(std::move(s));
    return true;
  }

  bool AddSearch(const RunStep& rs) {
    if (rs.query.empty() || !UsableText(rs.query, kMaxQueryChars, false))
      return Refuse(Refusal::kUnusableStep);
    if (HasTemplateBraces(rs.query))
      return Refuse(Refusal::kTemplateText);
    // A query that holds a secret would sit in the workflow's address.
    if (goal_unscanned || TextLooksSecret(rs.query) ||
        ContainsGoalSecret(rs.query)) {
      return Refuse(Refusal::kSecretInAddress);
    }
    return AddNavigation(rs.url, "Search the web for " +
                                     Quoted(CleanDisplayText(rs.query, 40)));
  }

  bool AddScroll(const RunStep& rs) {
    if (rs.direction != "down" && rs.direction != "up")
      return Refuse(Refusal::kUnusableStep);
    Step s;
    s.type = StepType::SCROLL;
    s.value = rs.direction;
    s.description = "Scroll " + rs.direction;
    script.steps.push_back(std::move(s));
    return true;
  }

  bool AddElementStep(const RunElement& e,
                      bool typing,
                      const std::string& host) {
    if (e.selector.empty() ||
        !UsableText(e.selector, kMaxSelectorBytes, false) ||
        e.selector.size() > kMaxSelectorBytes) {
      return Refuse(Refusal::kUnusableStep);
    }
    if (HasTemplateBraces(e.selector))
      return Refuse(Refusal::kTemplateText);
    // The label is the runner's text anchor and the step's description:
    // one clean line; dropped if a workflow would read it as a variable.
    std::string label = CleanDisplayText(e.label, kMaxLabelChars);
    if (HasTemplateBraces(label))
      label.clear();

    Step s;
    s.type = typing ? StepType::TYPE : StepType::CLICK;
    s.target = e.selector;
    base::DictValue extra;
    if (!label.empty())
      extra.Set("text", label);
    extra.Set(kStepHostKey, host);

    if (!typing) {
      s.description = label.empty() ? "Click the " + KindWord(e.kind)
                                    : "Click " + Quoted(label);
      s.extra = base::Value(std::move(extra));
      script.steps.push_back(std::move(s));
      return true;
    }

    if (!UsableText(e.text, kMaxTypedChars, true))
      return Refuse(Refusal::kUnusableStep);
    if (HasTemplateBraces(e.text))
      return Refuse(Refusal::kTemplateText);
    const agent_safety::FieldKind kind =
        LowerAscii(e.input_type) == "password"
            ? agent_safety::FieldKind::kPassword
            : e.field_kind;
    // The agent never types into a password field; a run that did isn't
    // one the browser made, and a draft never gets a saved-password step.
    if (kind == agent_safety::FieldKind::kPassword)
      return Refuse(Refusal::kUnusableStep);
    if (agent_safety::IsSensitive(kind)) {
      // A payment card or a one-time code: never stored, never replayed.
      extra.Set(kReviewKey, kReviewSecretField);
      s.extra = base::Value(std::move(extra));
      AutomationSecurity::MarkStepSecret(s, kind, host);
      s.description = "Enter the " +
                      std::string(agent_safety::FieldKindLabel(kind)) +
                      " yourself (not saved)";
      ++secret_steps;
      script.steps.push_back(std::move(s));
      return true;
    }
    const std::string check_label = StripHidden(e.label);
    if (goal_unscanned || LabelSuggestsSecret(check_label) ||
        TextLooksSecret(e.text) || ContainsGoalSecret(e.text)) {
      const std::string name = VariableNameFor(check_label, variables);
      variables.insert(name);
      script.default_variables[name] = std::string();
      s.value = "{{" + name + "}}";
      extra.Set(kReviewKey, kReviewWithheld);
      s.description = label.empty()
                          ? "Type a value you give at each run"
                          : "Type your " + Quoted(label) +
                                " (asked at each run)";
      ++withheld;
    } else {
      s.value = e.text;
      s.description = "Type " + Quoted(CleanDisplayText(e.text, 40)) +
                      (label.empty() ? std::string()
                                     : " in " + Quoted(label));
    }
    s.extra = base::Value(std::move(extra));
    script.steps.push_back(std::move(s));
    return true;
  }

  bool AddElementSteps(const RunStep& rs) {
    const bool fill = rs.action == "FILL_FORM";
    const bool typing = fill || rs.action == "TYPE";
    const size_t max = fill ? kMaxFormFields : 1;
    if (rs.elements.empty() || rs.elements.size() > max)
      return Refuse(Refusal::kUnusableStep);
    // The page acted on: an http(s) origin (it feeds the site list).
    const GURL origin(rs.origin);
    if (rs.origin.size() > kMaxUrlBytes || !origin.is_valid() ||
        !agent_safety::IsAllowedNavigationURL(origin) ||
        origin.host().empty()) {
      return Refuse(Refusal::kUnusableStep);
    }
    const std::string host = LowerAscii(origin.host());
    if (HasTemplateBraces(host) || !IsValidUtf8(host))
      return Refuse(Refusal::kUnusableStep);
    for (const RunElement& e : rs.elements) {
      if (!AddElementStep(e, typing, host))
        return false;
    }
    return true;
  }

  bool AddStep(const RunStep& rs) {
    const std::string& a = rs.action;
    if (a == "OBSERVE" || a == "DONE" || a == "ERROR" || a == "REPLAN")
      return true;  // nothing changed on a page
    if (a == "NAVIGATE")
      return AddNavigation(rs.url, std::string());
    if (a == "SEARCH")
      return AddSearch(rs);
    if (a == "CLICK" || a == "TYPE" || a == "FILL_FORM")
      return AddElementSteps(rs);
    if (a == "SCROLL")
      return AddScroll(rs);
    // CALL_TOOL is refused before; anything else isn't an action the
    // agent has.
    return Refuse(Refusal::kUnusableStep);
  }

  std::string NameFor() const {
    std::string first_host;
    for (const Step& s : script.steps) {
      if (s.type == StepType::NAVIGATE) {
        first_host = HostOf(s.target);
        break;
      }
    }
    const std::string goal = CleanDisplayText(run.goal, kMaxNameChars);
    const std::string check = StripHidden(run.goal);
    if (goal.empty() || goal_unscanned || !goal_secrets.empty() ||
        TextLooksSecret(check) || LabelSuggestsSecret(check)) {
      return first_host.empty() ? std::string("Agent run")
                                : "Agent run on " + first_host;
    }
    return goal;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------

bool LabelSuggestsSecret(std::string_view label) {
  const std::string lower = LowerAscii(label);
  constexpr std::string_view kSubstrings[] = {
      "password", "passwort", "passwd", "passcode", "contrase",
      "mot de passe", "senha", "wachtwoord",
      "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89",  // パスワード
      "\xE5\xAF\x86\xE7\xA0\x81"};                                     // 密码
  for (std::string_view s : kSubstrings) {
    if (Contains(lower, s))
      return true;
  }
  const std::vector<std::string> words = Words(label);
  const std::string joined = JoinWords(words);
  return HasWordOrPhrase(
      joined, words,
      {"pwd", "passphrase", "pin", "pincode", "cvv", "cvv2", "cvc", "cvc2",
       "csc", "otp", "totp", "2fa", "mfa", "ssn", "secret", "token", "apikey",
       "iban"},
      {"security code", "verification code", "one time", "auth code",
       "authentication code", "access code", "sms code", "login code",
       "api key", "private key", "secret key", "card number", "credit card",
       "debit card", "card no", "account number", "routing number",
       "sort code", "social security", "tax id", "seed phrase",
       "recovery phrase", "recovery code", "backup code", "expiry date",
       "expiration date", "security question", "security answer"});
}

bool TextLooksSecret(std::string_view text) {
  if (HasCardNumber(text) || LooksLikeOneTimeCode(text))
    return true;
  // An IBAN written in groups: "DE89 3704 0044 0532 0130 00".
  std::string compact;
  for (char c : text) {
    if (c != ' ')
      compact.push_back(c);
  }
  if (IsIbanShaped(compact))
    return true;
  const std::string lower = LowerAscii(text);
  if (Contains(lower, "password") || Contains(lower, "passwd") ||
      Contains(lower, "passcode") || Contains(lower, "passphrase")) {
    return true;
  }
  const std::vector<std::string> words = Words(text);
  if (HasWordOrPhrase(JoinWords(words), words,
                      {"pwd", "otp", "cvv", "cvc", "ssn"},
                      {"api key", "secret key", "private key", "seed phrase",
                       "recovery phrase", "security code",
                       "verification code"})) {
    return true;
  }
  for (std::string_view tok : SplitTokens(text)) {
    if (TokenLooksSecret(TrimPunct(tok)))
      return true;
  }
  return false;
}

std::vector<std::string> GoalSecretTokens(std::string_view goal) {
  if (goal.size() > kMaxGoalScanBytes)
    goal = goal.substr(0, kMaxGoalScanBytes);
  std::vector<std::string_view> tokens;
  for (std::string_view t : SplitTokens(goal, "=:")) {
    t = TrimPunct(t);
    if (!t.empty())
      tokens.push_back(t);
  }
  // After these the next word is the secret ("password is hunter2").
  constexpr std::string_view kStrongKeywords[] = {
      "password", "passwd", "pwd", "passcode", "passphrase", "secret",
      "token"};
  // After these only a word with a digit in it ("code SAVE20", "PIN
  // 1234"): "zip code for Paris" gives nothing.
  constexpr std::string_view kNumericKeywords[] = {
      "pin", "code", "otp", "cvv", "cvc", "ssn", "key", "card"};
  constexpr std::string_view kSkip[] = {
      "is",   "was", "will", "be",    "my",     "the",  "a",     "an",
      "of",   "with", "as",  "to",    "number", "no",   "num",   "#",
      "and",  "its", "it's", "it",    "use",    "using", "enter", "type",
      "->",   "for", "are",  "here",  "this",   "that", "your",  "our"};
  std::vector<std::string> out;
  auto add = [&out](std::string_view t) {
    if (t.size() < 3)
      return;
    for (const std::string& o : out) {
      if (o == t)
        return;
    }
    out.emplace_back(t);
  };
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string lower = LowerAscii(tokens[i]);
    bool strong = false;
    bool numeric = false;
    for (std::string_view k : kStrongKeywords)
      strong |= lower == k;
    for (std::string_view k : kNumericKeywords)
      numeric |= lower == k;
    if (strong || numeric) {
      for (size_t j = i + 1; j < tokens.size() && j <= i + 4; ++j) {
        const std::string next = LowerAscii(tokens[j]);
        bool skip = false;
        for (std::string_view s : kSkip)
          skip |= next == s;
        if (skip)
          continue;
        if (strong || std::any_of(next.begin(), next.end(), IsDigit))
          add(tokens[j]);
        break;
      }
    }
    if (TokenLooksSecret(tokens[i]))
      add(tokens[i]);
  }
  return out;
}

bool UrlLooksSecret(std::string_view url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos)
    return false;
  std::string_view rest = url.substr(scheme_end + 3);
  const size_t authority_end = rest.find_first_of("/?#");
  const std::string_view authority = rest.substr(
      0, authority_end == std::string_view::npos ? rest.size()
                                                 : authority_end);
  if (authority.find('@') != std::string_view::npos)
    return true;  // user:password@host
  rest = authority_end == std::string_view::npos
             ? std::string_view()
             : rest.substr(authority_end);
  const size_t hash = rest.find('#');
  const std::string_view fragment =
      hash == std::string_view::npos ? std::string_view()
                                     : rest.substr(hash + 1);
  const std::string_view before_hash = rest.substr(
      0, hash == std::string_view::npos ? rest.size() : hash);
  const size_t question = before_hash.find('?');
  const std::string_view query = question == std::string_view::npos
                                     ? std::string_view()
                                     : before_hash.substr(question + 1);
  const std::string_view path = before_hash.substr(
      0, question == std::string_view::npos ? before_hash.size() : question);

  if (ParamsLookSecret(query, /*require_equals=*/false) ||
      ParamsLookSecret(fragment, /*require_equals=*/true)) {
    return true;
  }
  const std::string lower_path = LowerAscii(PercentDecode(path, false));
  bool sign_in_path = false;
  for (std::string_view w :
       {"reset", "password", "passwd", "login", "signin", "sign-in", "verify",
        "confirm", "magic", "token", "auth", "activate", "invite", "otp"}) {
    sign_in_path |= Contains(lower_path, w);
  }
  for (std::string_view segment : SplitTokens(path, "/")) {
    const std::string decoded = PercentDecode(segment, false);
    if (UrlValueLooksSecret(decoded))
      return true;
    if (sign_in_path && IsLongToken(decoded))
      return true;
  }
  return false;
}

std::string CleanDisplayText(std::string_view text, size_t max_chars) {
  std::vector<uint32_t> cps;
  bool pending_space = false;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = 0;
    size_t n = DecodeUtf8At(text, i, &cp);
    if (!n) {
      cp = kReplacementChar;
      n = 1;
    }
    i += n;
    if (IsControl(cp) || cp == ' ') {
      pending_space = !cps.empty();
      continue;
    }
    if (IsHidden(cp))
      cp = kReplacementChar;
    if (pending_space)
      cps.push_back(' ');
    pending_space = false;
    cps.push_back(cp);
  }
  if (max_chars == 0)
    return std::string();
  if (cps.size() > max_chars) {
    cps.resize(max_chars - 1);
    while (!cps.empty() && cps.back() == ' ')
      cps.pop_back();
    cps.push_back(kEllipsis);
  }
  std::string out;
  out.reserve(cps.size());
  for (uint32_t cp : cps)
    AppendUtf8(&out, cp);
  return out;
}

std::string VariableNameFor(std::string_view label,
                            const std::set<std::string>& taken) {
  std::string slug;
  for (char c : label) {
    if (IsAlnum(c)) {
      slug.push_back(IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c);
    } else if (!slug.empty() && slug.back() != '_') {
      slug.push_back('_');
    }
  }
  while (!slug.empty() && slug.back() == '_')
    slug.pop_back();
  if (slug.empty())
    slug = "value";
  else if (!IsLower(slug[0]))
    slug = "value_" + slug;
  auto fit = [](std::string s, size_t max) {
    if (s.size() > max)
      s.resize(max);
    while (!s.empty() && s.back() == '_')
      s.pop_back();
    return s;
  };
  slug = fit(slug, kMaxVariableNameChars);
  if (!taken.count(slug))
    return slug;
  for (size_t n = 2;; ++n) {
    const std::string suffix = "_" + base::NumberToString(n);
    const std::string candidate =
        fit(slug, kMaxVariableNameChars - suffix.size()) + suffix;
    if (!taken.count(candidate))
      return candidate;
  }
}

std::string_view RefusalText(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone:
    case Refusal::kNotSucceeded:
      return "";
    case Refusal::kMoltNet:
      return "Saving agent runs as workflows isn't available in MoltNet "
             "windows: a workflow would run later outside Tor.";
    case Refusal::kUsedSiteTool:
      return "This run used a site's own tool, which a workflow can't "
             "replay, so it can't be saved as a workflow.";
    case Refusal::kNothingToSave:
      return "The agent didn't open, click or type anything, so there is "
             "nothing to save as a workflow.";
    case Refusal::kUnusableStep:
      return "A step of this run can't be replayed as it ran, so it can't "
             "be saved as a workflow.";
    case Refusal::kSecretInAddress:
      return "A page this run opened, or a search it made, looks like it "
             "holds a password, code or key, so it can't be saved as a "
             "workflow.";
    case Refusal::kTemplateText:
      return "This run typed or acted on text with \"{{\" in it, which a "
             "workflow would read as a variable, so it can't be saved.";
  }
  return "";
}

Draft BuildDraft(const Run& run,
                 bool saved_workflows_allowed,
                 int64_t now_unix) {
  Draft draft;
  auto refuse = [&draft](Refusal r) {
    draft.refusal = r;
    draft.message = std::string(RefusalText(r));
    return std::move(draft);
  };
  if (!run.succeeded)
    return refuse(Refusal::kNotSucceeded);
  if (run.in_moltnet || !saved_workflows_allowed)
    return refuse(Refusal::kMoltNet);
  if (run.steps.size() > kMaxDraftSteps)
    return refuse(Refusal::kUnusableStep);
  for (const RunStep& rs : run.steps) {
    if (rs.success && rs.action == "CALL_TOOL")
      return refuse(Refusal::kUsedSiteTool);
  }

  Builder b(run);
  b.goal_unscanned = run.goal.size() > kMaxGoalScanBytes;
  b.goal_secrets = GoalSecretTokens(run.goal);
  for (const RunStep& rs : run.steps) {
    if (!rs.success)
      continue;  // only what ran and worked
    if (!b.AddStep(rs))
      return refuse(b.refusal);
  }
  if (b.script.steps.empty())
    return refuse(Refusal::kNothingToSave);

  // Start where the agent started, unless it began by opening a page.
  if (b.script.steps.front().type != StepType::NAVIGATE) {
    std::vector<Step> rest = std::move(b.script.steps);
    b.script.steps.clear();
    if (!b.AddNavigation(run.start_url, std::string()))
      return refuse(b.refusal);
    for (Step& s : rest)
      b.script.steps.push_back(std::move(s));
  }
  if (b.script.steps.size() > kMaxDraftSteps)
    return refuse(Refusal::kUnusableStep);

  Script& s = b.script;
  s.id.clear();
  s.name = b.NameFor();
  s.version = 1;
  s.created_at_unix = now_unix;
  s.trigger = Trigger();  // manual: a draft is never scheduled
  s.stats = Stats();
  // Model-made: CASUAL, no site until the user confirms them.
  s.security = SecurityPolicy();
  s.security.trust = TrustLevel::CASUAL;
  s.security.domain_whitelist.clear();
  s.security.require_approval_for = {"form_submit", "payment", "login"};
  s.security.sites_unconfirmed = true;

  draft.withheld = b.withheld;
  draft.secret_steps = b.secret_steps;
  draft.message =
      "Saved as a draft workflow. Review its steps and confirm its sites "
      "in Agent mode before it can run.";
  if (b.withheld > 0) {
    draft.message += " It will ask you at each run for " +
                     base::NumberToString(b.withheld) +
                     (b.withheld == 1 ? " value" : " values") +
                     " the agent typed that looked private; " +
                     (b.withheld == 1 ? "it wasn't" : "they weren't") +
                     " saved.";
  }
  if (b.secret_steps > 0) {
    draft.message += " " + base::NumberToString(b.secret_steps) +
                     (b.secret_steps == 1 ? " step enters" : " steps enter") +
                     " a card number or one-time code, which MoltBrowser "
                     "never saves: you enter it yourself.";
  }
  draft.script = std::move(b.script);
  return draft;
}

}  // namespace agent_draft
}  // namespace automation
}  // namespace molt_ai
