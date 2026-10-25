// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_locators.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace locators {
namespace {

using agent_safety::FieldKind;

constexpr char kOrigin[] = "https://shop.example";

base::Value Json(const std::string& json) {
  std::optional<base::Value> v =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  EXPECT_TRUE(v.has_value()) << json;
  return v ? std::move(*v) : base::Value();
}

std::string Write(const base::Value& v) {
  std::string out;
  base::JSONWriter::Write(v, &out);
  return out;
}

Locator L(By by, std::string value, std::string extra = std::string()) {
  Locator l;
  l.by = by;
  l.value = std::move(value);
  if (by == By::kRole)
    l.role = std::move(extra);
  if (by == By::kAttribute)
    l.name = std::move(extra);
  return l;
}

StepLocators Button() {
  StepLocators s;
  s.origin = kOrigin;
  s.element = {"button", "", "button"};
  s.list = {L(By::kRole, "Add to cart", "button"),
            L(By::kAttribute, "add-btn", "data-testid"),
            L(By::kText, "Add to cart"),
            L(By::kPath, "#main > div:nth-of-type(2) > button:nth-of-type(1)")};
  return s;
}

// A stored block with |list| as its "list".
std::string Block(const std::string& list,
                  const std::string& element =
                      "{\"tag\":\"button\",\"type\":\"\",\"role\":\"button\"}",
                  const std::string& origin = "\"https://shop.example\"",
                  const std::string& field = std::string()) {
  return "{\"origin\":" + origin + ",\"element\":" + element +
         (field.empty() ? std::string() : ",\"field\":" + field) +
         ",\"list\":" + list + "}";
}

const char kGoodEntry[] = "{\"by\":\"text\",\"value\":\"Go\"}";

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

TEST(AutomationLocatorsTest, TypeClasses) {
  EXPECT_EQ(TypeClass("button", "submit"), "");
  EXPECT_EQ(TypeClass("textarea", ""), "");
  for (const char* t : {"", "text", "search", "email", "url", "tel", "number"})
    EXPECT_EQ(TypeClass("input", t), "text") << t;
  for (const char* t : {"submit", "button", "reset", "image"})
    EXPECT_EQ(TypeClass("input", t), "button") << t;
  for (const char* t : {"password", "checkbox", "radio", "date", "file"})
    EXPECT_EQ(TypeClass("input", t), t) << t;
  EXPECT_EQ(TypeClass("input", "x\"y"), "text");
  EXPECT_EQ(TypeClass("input", std::string(30, 'a')), "text");
}

TEST(AutomationLocatorsTest, KindsMatchExactly) {
  const ElementKind button{"button", "", "button"};
  EXPECT_TRUE(KindMatches(button, {"button", "", "button"}));
  EXPECT_FALSE(KindMatches(button, {"a", "", "link"}));
  EXPECT_FALSE(KindMatches(button, {"button", "", "link"}));  // role changed
  EXPECT_FALSE(KindMatches(button, {"div", "", "button"}));
  const ElementKind email{"input", "text", "textbox"};
  EXPECT_TRUE(KindMatches(email, {"input", "text", "textbox"}));
  EXPECT_FALSE(KindMatches(email, {"input", "password", ""}));
  EXPECT_FALSE(KindMatches(email, {"input", "checkbox", "checkbox"}));
  EXPECT_FALSE(KindMatches(email, {"textarea", "", "textbox"}));
}

TEST(AutomationLocatorsTest, Strength) {
  EXPECT_TRUE(IsStrong(L(By::kRole, "Go", "button")));
  EXPECT_TRUE(IsStrong(L(By::kLabel, "Email")));
  EXPECT_TRUE(IsStrong(L(By::kSelector, "#go")));
  for (std::string_view a : kStrongAttributes)
    EXPECT_TRUE(IsStrong(L(By::kAttribute, "v", std::string(a)))) << a;
  for (std::string_view a : kWeakAttributes)
    EXPECT_FALSE(IsStrong(L(By::kAttribute, "v", std::string(a)))) << a;
  EXPECT_FALSE(IsStrong(L(By::kAttribute, "v", "onclick")));
  EXPECT_FALSE(IsStrong(L(By::kText, "Go")));
  EXPECT_FALSE(IsStrong(L(By::kPath, "html > body:nth-of-type(1)")));
  EXPECT_EQ(Describe(L(By::kAttribute, "v", "data-testid")), "its data-testid");
  EXPECT_EQ(Describe(L(By::kAttribute, "v", "onclick")), "an attribute");
  EXPECT_EQ(Describe(L(By::kPath, "x")), "its position on the page");
}

// ---------------------------------------------------------------------------
// The stored form
// ---------------------------------------------------------------------------

TEST(AutomationLocatorsTest, RoundTrips) {
  StepLocators s = Button();
  s.field = FieldKind::kOrdinary;
  std::optional<StepLocators> back = Parse(ToValue(s));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, s);
  // Through JSON text as well, and a password field's block.
  s.element = {"input", "password", ""};
  s.field = FieldKind::kPassword;
  s.list = {L(By::kLabel, "Password"), L(By::kAttribute, "pw", "name")};
  back = Parse(Json(Write(ToValue(s))));
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, s);
}

