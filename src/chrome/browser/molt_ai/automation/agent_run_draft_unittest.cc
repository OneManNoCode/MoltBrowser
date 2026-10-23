// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/agent_run_draft.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace agent_draft {
namespace {

using agent_safety::FieldKind;
// gtest's Test::Run() hides the struct inside TEST bodies.
using AgentRun = agent_draft::Run;

constexpr int64_t kNow = 1790000000;
constexpr char kOrigin[] = "https://shop.example";

RunStep Nav(std::string url) {
  RunStep s;
  s.action = "NAVIGATE";
  s.success = true;
  s.url = std::move(url);
  return s;
}

RunStep Search(std::string query) {
  RunStep s;
  s.action = "SEARCH";
  s.success = true;
  s.query = query;
  // agent_actions::BuildSearchURL's shape.
  std::string q;
  for (char c : query)
    q.push_back(c == ' ' ? '+' : c);
  s.url = "https://search.brave.com/search?q=" + q;
  return s;
}

RunElement Element(std::string selector,
                   std::string label,
                   std::string kind = "button") {
  RunElement e;
  e.selector = std::move(selector);
  e.label = std::move(label);
  e.kind = std::move(kind);
  return e;
}

RunElement Field(std::string selector,
                 std::string label,
                 std::string text,
                 FieldKind kind = FieldKind::kOrdinary) {
  RunElement e = Element(std::move(selector), std::move(label), "input");
  e.input_type = "text";
  e.text = std::move(text);
  e.field_kind = kind;
  return e;
}

RunStep Click(std::string selector,
              std::string label,
              std::string origin = kOrigin) {
  RunStep s;
  s.action = "CLICK";
  s.success = true;
  s.origin = std::move(origin);
  s.elements.push_back(Element(std::move(selector), std::move(label)));
  return s;
}

RunStep Type(std::string selector,
             std::string label,
             std::string text,
             FieldKind kind = FieldKind::kOrdinary) {
  RunStep s;
  s.action = "TYPE";
  s.success = true;
  s.origin = kOrigin;
  s.elements.push_back(
      Field(std::move(selector), std::move(label), std::move(text), kind));
  return s;
}

RunStep Fill(std::vector<RunElement> fields) {
  RunStep s;
  s.action = "FILL_FORM";
  s.success = true;
  s.origin = kOrigin;
  s.elements = std::move(fields);
  return s;
}

RunStep Scroll(std::string direction) {
  RunStep s;
  s.action = "SCROLL";
  s.success = true;
  s.direction = std::move(direction);
  return s;
}

RunStep Bare(std::string action, bool success = true) {
  RunStep s;
  s.action = std::move(action);
  s.success = success;
  return s;
}

RunStep Failed(RunStep s) {
  s.success = false;
  return s;
}

AgentRun MakeRun(std::vector<RunStep> steps,
                 std::string goal = "Buy blue socks",
                 std::string start_url = "https://shop.example/home") {
  AgentRun r;
  r.goal = std::move(goal);
  r.start_url = std::move(start_url);
  r.succeeded = true;
  r.steps = std::move(steps);
  return r;
}

Draft Build(const AgentRun& run) {
  return BuildDraft(run, /*saved_workflows_allowed=*/true, kNow);
}

const std::string* ExtraString(const Step& s, const char* key) {
  return s.extra.is_dict() ? s.extra.GetDict().FindString(key) : nullptr;
}

// One line, valid UTF-8, nothing hidden: what the studio shows.
void ExpectCleanLine(const std::string& s) {
  for (unsigned char c : s) {
    EXPECT_FALSE(c < 0x20 || c == 0x7F) << s;
  }
  for (const char* hidden :
       {"\xE2\x80\xAE", "\xE2\x80\x8B", "\xE2\x80\xA8", "\xEF\xBB\xBF",
        "\xC2\x85", "\xD8\x9C", "\xE2\x81\xA6"}) {
    EXPECT_EQ(s.find(hidden), std::string::npos) << s;
  }
  // Descriptions are {{variable}}-resolved by the runner.
  EXPECT_EQ(s.find("{{"), std::string::npos) << s;
}

TEST(AgentRunDraftTest, ConvertsTheStepsThatRanInOrder) {
  AgentRun run = MakeRun({
      Bare("OBSERVE"),
      Click("#menu", "Menu"),
      Failed(Click("#gone", "Gone")),
      Bare("REPLAN"),
      Type("#q", "Search products", "blue socks"),
      Scroll("down"),
      Fill({Field("#name", "Full name", "Ada Lovelace"),
            Field("body > form > input:nth-of-type(2)", "", "London")}),
      Click("html > body > div:nth-of-type(3) > a:nth-of-type(1)", ""),
      Scroll("up"),
      Bare("DONE"),
  });
  Draft d = Build(run);
  ASSERT_EQ(d.refusal, Refusal::kNone) << d.message;
  ASSERT_TRUE(d.script);
  const Script& s = *d.script;
  ASSERT_EQ(s.steps.size(), 8u);

  // The agent started on a page: the draft opens it first.
  EXPECT_EQ(s.steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(s.steps[0].target, "https://shop.example/home");
  EXPECT_EQ(s.steps[0].description, "Open shop.example");

  EXPECT_EQ(s.steps[1].type, StepType::CLICK);
  EXPECT_EQ(s.steps[1].target, "#menu");
  EXPECT_TRUE(s.steps[1].selector_fallbacks.empty());
  EXPECT_EQ(*ExtraString(s.steps[1], "text"), "Menu");
  EXPECT_EQ(*ExtraString(s.steps[1], kStepHostKey), "shop.example");
  EXPECT_EQ(s.steps[1].description, "Click \xE2\x80\x9CMenu\xE2\x80\x9D");

  EXPECT_EQ(s.steps[2].type, StepType::TYPE);
  EXPECT_EQ(s.steps[2].target, "#q");
  EXPECT_EQ(s.steps[2].value, "blue socks");
  EXPECT_EQ(*ExtraString(s.steps[2], "text"), "Search products");
  EXPECT_EQ(ExtraString(s.steps[2], kReviewKey), nullptr);

  EXPECT_EQ(s.steps[3].type, StepType::SCROLL);
  EXPECT_EQ(s.steps[3].value, "down");
  EXPECT_EQ(s.steps[3].description, "Scroll down");

  // FILL_FORM: one TYPE step per field, in order.
  EXPECT_EQ(s.steps[4].type, StepType::TYPE);
  EXPECT_EQ(s.steps[4].value, "Ada Lovelace");
  EXPECT_EQ(s.steps[5].type, StepType::TYPE);
  EXPECT_EQ(s.steps[5].target, "body > form > input:nth-of-type(2)");
  EXPECT_EQ(s.steps[5].value, "London");
  // No label: no text anchor, and no ASCII quote in the description the
  // runner would read as one.
  EXPECT_EQ(ExtraString(s.steps[5], "text"), nullptr);

  EXPECT_EQ(s.steps[6].description, "Click the button");
  EXPECT_EQ(ExtraString(s.steps[6], "text"), nullptr);
  EXPECT_EQ(s.steps[7].value, "up");

  for (const Step& step : s.steps) {
    EXPECT_EQ(step.description.find('"'), std::string::npos)
        << step.description;
    ExpectCleanLine(step.description);
  }

  // A draft: CASUAL, no site until the user confirms, never scheduled.
  EXPECT_TRUE(s.id.empty());
  EXPECT_EQ(s.name, "Buy blue socks");
  EXPECT_EQ(s.created_at_unix, kNow);
  EXPECT_EQ(s.trigger.type, TriggerType::MANUAL);
  EXPECT_EQ(s.security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(s.security.sites_unconfirmed);
  EXPECT_TRUE(s.security.domain_whitelist.empty());
  EXPECT_EQ(s.security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  EXPECT_EQ(s.stats.runs, 0);
  EXPECT_TRUE(s.default_variables.empty());
  EXPECT_EQ(d.withheld, 0);
  EXPECT_EQ(d.secret_steps, 0);

  // The sites the user will be asked to confirm.
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(s.steps),
            std::vector<std::string>{"shop.example"});

  // It saves and loads back the same.
  d.script->id = "agent_1";
  const std::string json = s.ToJSONString();
  std::optional<Script> loaded = Script::FromJSONString(json);
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->ToJSONString(), json);
  EXPECT_TRUE(loaded->security.sites_unconfirmed);
}

TEST(AgentRunDraftTest, SearchAndNavigationAreNavigations) {
  Draft d = Build(MakeRun({Search("cheap flights tokyo"),
                           Click("#r1", "Result one",
                                 "https://search.brave.com"),
                           Nav("https://www.airline.example/book?from=SFO"),
                           Bare("DONE")}));
  ASSERT_EQ(d.refusal, Refusal::kNone) << d.message;
  const Script& s = *d.script;
  // The run began by opening a page: no start page.
  ASSERT_EQ(s.steps.size(), 3u);
  EXPECT_EQ(s.steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(s.steps[0].target,
            "https://search.brave.com/search?q=cheap+flights+tokyo");
  EXPECT_EQ(s.steps[0].description,
            "Search the web for \xE2\x80\x9C" "cheap flights tokyo"
            "\xE2\x80\x9D");
  EXPECT_EQ(s.steps[2].target, "https://www.airline.example/book?from=SFO");
  EXPECT_EQ(s.steps[2].description, "Open www.airline.example");
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(s.steps),
            (std::vector<std::string>{"search.brave.com",
                                      "www.airline.example"}));
}

TEST(AgentRunDraftTest, RefusesUnfinishedAndMoltNetRuns) {
  AgentRun run = MakeRun({Click("#a", "A")});
  run.succeeded = false;
  Draft d = Build(run);
  EXPECT_EQ(d.refusal, Refusal::kNotSucceeded);
  EXPECT_FALSE(d.script);
  EXPECT_TRUE(d.message.empty());

  run.succeeded = true;
  run.in_moltnet = true;
  d = Build(run);
  EXPECT_EQ(d.refusal, Refusal::kMoltNet);
  EXPECT_FALSE(d.script);
  EXPECT_NE(d.message.find("MoltNet"), std::string::npos);

  // The profile's access says no, whatever the run says.
  run.in_moltnet = false;
  d = BuildDraft(run, /*saved_workflows_allowed=*/false, kNow);
  EXPECT_EQ(d.refusal, Refusal::kMoltNet);
  EXPECT_FALSE(d.script);
}

TEST(AgentRunDraftTest, RefusesRunsThatCalledASiteTool) {
  RunStep tool = Bare("CALL_TOOL");
  Draft d = Build(MakeRun({Click("#a", "A"), tool}));
  EXPECT_EQ(d.refusal, Refusal::kUsedSiteTool);
  EXPECT_FALSE(d.script);
  EXPECT_FALSE(d.message.empty());
  // A call that failed changed nothing the draft would miss.
  d = Build(MakeRun({Failed(tool), Click("#a", "A")}));
  EXPECT_EQ(d.refusal, Refusal::kNone);
}

TEST(AgentRunDraftTest, NothingToSave) {
  EXPECT_EQ(Build(MakeRun({Bare("OBSERVE"), Bare("DONE")})).refusal,
            Refusal::kNothingToSave);
  EXPECT_EQ(Build(MakeRun({Failed(Click("#a", "A")), Bare("DONE")})).refusal,
            Refusal::kNothingToSave);
  EXPECT_EQ(Build(MakeRun({})).refusal, Refusal::kNothingToSave);
}

TEST(AgentRunDraftTest, RefusesStepsItCantReplay) {
  auto refusal = [](RunStep step) {
    return Build(MakeRun({Click("#ok", "OK"), std::move(step)})).refusal;
  };
  RunStep no_element = Click("#a", "A");
  no_element.elements.clear();
  EXPECT_EQ(refusal(no_element), Refusal::kUnusableStep);
  RunStep two = Click("#a", "A");
  two.elements.push_back(Element("#b", "B"));
  EXPECT_EQ(refusal(two), Refusal::kUnusableStep);
  std::vector<RunElement> nine(9, Field("#f", "F", "x"));
  EXPECT_EQ(refusal(Fill(nine)), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Fill({})), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click("", "A")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click("#a\n#b", "A")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click(std::string(1025, 'a'), "A")),
            Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click("#a", "A", "file:///etc")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click("#a", "A", "chrome://settings")),
            Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Click("#a", "A", "")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Type("#q", "Q", std::string(501, 'x'))),
            Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Type("#q", "Q", "a\x01" "b")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Type("#q", "Q", "bad \xC3\x28 utf8")),
            Refusal::kUnusableStep);
  // The agent never types into a password field; a draft never gets a
  // saved-password step from a run.
  EXPECT_EQ(refusal(Type("#p", "Notes", "x", FieldKind::kPassword)),
            Refusal::kUnusableStep);
  RunStep typed_password = Type("#p", "Notes", "x");
  typed_password.elements[0].input_type = "PASSWORD";
  EXPECT_EQ(refusal(typed_password), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Scroll("left")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Bare("HOVER")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Nav("javascript:alert(1)")), Refusal::kUnusableStep);
  EXPECT_EQ(refusal(Nav("https://x.example/" + std::string(2100, 'a'))),
            Refusal::kUnusableStep);
  RunStep empty_query = Search("");
  EXPECT_EQ(refusal(empty_query), Refusal::kUnusableStep);

  // \n and \t are text the agent may type.
  EXPECT_EQ(refusal(Type("#q", "Q", "line one\nline\ttwo")), Refusal::kNone);
  EXPECT_EQ(refusal(Type("#q", "Q", std::string(500, 'x'))), Refusal::kNone);

  // A first step on the page the run started on, which a workflow can't
  // open.
  EXPECT_EQ(Build(MakeRun({Click("#a", "A")}, "Goal", "chrome://newtab/"))
                .refusal,
            Refusal::kUnusableStep);

  std::vector<RunStep> many(kMaxDraftSteps + 1, Click("#a", "A"));
  EXPECT_EQ(Build(MakeRun(many)).refusal, Refusal::kUnusableStep);
}