TEST(AutomationLocatorsTest, RefusesMalformedBlocks) {
  const std::string list = std::string("[") + kGoodEntry + "]";
  EXPECT_TRUE(Parse(Json(Block(list))));
  EXPECT_FALSE(Parse(base::Value()));
  EXPECT_FALSE(Parse(Json("[]")));
  EXPECT_FALSE(Parse(Json("\"x\"")));
  // Origins: an http(s) origin, exactly.
  for (const char* origin :
       {"\"https://shop.example/\"", "\"https://shop.example/a\"",
        "\"https://user@shop.example\"", "\"ftp://shop.example\"",
        "\"javascript:alert(1)\"", "\"null\"", "\"\"", "1",
        "\"https://shop.example:443\""}) {
    EXPECT_FALSE(Parse(Json(Block(list, "{\"tag\":\"a\",\"type\":\"\",\"role\":"
                                        "\"link\"}",
                                  origin))))
        << origin;
  }
  EXPECT_TRUE(Parse(Json(Block(list, "{\"tag\":\"a\",\"type\":\"\",\"role\":"
                                     "\"link\"}",
                               "\"http://shop.example:8080\""))));
  const std::string long_origin =
      "\"https://" + std::string(kMaxOriginBytes, 'a') + ".example\"";
  EXPECT_FALSE(Parse(Json(Block(list, "{\"tag\":\"a\",\"type\":\"\",\"role\":"
                                      "\"link\"}",
                                long_origin))));
  // Element kinds.
  for (const char* element : {
           "{\"tag\":\"BUTTON\",\"type\":\"\",\"role\":\"button\"}",
           "{\"tag\":\"\",\"type\":\"\",\"role\":\"\"}",
           "{\"tag\":\"input\",\"type\":\"\",\"role\":\"textbox\"}",
           "{\"tag\":\"button\",\"type\":\"text\",\"role\":\"button\"}",
           "{\"tag\":\"button\",\"type\":\"\",\"role\":\"Button\"}",
           "{\"tag\":\"button\",\"type\":\"\",\"role\":\"b1\"}",
           "{\"tag\":\"button\",\"type\":\"\"}",
           "{\"tag\":\"button\",\"type\":\"\",\"role\":\"button\",\"x\":1}",
           "{\"tag\":\"1a\",\"type\":\"\",\"role\":\"\"}",
       }) {
    EXPECT_FALSE(Parse(Json(Block(list, element)))) << element;
  }
  // Field kinds the browser writes, only.
  EXPECT_TRUE(Parse(Json(Block(list, "{\"tag\":\"input\",\"type\":\"password\","
                                     "\"role\":\"\"}",
                               "\"https://shop.example\"", "\"password\""))));
  EXPECT_FALSE(Parse(Json(Block(list, "{\"tag\":\"button\",\"type\":\"\","
                                      "\"role\":\"button\"}",
                                "\"https://shop.example\"", "\"pin\""))));
  for (const char* field : {"\"\"", "\"ordinary\"", "1", "null"}) {
    EXPECT_FALSE(Parse(Json(Block(list, "{\"tag\":\"button\",\"type\":\"\","
                                        "\"role\":\"button\"}",
                                  "\"https://shop.example\"", field))))
        << field;
  }
  // A list of nothing usable.
  EXPECT_FALSE(Parse(Json(Block("[]"))));
  EXPECT_FALSE(Parse(Json(Block("{}"))));
  EXPECT_FALSE(Parse(Json(Block("[{\"by\":\"css\",\"value\":\"#x\"}]"))));
}

TEST(AutomationLocatorsTest, DropsBadEntriesKeepsGoodOnes) {
  const std::string long_text(kMaxLocatorTextChars + 1, 'a');
  const std::string max_text(kMaxLocatorTextChars, 'a');
  const std::string entries[] = {
      "1",
      "{\"by\":\"text\"}",
      "{\"by\":\"text\",\"value\":\"\"}",
      "{\"by\":\"text\",\"value\":\"" + long_text + "\"}",
      "{\"by\":\"text\",\"value\":\"a\\nb\"}",
      "{\"by\":\"text\",\"value\":\"a\\u0085b\"}",
      "{\"by\":\"text\",\"value\":\"a\\u007fb\"}",
      "{\"by\":\"text\",\"value\":\"{{pin}}\"}",
      "{\"by\":\"text\",\"value\":\"a\",\"role\":\"button\"}",
      "{\"by\":\"selector\",\"value\":\"#x\"}",
      "{\"by\":\"role\",\"value\":\"Go\"}",
      "{\"by\":\"role\",\"role\":\"Button\",\"value\":\"Go\"}",
      "{\"by\":\"role\",\"role\":\"" + std::string(33, 'a') +
          "\",\"value\":\"Go\"}",
      "{\"by\":\"attr\",\"name\":\"onclick\",\"value\":\"x\"}",
      "{\"by\":\"attr\",\"name\":\"href\",\"value\":\"/reset?token=1\"}",
      "{\"by\":\"attr\",\"value\":\"x\"}",
      "{\"by\":\"path\",\"value\":\"div > button\"}",
      "{\"by\":\"path\",\"value\":\"html" +
          std::string(kMaxPathBytes, ' ') + "\"}",
      "{\"by\":\"label\",\"value\":1}",
      // Good ones; the second is the first again.
      "{\"by\":\"text\",\"value\":\"" + max_text + "\"}",
      "{\"by\":\"text\",\"value\":\"" + max_text + "\"}",
      "{\"by\":\"role\",\"role\":\"button\",\"value\":\"Go\"}",
      "{\"by\":\"attr\",\"name\":\"data-testid\",\"value\":\"go\"}",
      "{\"by\":\"path\",\"value\":\"#main > button:nth-of-type(2)\"}",
  };
  std::string list = "[";
  for (const std::string& e : entries)
    list += (list.size() > 1 ? "," : "") + e;
  list += "]";
  std::optional<StepLocators> s = Parse(Json(Block(list)));
  ASSERT_TRUE(s);
  ASSERT_EQ(s->list.size(), 4u);
  EXPECT_EQ(s->list[0].value, max_text);
  EXPECT_EQ(s->list[1].role, "button");
  EXPECT_EQ(s->list[2].name, "data-testid");
  EXPECT_EQ(s->list[3].by, By::kPath);
}

TEST(AutomationLocatorsTest, CapsTheList) {
  // At most kMaxLocators kept, and only the start of a long list is read.
  std::string list = "[";
  for (int i = 0; i < 20; ++i) {
    list += std::string(i ? "," : "") + "{\"by\":\"text\",\"value\":\"t" +
            std::to_string(i) + "\"}";
  }
  list += "]";
  std::optional<StepLocators> s = Parse(Json(Block(list)));
  ASSERT_TRUE(s);
  EXPECT_EQ(s->list.size(), kMaxLocators);
  EXPECT_EQ(s->list.back().value, "t7");

  std::string junk = "[";
  for (size_t i = 0; i < 4 * kMaxLocators; ++i)
    junk += std::string(i ? "," : "") + "1";
  junk += std::string(",") + kGoodEntry + "]";
  EXPECT_FALSE(Parse(Json(Block(junk))));
}

TEST(AutomationLocatorsTest, Paths) {
  auto path = [](const std::string& p) {
    return Parse(Json(Block("[{\"by\":\"path\",\"value\":" +
                            Write(base::Value(p)) + "}]")))
        .has_value();
  };
  EXPECT_TRUE(path("html"));
  EXPECT_TRUE(path("#main > a:nth-of-type(1)"));
  std::string twelve = "html";
  for (int i = 0; i < 11; ++i)
    twelve += " > div:nth-of-type(1)";
  EXPECT_TRUE(path(twelve));
  EXPECT_FALSE(path(twelve + " > a:nth-of-type(1)"));
  EXPECT_FALSE(path("body > a"));
  EXPECT_FALSE(path("a, html"));
  EXPECT_FALSE(path("#" + std::string(kMaxPathBytes, 'a')));
}