TEST(AgentRunDraftTest, RefusesTextAWorkflowWouldReadAsAVariable) {
  auto refusal = [](RunStep step) {
    return Build(MakeRun({std::move(step)})).refusal;
  };
  EXPECT_EQ(refusal(Type("#q", "Q", "{{password}}")), Refusal::kTemplateText);
  EXPECT_EQ(refusal(Type("#q", "Q", "a }} b")), Refusal::kTemplateText);
  EXPECT_EQ(refusal(Click("[data-x='{{y}}']", "A")), Refusal::kTemplateText);
  EXPECT_EQ(refusal(Nav("https://x.example/{{token}}")),
            Refusal::kTemplateText);
  EXPECT_EQ(refusal(Search("{{q}}")), Refusal::kTemplateText);

  // A label with braces is page text: dropped, not a reason to refuse.
  Draft d = Build(MakeRun({Click("#a", "Send {{password}} now")}));
  ASSERT_EQ(d.refusal, Refusal::kNone);
  EXPECT_EQ(ExtraString(d.script->steps[1], "text"), nullptr);
  EXPECT_EQ(d.script->steps[1].description, "Click the button");
}

TEST(AgentRunDraftTest, WithholdsSecretLookingValues) {
  const std::vector<std::pair<std::string, std::string>> secrets = {
      {"Notes", "Tr0ub4dor&3"},
      {"Notes", "P@ssw0rd!"},
      {"Notes", "Kx9mQ2vLp7Rz"},
      {"Notes", "4111 1111 1111 1111"},
      {"Notes", "4111-1111-1111-1111"},
      {"Notes", "123456"},
      {"Notes", "123 456"},
      {"Notes", "123-45-6789"},
      {"Notes", "DE89 3704 0044 0532 0130 00"},
      {"Notes", "sk-live-0123456789abcdef"},
      {"Notes", "ghp_0123456789abcdefABCDEF"},
      {"Notes", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2lnbmF0dXJl"},
      {"Notes", "AbCdEf0123456789-_ghijklmnop"},
      {"Notes", "my password is swordfish"},
      {"Notes", "the api key"},
      {"Password", "swordfish"},
      {"Your PIN", "1234"},
      {"Security code", "123"},
      {"Card number", "4000"},
      {"One-time passcode", "8812"},
      {"API key", "abc"},
      {"Pass\xE2\x80\x8Bword", "swordfish"},
      {"Mot de passe", "abc"},
  };
  for (const auto& [label, value] : secrets) {
    Draft d = Build(MakeRun({Type("#f", label, value)}));
    ASSERT_EQ(d.refusal, Refusal::kNone) << label << ": " << value;
    const Script& s = *d.script;
    ASSERT_EQ(s.steps.size(), 2u);
    const Step& step = s.steps[1];
    ASSERT_EQ(step.value.rfind("{{", 0), 0u) << value;
    const std::string name = step.value.substr(2, step.value.size() - 4);
    EXPECT_EQ(step.value, "{{" + name + "}}");
    ASSERT_EQ(s.default_variables.count(name), 1u) << name;
    EXPECT_EQ(s.default_variables.at(name), "");
    EXPECT_EQ(*ExtraString(step, kReviewKey), kReviewWithheld);
    EXPECT_EQ(d.withheld, 1);
    ExpectCleanLine(step.description);
    EXPECT_NE(d.message.find("ask you at each run"), std::string::npos);
    // Nowhere in what is stored.
    EXPECT_EQ(s.ToJSONString().find(value), std::string::npos) << value;
  }
}

TEST(AgentRunDraftTest, KeepsOrdinaryValues) {
  for (const char* value :
       {"blue socks", "alice@example.com", "2026-12-01", "94110",
        "New York", "iPhone 15 Pro", "Zurich", "Z\xC3\xBCrich2024",
        "+1 555 123 4567", "flights to Tokyo on 12/24", "v1.2.3-beta",
        "Hello, World!", "12"}) {
    Draft d = Build(MakeRun({Type("#q", "Search", value)}));
    ASSERT_EQ(d.refusal, Refusal::kNone) << value;
    EXPECT_EQ(d.script->steps[1].value, value);
    EXPECT_EQ(d.withheld, 0) << value;
  }
}

TEST(AgentRunDraftTest, WithholdsWhatTheGoalGaveAsASecret) {
  Draft d = Build(MakeRun(
      {Type("#u", "Username", "ada"), Type("#c", "Comment", "hunter2 rocks"),
       Type("#promo", "Promo", "SAVE20"), Type("#city", "City", "Paris")},
      "Sign in as ada with password: hunter2, use code SAVE20, zip code for "
      "Paris"));
  ASSERT_EQ(d.refusal, Refusal::kNone);
  const Script& s = *d.script;
  EXPECT_EQ(s.steps[1].value, "ada");
  EXPECT_EQ(*ExtraString(s.steps[2], kReviewKey), kReviewWithheld);
  EXPECT_EQ(*ExtraString(s.steps[3], kReviewKey), kReviewWithheld);
  EXPECT_EQ(s.steps[4].value, "Paris");
  EXPECT_EQ(d.withheld, 2);
  // The goal named a secret: it isn't the draft's name.
  EXPECT_EQ(s.name, "Agent run on shop.example");
  const std::string json = s.ToJSONString();
  EXPECT_EQ(json.find("hunter2"), std::string::npos);
  EXPECT_EQ(json.find("SAVE20"), std::string::npos);
}

TEST(AgentRunDraftTest, VariableNamesAreUniqueAndPlain) {
  Draft d = Build(MakeRun({Fill({Field("#a", "Password", "x1"),
                                 Field("#b", "Password", "x2"),
                                 Field("#c", "", "Tr0ub4dor&3")})}));
  ASSERT_EQ(d.refusal, Refusal::kNone);
  const Script& s = *d.script;
  EXPECT_EQ(s.steps[1].value, "{{password}}");
  EXPECT_EQ(s.steps[2].value, "{{password_2}}");
  EXPECT_EQ(s.steps[3].value, "{{value}}");
  EXPECT_EQ(s.default_variables.size(), 3u);
  EXPECT_EQ(d.withheld, 3);
}

TEST(AgentRunDraftTest, CardAndCodeFieldsBecomeSecretSteps) {
  Draft d = Build(MakeRun(
      {Type("#cc", "Card", "4111 1111 1111 1111", FieldKind::kPaymentCard),
       Type("#otp", "Code", "123456", FieldKind::kOneTimeCode)}));
  ASSERT_EQ(d.refusal, Refusal::kNone);
  const Script& s = *d.script;
  ASSERT_EQ(s.steps.size(), 3u);
  EXPECT_TRUE(s.steps[1].value.empty());
  EXPECT_EQ(AutomationSecurity::SecretStepKind(s.steps[1]),
            FieldKind::kPaymentCard);
  EXPECT_EQ(*ExtraString(s.steps[1], kSecretHostKey), "shop.example");
  EXPECT_EQ(*ExtraString(s.steps[1], kReviewKey), kReviewSecretField);
  EXPECT_EQ(s.steps[1].description,
            "Enter the payment card yourself (not saved)");
  EXPECT_EQ(AutomationSecurity::SecretStepKind(s.steps[2]),
            FieldKind::kOneTimeCode);
  EXPECT_EQ(d.secret_steps, 2);
  EXPECT_EQ(d.withheld, 0);
  EXPECT_NE(d.message.find("never saves"), std::string::npos);
  const std::string json = s.ToJSONString();
  EXPECT_EQ(json.find("4111"), std::string::npos);
  EXPECT_EQ(json.find("123456"), std::string::npos);
  // Saving finds nothing more to scrub.
  base::DictValue dict = s.ToJSON();
  EXPECT_FALSE(AutomationSecurity::ScrubPlaintextSecrets(dict));
}

TEST(AgentRunDraftTest, RefusesAddressesThatHoldSecrets) {
  for (const char* url : {
           "https://user:pw@shop.example/",
           "https://shop.example/cb?token=abc",
           "https://shop.example/login?password=x&u=a",
           "https://shop.example/?PassWord=x",
           "https://shop.example/oauth?code=4/0AbC&state=1",
           "https://shop.example/#access_token=abc&type=bearer",
           "https://shop.example/#/inbox?session_id=1",
           "https://shop.example/x?api%5Fkey=1",
           "https://shop.example/reset-password/abcdefghij0123456789abcd",
           "https://shop.example/u/eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2ln",
           "https://shop.example/pay?n=4111111111111111",
           "https://shop.example/a?x=sk_live_0123456789abcdef",
       }) {
    EXPECT_TRUE(UrlLooksSecret(url)) << url;
    EXPECT_EQ(Build(MakeRun({Nav(url)})).refusal, Refusal::kSecretInAddress)
        << url;
  }
  for (const char* url : {
           "https://shop.example/",
           "https://shop.example/search?q=shoes&page=2",
           "https://shop.example/dp/B08N5WRWNW",
           "https://shop.example/item?id=550e8400-e29b-41d4-a716-446655440000",
           "https://shop.example/#/inbox",
           "https://shop.example/#section-2",
           "https://docs.example/d/1aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789/edit",
           "https://shop.example/author?name=tolkien",
       }) {
    EXPECT_FALSE(UrlLooksSecret(url)) << url;
    EXPECT_EQ(Build(MakeRun({Nav(url)})).refusal, Refusal::kNone) << url;
  }
  // The page the run started on.
  EXPECT_EQ(Build(MakeRun({Click("#a", "A")}, "Goal",
                          "https://shop.example/?sessionid=abc"))
                .refusal,
            Refusal::kSecretInAddress);
  // A search for a secret.
  EXPECT_EQ(Build(MakeRun({Search("4111 1111 1111 1111")})).refusal,
            Refusal::kSecretInAddress);
  EXPECT_EQ(Build(MakeRun({Search("hunter2 leak")}, "my password is hunter2"))
                .refusal,
            Refusal::kSecretInAddress);
}

TEST(AgentRunDraftTest, HostileLabelsBecomeOneCleanLine) {
  const std::string hostile =
      "Pay\xE2\x80\xAE" "evil\xE2\x80\x8B\n\tnow\x7F\xC2\x85 "
      "\xEF\xBB\xBF\xE2\x80\xA8\xE2\x81\xA6\xD8\x9C\xC3\x28" +
      std::string(400, 'x');
  Draft d = Build(MakeRun({Click("#pay", hostile)}));
  ASSERT_EQ(d.refusal, Refusal::kNone);
  const Step& step = d.script->steps[1];
  const std::string& text = *ExtraString(step, "text");
  ExpectCleanLine(text);
  ExpectCleanLine(step.description);
  EXPECT_EQ(text.rfind("Pay\xEF\xBF\xBD" "evil\xEF\xBF\xBD now", 0), 0u)
      << text;
  // At most kMaxLabelChars characters, cut with an ellipsis.
  size_t chars = 0;
  for (unsigned char c : text)
    chars += (c & 0xC0) != 0x80;
  EXPECT_EQ(chars, kMaxLabelChars);
  EXPECT_EQ(text.substr(text.size() - 3), "\xE2\x80\xA6");
}

TEST(AgentRunDraftTest, NamesTheDraftAfterACleanGoal) {
  Draft d = Build(MakeRun({Click("#a", "A")}, "  Order\n more \xE2\x80\xAE" "coffee  "));
  EXPECT_EQ(d.script->name, "Order more \xEF\xBF\xBD" "coffee");
  d = Build(MakeRun({Click("#a", "A")}, std::string(200, 'g')));
  EXPECT_EQ(d.script->name, std::string(59, 'g') + "\xE2\x80\xA6");
  d = Build(MakeRun({Click("#a", "A")}, ""));
  EXPECT_EQ(d.script->name, "Agent run on shop.example");
  d = Build(MakeRun({Click("#a", "A")}, "Pay with card 4111 1111 1111 1111"));
  EXPECT_EQ(d.script->name, "Agent run on shop.example");
  // A goal too long to scan: generic name, and every value asked.
  d = Build(MakeRun({Type("#q", "Search", "socks")}, std::string(5000, 'a')));
  EXPECT_EQ(d.script->name, "Agent run on shop.example");
  EXPECT_EQ(d.withheld, 1);
}

TEST(AgentRunDraftTest, Pieces) {
  EXPECT_TRUE(LabelSuggestsSecret("Password"));
  EXPECT_TRUE(LabelSuggestsSecret("Confirm new password"));
  EXPECT_TRUE(LabelSuggestsSecret("PIN"));
  EXPECT_TRUE(LabelSuggestsSecret("CVV2"));
  EXPECT_TRUE(LabelSuggestsSecret("Verification code"));
  EXPECT_TRUE(LabelSuggestsSecret("Card number"));
  EXPECT_TRUE(LabelSuggestsSecret("IBAN"));
  EXPECT_TRUE(LabelSuggestsSecret("Contrase\xC3\xB1" "a"));
  EXPECT_FALSE(LabelSuggestsSecret("Search"));
  EXPECT_FALSE(LabelSuggestsSecret("Zip code"));
  EXPECT_FALSE(LabelSuggestsSecret("Promo code"));
  EXPECT_FALSE(LabelSuggestsSecret("Pinterest handle"));
  EXPECT_FALSE(LabelSuggestsSecret(""));

  EXPECT_EQ(GoalSecretTokens("my password is hunter2"),
            std::vector<std::string>{"hunter2"});
  EXPECT_EQ(GoalSecretTokens("PIN: 1234, then the zip code for Paris"),
            std::vector<std::string>{"1234"});
  EXPECT_EQ(GoalSecretTokens("password=Tr0ub4dor&3"),
            std::vector<std::string>{"Tr0ub4dor&3"});
  EXPECT_TRUE(GoalSecretTokens("find red shoes under $50").empty());
  EXPECT_TRUE(GoalSecretTokens("").empty());

  EXPECT_EQ(CleanDisplayText("  a \n\t b  ", 10), "a b");
  EXPECT_EQ(CleanDisplayText("abcdef", 4), "abc\xE2\x80\xA6");
  EXPECT_EQ(CleanDisplayText("ab\xFF", 10), "ab\xEF\xBF\xBD");
  EXPECT_EQ(CleanDisplayText("x", 0), "");

  const std::set<std::string> none;
  EXPECT_EQ(VariableNameFor("Search products", none), "search_products");
  EXPECT_EQ(VariableNameFor("", none), "value");
  EXPECT_EQ(VariableNameFor("\xE5\xAF\x86\xE7\xA0\x81", none), "value");
  EXPECT_EQ(VariableNameFor("123 code", none), "value_123_code");
  EXPECT_EQ(VariableNameFor("__proto__", none), "proto");
  EXPECT_EQ(VariableNameFor(std::string(100, 'z'), none),
            std::string(kMaxVariableNameChars, 'z'));
  const std::set<std::string> taken = {"pin", "pin_2",
                                       std::string(kMaxVariableNameChars, 'z')};
  EXPECT_EQ(VariableNameFor("PIN", taken), "pin_3");
  EXPECT_EQ(VariableNameFor(std::string(100, 'z'), taken),
            std::string(kMaxVariableNameChars - 2, 'z') + "_2");
}

}  // namespace
}  // namespace agent_draft
}  // namespace automation
}  // namespace molt_ai