TEST(AutomationLocatorsTest, ReadsCaptureReports) {
  const std::string report =
      "{\"origin\":\"https://shop.example\",\"tag\":\"input\","
      "\"type\":\"text\",\"role\":\"textbox\",\"input_type\":\"email\","
      "\"autocomplete\":\"email\",\"list\":[{\"by\":\"label\",\"value\":"
      "\"Email\"},{\"by\":\"attr\",\"name\":\"name\",\"value\":\"email\"}]}";
  std::optional<StepLocators> s = FromCaptureReport(Json(report));
  ASSERT_TRUE(s);
  EXPECT_EQ(s->origin, kOrigin);
  EXPECT_EQ(s->element, (ElementKind{"input", "text", "textbox"}));
  EXPECT_EQ(s->field, FieldKind::kOrdinary);
  EXPECT_EQ(s->list.size(), 2u);

  // The field kind is the browser's, from the reported type and
  // autocomplete: a password, card or one-time-code field says so.
  auto kind_of = [](const std::string& type, const std::string& input_type,
                    const std::string& autocomplete) {
    std::optional<StepLocators> r = FromCaptureReport(Json(
        "{\"origin\":\"https://shop.example\",\"tag\":\"input\",\"type\":\"" +
        type + "\",\"role\":\"\",\"input_type\":\"" + input_type +
        "\",\"autocomplete\":\"" + autocomplete +
        "\",\"list\":[{\"by\":\"label\",\"value\":\"X\"}]}"));
    return r ? r->field : FieldKind::kOrdinary;
  };
  EXPECT_EQ(kind_of("password", "password", ""), FieldKind::kPassword);
  EXPECT_EQ(kind_of("text", "text", "current-password"), FieldKind::kPassword);
  EXPECT_EQ(kind_of("text", "text", "cc-number"), FieldKind::kPaymentCard);
  EXPECT_EQ(kind_of("text", "text", "one-time-code"),
            FieldKind::kOneTimeCode);

  // The type class must be the browser's own for the reported type.
  EXPECT_FALSE(FromCaptureReport(Json(
      "{\"origin\":\"https://shop.example\",\"tag\":\"input\",\"type\":"
      "\"text\",\"role\":\"\",\"input_type\":\"password\",\"autocomplete\":"
      "\"\",\"list\":[{\"by\":\"label\",\"value\":\"X\"}]}")));
  EXPECT_FALSE(FromCaptureReport(base::Value()));
  EXPECT_FALSE(FromCaptureReport(Json(
      "{\"origin\":\"file:///x\",\"tag\":\"a\",\"type\":\"\",\"role\":"
      "\"link\",\"input_type\":\"\",\"autocomplete\":\"\",\"list\":[" +
      std::string(kGoodEntry) + "]}")));
  EXPECT_FALSE(FromCaptureReport(Json(
      "{\"origin\":\"https://shop.example\",\"tag\":\"a\",\"type\":\"\","
      "\"role\":\"link\",\"input_type\":\"\",\"autocomplete\":\"" +
      std::string(kMaxAutocompleteChars + 2, 'a') + "\",\"list\":[" +
      std::string(kGoodEntry) + "]}")));
}

TEST(AutomationLocatorsTest, ReadsCaptureLists) {
  const std::string field =
      "{\"origin\":\"https://shop.example\",\"tag\":\"input\",\"type\":"
      "\"text\",\"role\":\"textbox\",\"input_type\":\"text\","
      "\"autocomplete\":\"\",\"list\":[{\"by\":\"label\",\"value\":\"Name\"}]}";
  const std::string elsewhere =
      "{\"origin\":\"https://evil.example\",\"tag\":\"input\",\"type\":"
      "\"text\",\"role\":\"textbox\",\"input_type\":\"text\","
      "\"autocomplete\":\"\",\"list\":[{\"by\":\"label\",\"value\":\"Name\"}]}";
  std::vector<std::optional<StepLocators>> got = ParseCaptureList(
      Json("[" + field + ",null," + elsewhere + ",7]"), 4, kOrigin);
  ASSERT_EQ(got.size(), 4u);
  EXPECT_TRUE(got[0]);
  EXPECT_FALSE(got[1]);
  EXPECT_FALSE(got[2]) << "captured on another origin";
  EXPECT_FALSE(got[3]);
  // Another count than asked: nothing.
  got = ParseCaptureList(Json("[" + field + "]"), 2, kOrigin);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_FALSE(got[0]);
  got = ParseCaptureList(Json("{}"), 1, kOrigin);
  EXPECT_FALSE(got[0]);
  // The script carries the selectors as JSON.
  const std::string script =
      BuildCaptureListScript({"#a", "\"</script><b>"});
  EXPECT_NE(script.find("return [\"#a\",\"\\\"\\u003C/script>"),
            std::string::npos);
  EXPECT_EQ(script.find("</script>"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The probe and the decision
// ---------------------------------------------------------------------------

Candidate Cand(ElementKind kind,
               std::string label,
               FieldKind field = FieldKind::kOrdinary) {
  Candidate c;
  c.kind = std::move(kind);
  c.label = std::move(label);
  c.field = field;
  return c;
}

Probe MakeProbe(std::vector<Candidate> candidates,
                std::vector<std::vector<size_t>> matches) {
  Probe p;
  p.origin = kOrigin;
  p.candidates = std::move(candidates);
  p.overflow.assign(matches.size(), false);
  p.matches = std::move(matches);
  return p;
}

const ElementKind kButton{"button", "", "button"};
const ElementKind kLink{"a", "", "link"};

TEST(AutomationLocatorsTest, TheRecordedSelectorFirst) {
  const std::vector<Locator> order = ProbeOrder("#add", Button());
  ASSERT_EQ(order.size(), 5u);
  EXPECT_EQ(order[0].by, By::kSelector);
  const Probe p = MakeProbe({Cand(kButton, "Add to cart")},
                            {{0}, {0}, {0}, {0}, {0}});
  const Decision d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kStrong);
  EXPECT_EQ(d.locator, 0u);
  EXPECT_EQ(d.candidate, 0u);
  EXPECT_EQ(ProbeOrder("", Button()).size(), 4u);
}

TEST(AutomationLocatorsTest, FallsBackInOrder) {
  const std::vector<Locator> order = ProbeOrder("#add", Button());
  // The selector found nothing; the role and name did.
  Probe p = MakeProbe({Cand(kButton, "Add to cart")}, {{}, {0}, {}, {0}, {}});
  Decision d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kStrong);
  EXPECT_EQ(d.locator, 1u);
  // Only the data-testid.
  p = MakeProbe({Cand(kButton, "Add")}, {{}, {}, {0}, {}, {}});
  d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kStrong);
  EXPECT_EQ(d.locator, 2u);
  // Only weak ones: the text, or the path. The user is asked.
  p = MakeProbe({Cand(kButton, "Add to cart")}, {{}, {}, {}, {0}, {0}});
  d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kWeak);
  EXPECT_EQ(d.locator, 3u);
  p = MakeProbe({Cand(kButton, "Buy")}, {{}, {}, {}, {}, {0}});
  d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kWeak);
  EXPECT_EQ(d.locator, 4u);
  // Nothing.
  p = MakeProbe({}, {{}, {}, {}, {}, {}});
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
}

TEST(AutomationLocatorsTest, StrongLocatorsThatDisagreeAsk) {
  const std::vector<Locator> order = ProbeOrder("", Button());
  Probe p = MakeProbe({Cand(kButton, "Add to cart"), Cand(kButton, "Delete")},
                      {{0}, {1}, {0}, {0}});
  Decision d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kConflict);
  EXPECT_EQ(d.candidate, 0u);
  // A weak locator pointing elsewhere doesn't overrule a strong one.
  p = MakeProbe({Cand(kButton, "Add to cart"), Cand(kButton, "Delete")},
                {{0}, {0}, {1}, {1}});
  d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kStrong);
  EXPECT_EQ(d.candidate, 0u);
}

TEST(AutomationLocatorsTest, NeverAnotherKind) {
  const std::vector<Locator> order = ProbeOrder("#add", Button());
  // Every locator finds a link: nothing is chosen.
  Probe p = MakeProbe({Cand(kLink, "Add to cart")}, {{0}, {0}, {0}, {0}, {0}});
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
  // A link and a button with the same text: the button, the only one of
  // its kind.
  p = MakeProbe({Cand(kLink, "Add to cart"), Cand(kButton, "Add to cart")},
                {{}, {}, {}, {0, 1}, {}});
  Decision d = Decide(order, p, Button(), kOrigin);
  EXPECT_EQ(d.strength, Strength::kWeak);
  EXPECT_EQ(d.candidate, 1u);
  // Two buttons: the locator identifies nothing.
  p = MakeProbe({Cand(kButton, "Add to cart"), Cand(kButton, "Add to cart")},
                {{}, {0, 1}, {}, {0, 1}, {}});
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
  // ... nor does one that found too many to report.
  p = MakeProbe({Cand(kButton, "Add to cart")}, {{}, {0}, {}, {}, {}});
  p.overflow[1] = true;
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
}

TEST(AutomationLocatorsTest, CredentialFieldsOnlyForCredentialSteps) {
  StepLocators email;
  email.origin = kOrigin;
  email.element = {"input", "text", "textbox"};
  email.list = {L(By::kLabel, "Email")};
  const std::vector<Locator> order = ProbeOrder("", email);
  const ElementKind text{"input", "text", "textbox"};
  // A text field that is now a one-time-code or card field: never, for a
  // step on an ordinary field.
  for (FieldKind f : {FieldKind::kOneTimeCode, FieldKind::kPaymentCard,
                      FieldKind::kPassword}) {
    Probe p = MakeProbe({Cand(text, "Email", f)}, {{0}});
    EXPECT_EQ(Decide(order, p, email, kOrigin).strength, Strength::kNone);
  }
  // A password step: only a password field.
  StepLocators pw;
  pw.origin = kOrigin;
  pw.element = {"input", "password", ""};
  pw.field = FieldKind::kPassword;
  pw.list = {L(By::kLabel, "Password")};
  const ElementKind pw_kind{"input", "password", ""};
  Probe p = MakeProbe({Cand(pw_kind, "Password", FieldKind::kPassword)}, {{0}});
  EXPECT_EQ(Decide(ProbeOrder("", pw), p, pw, kOrigin).strength,
            Strength::kStrong);
  p = MakeProbe({Cand(text, "Password", FieldKind::kOrdinary)}, {{0}});
  EXPECT_EQ(Decide(ProbeOrder("", pw), p, pw, kOrigin).strength,
            Strength::kNone);
  // A card field is not a password field.
  StepLocators card = pw;
  card.field = FieldKind::kPaymentCard;
  p = MakeProbe({Cand(pw_kind, "Password", FieldKind::kPassword)}, {{0}});
  EXPECT_EQ(Decide(ProbeOrder("", card), p, card, kOrigin).strength,
            Strength::kNone);
}

TEST(AutomationLocatorsTest, NeverAnotherOrigin) {
  const std::vector<Locator> order = ProbeOrder("", Button());
  Probe p = MakeProbe({Cand(kButton, "Add to cart")}, {{0}, {0}, {0}, {0}});
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kStrong);
  EXPECT_EQ(Decide(order, p, Button(), "https://evil.example").strength,
            Strength::kNone);
  EXPECT_EQ(Decide(order, p, Button(), "https://www.shop.example").strength,
            Strength::kNone);
  p.origin = "https://evil.example";
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
  // A probe of another shape decides nothing.
  p = MakeProbe({Cand(kButton, "x")}, {{0}});
  EXPECT_EQ(Decide(order, p, Button(), kOrigin).strength, Strength::kNone);
}

TEST(AutomationLocatorsTest, ParsesProbes) {
  const std::string candidate =
      "{\"tag\":\"button\",\"type\":\"\",\"role\":\"button\",\"input_type\":"
      "\"\",\"autocomplete\":\"\",\"label\":\"Add \\u202eto\\n cart\"}";
  std::optional<Probe> p = ParseProbe(
      Json("{\"origin\":\"https://shop.example\",\"candidates\":[" +
           candidate + "],\"matches\":[[0],[]],\"over\":[false,true]}"),
      2);
  ASSERT_TRUE(p);
  ASSERT_EQ(p->candidates.size(), 1u);
  EXPECT_EQ(p->candidates[0].kind, kButton);
  EXPECT_EQ(p->candidates[0].label, "Add \xEF\xBF\xBDto cart");
  EXPECT_EQ(p->matches[0], std::vector<size_t>{0});
  EXPECT_TRUE(p->overflow[1]);

  // A password field reports itself as one.
  p = ParseProbe(
      Json("{\"origin\":\"https://shop.example\",\"candidates\":[{\"tag\":"
           "\"input\",\"type\":\"password\",\"role\":\"\",\"input_type\":"
           "\"password\",\"autocomplete\":\"\",\"label\":\"\"}],\"matches\":"
           "[[0]],\"over\":[false]}"),
      1);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->candidates[0].field, FieldKind::kPassword);

  for (const std::string& bad : {
           std::string("1"),
           "{\"origin\":\"https://shop.example\",\"candidates\":[" + candidate +
               "],\"matches\":[[0]],\"over\":[false,false]}",
           "{\"origin\":\"https://shop.example\",\"candidates\":[" + candidate +
               "],\"matches\":[[1],[]],\"over\":[false,false]}",
           "{\"origin\":\"https://shop.example\",\"candidates\":[" + candidate +
               "],\"matches\":[[0,0],[]],\"over\":[false,false]}",
           "{\"origin\":\"https://shop.example\",\"candidates\":[" + candidate +
               "],\"matches\":[[-1],[]],\"over\":[false,false]}",
           "{\"origin\":\"https://shop.example\",\"candidates\":[" + candidate +
               "],\"matches\":[[0],[]],\"over\":[false,1]}",
           std::string("{\"origin\":\"https://shop.example\",\"candidates\":[{\"tag\":"
           "\"BUTTON\"}],\"matches\":[[],[]],\"over\":[false,false]}"),
           std::string("{\"candidates\":[],\"matches\":[[],[]],\"over\":[false,false]}"),
       }) {
    EXPECT_FALSE(ParseProbe(Json(bad), 2)) << bad;
  }
  // At most kMaxProbeCandidates candidates, kMaxMatchesPerLocator matches.
  std::string many = "[";
  for (size_t i = 0; i <= kMaxProbeCandidates; ++i)
    many += (i ? "," : "") + candidate;
  many += "]";
  EXPECT_FALSE(ParseProbe(
      Json("{\"origin\":\"https://shop.example\",\"candidates\":" + many +
           ",\"matches\":[[]],\"over\":[false]}"),
      1));
  std::string nine = "[";
  std::string nine_matches = "[";
  for (size_t i = 0; i <= kMaxMatchesPerLocator; ++i) {
    nine += (i ? "," : "") + candidate;
    nine_matches += (i ? "," : "") + std::to_string(i);
  }
  EXPECT_FALSE(ParseProbe(
      Json("{\"origin\":\"https://shop.example\",\"candidates\":" + nine +
           "],\"matches\":[" + nine_matches + "]],\"over\":[false]}"),
      1));
}

// ---------------------------------------------------------------------------
// Scripts and text
// ---------------------------------------------------------------------------

TEST(AutomationLocatorsTest, ScriptsQuotePageDataAsJson) {
  const std::string hostile = "\"); alert(1); (\"</script>\xE2\x80\xA8";
  std::vector<Locator> order = ProbeOrder(hostile, Button());
  order.push_back(L(By::kText, hostile));
  const std::string probe = BuildProbeScript("p1", order, hostile);
  EXPECT_EQ(probe.find("alert(1); (\"</script>"), std::string::npos);
  EXPECT_NE(probe.find("\\u003C/script>"), std::string::npos);
  EXPECT_NE(probe.find("var __moltLocatorLib = "), std::string::npos);
  EXPECT_NE(probe.find("__moltLocatorLib.probe(\"p1\", [{"),
            std::string::npos);
  const std::string capture = BuildCaptureScript(hostile);
  EXPECT_EQ(capture.find("alert(1); (\"</script>"), std::string::npos);
  EXPECT_NE(capture.find("__moltLocatorLib.capture(el)"), std::string::npos);

  EXPECT_NE(BuildPinScript("p1", 3, "AbC123").find(
                "__moltLocatorLib.pin(\"p1\", 3, \"AbC123\")"),
            std::string::npos);
  // A token that isn't one never reaches the page.
  EXPECT_NE(BuildPinScript("p1", 0, "a\"b").find("pin(\"p1\", 0, \"\")"),
            std::string::npos);
  EXPECT_EQ(PinSelector("AbC123"), "[data-molt-pick=\"AbC123\"]");
  EXPECT_EQ(PinSelector("a\"]"), "");
  EXPECT_EQ(PinSelector(""), "");
  EXPECT_EQ(PinSelector(std::string(kMaxTokenChars + 1, 'a')), "");
  EXPECT_EQ(PinnedCheck("a b"), "false");
  EXPECT_EQ(PinnedCheck("AbC"),
            "(globalThis.__moltLocate && globalThis.__moltLocate.token === "
            "\"AbC\" && globalThis.__moltLocate.pinned === el)");
}

TEST(AutomationLocatorsTest, CleansLabels) {
  EXPECT_EQ(CleanLabel("  Add\tto\n\ncart  ", 60), "Add to cart");
  EXPECT_EQ(CleanLabel("say \"hi\"", 60), "say 'hi'");
  EXPECT_EQ(CleanLabel("a\xE2\x80\xAE" "b\xE2\x80\x8B" "c", 60),
            "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c");
  EXPECT_EQ(CleanLabel("a\xC2\x85" "b", 60), "a b");
  EXPECT_EQ(CleanLabel("a\xFF" "b", 60), "a\xEF\xBF\xBD" "b");
  EXPECT_EQ(CleanLabel("abcdef", 6), "abcdef");
  EXPECT_EQ(CleanLabel("abcdefg", 6), "abcde\xE2\x80\xA6");
  EXPECT_EQ(CleanLabel("abcd efg", 6), "abcd\xE2\x80\xA6");
  EXPECT_EQ(CleanLabel("\xC3\xA9\xC3\xA9\xC3\xA9", 2), "\xC3\xA9\xE2\x80\xA6");
  EXPECT_EQ(CleanLabel("x", 0), "");
  EXPECT_EQ(CleanLabel(std::string(100000, 'a'), 3).size(), 2u + 3u);
}

TEST(AutomationLocatorsTest, AsksInTheBrowsersWords) {
  const std::string q = WeakMatchQuestion(
      "Click \xE2\x80\x9C" "Add to cart\xE2\x80\x9D",
      Cand(kButton, "Add to basket"), Describe(L(By::kText, "Add to cart")),
      Strength::kWeak);
  EXPECT_EQ(q,
            "The step \xE2\x80\x9C" "Click \xE2\x80\x9C" "Add to cart"
            "\xE2\x80\x9D\xE2\x80\x9D couldn't find its element as recorded: "
            "the page has changed. The closest match is the button "
            "\xE2\x80\x9C" "Add to basket\xE2\x80\x9D, found by its text. "
            "Use it?");
  const std::string c = WeakMatchQuestion(
      "", Cand({"input", "text", "textbox"}, ""),
      Describe(L(By::kRole, "Email", "textbox")), Strength::kConflict);
  EXPECT_EQ(c,
            "This step found different elements in different ways: the page "
            "has changed. The closest match is the text field, found by its "
            "role and name. Use it?");
  EXPECT_EQ(KindWord({"select", "", "combobox"}), "list");
  EXPECT_EQ(KindWord({"input", "password", ""}), "password field");
  EXPECT_EQ(KindWord({"div", "", ""}), "element");
}

// ---------------------------------------------------------------------------
// In a .molt file
// ---------------------------------------------------------------------------

TEST(AutomationLocatorsTest, StepsKeepThemThroughTheFile) {
  Script s;
  s.id = "wf";
  Step step;
  step.type = StepType::CLICK;
  step.target = "#add";
  step.locators = Button();
  s.steps.push_back(std::move(step));
  std::optional<Script> back = Script::FromJSONString(s.ToJSONString());
  ASSERT_TRUE(back);
  ASSERT_EQ(back->steps.size(), 1u);
  ASSERT_TRUE(back->steps[0].locators);
  EXPECT_EQ(*back->steps[0].locators, Button());
}

TEST(AutomationLocatorsTest, OldFilesLoadAndBadBlocksAreDropped) {
  // A file written before locators: unchanged.
  std::optional<Script> old = Script::FromJSONString(
      "{\"id\":\"wf\",\"steps\":[{\"type\":\"click\",\"target\":\"#go\","
      "\"selector_fallbacks\":[\"[name=go]\"],\"extra\":{\"text\":\"Go\"}}]}");
  ASSERT_TRUE(old);
  ASSERT_EQ(old->steps.size(), 1u);
  EXPECT_FALSE(old->steps[0].locators);
  EXPECT_EQ(old->steps[0].selector_fallbacks.size(), 1u);
  EXPECT_EQ(old->ToJSON().FindList("steps")->front().GetDict().Find("locators"),
            nullptr);
  // A hostile block: the step stays, without it.
  for (const std::string& block :
       {std::string("\"x\""), std::string("[]"),
        Block("[]"), Block("[{\"by\":\"text\",\"value\":\"" +
                           std::string(100000, 'a') + "\"}]"),
        Block(std::string("[") + kGoodEntry + "]", "{}")}) {
    std::optional<Script> s = Script::FromJSONString(
        "{\"id\":\"wf\",\"steps\":[{\"type\":\"click\",\"target\":\"#go\","
        "\"locators\":" + block + "}]}");
    ASSERT_TRUE(s) << block;
    ASSERT_EQ(s->steps.size(), 1u);
    EXPECT_FALSE(s->steps[0].locators) << block;
    EXPECT_EQ(s->steps[0].target, "#go");
  }
}

}  // namespace
}  // namespace locators
}  // namespace automation
}  // namespace molt_ai
