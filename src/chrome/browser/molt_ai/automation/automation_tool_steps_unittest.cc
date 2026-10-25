// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_tool_steps.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace tool_steps {
namespace {

using agent_safety::FieldKind;
using agent_tools::SiteTool;

constexpr char kOrigin[] = "https://shop.example";
constexpr char kCartSchema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\",\"maxLength\":20},"
    "\"qty\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":99},"
    "\"gift\":{\"type\":\"boolean\"},"
    "\"note\":{\"type\":\"string\"},"
    "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
    "\"required\":[\"id\"]}";

SiteTool Tool(const std::string& name,
              const std::string& schema,
              bool read_only = false) {
  agent_tools::ListedTool l;
  l.name = name;
  l.description = "A tool.";
  l.input_schema = schema;
  l.read_only = read_only;
  std::optional<SiteTool> t = agent_tools::MakeSiteTool(l);
  EXPECT_TRUE(t) << schema;
  return t ? std::move(*t) : SiteTool();
}

// Withholds values containing "secret".
class TestPolicy : public ValuePolicy {
 public:
  bool Withhold(std::string_view, std::string_view value) const override {
    return value.find("secret") != std::string_view::npos;
  }
};

RecordedCall Call(const std::string& args,
                  SiteTool tool = Tool("add_to_cart", kCartSchema)) {
  RecordedCall c;
  c.origin = kOrigin;
  c.tool = std::move(tool);
  c.arguments_json = args;
  return c;
}

base::DictValue Dict(const std::string& json) {
  std::optional<base::DictValue> d =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  EXPECT_TRUE(d) << json;
  return d ? std::move(*d) : base::DictValue();
}

// A step through a .molt file and back.
Step RoundTrip(const Step& s) {
  Script script;
  script.id = "wf";
  Step copy;
  copy.type = s.type;
  copy.target = s.target;
  copy.description = s.description;
  copy.extra = s.extra.Clone();
  script.steps.push_back(std::move(copy));
  std::optional<Script> back = Script::FromJSONString(script.ToJSONString());
  EXPECT_TRUE(back);
  return back ? std::move(back->steps[0]) : Step();
}

// The step as a .molt file holds it.
std::string StepJson(const Step& s) {
  Script script;
  script.id = "wf";
  Step copy;
  copy.type = s.type;
  copy.target = s.target;
  copy.description = s.description;
  copy.extra = s.extra.Clone();
  script.steps.push_back(std::move(copy));
  return script.ToJSONString();
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

TEST(AutomationToolStepsTest, RecordsTheCallTheUserApproved) {
  TestPolicy policy;
  std::map<std::string, std::string> vars;
  std::optional<Step> s =
      BuildToolStep(Call("{\"id\":\"A1\",\"qty\":2,\"tags\":[\"x\"]}"), policy,
                    &vars);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->type, StepType::CALL_TOOL);
  EXPECT_EQ(s->target, "add_to_cart");
  EXPECT_EQ(s->description,
            "Run the site's tool \xE2\x80\x9C" "add_to_cart\xE2\x80\x9D on "
            "shop.example");
  EXPECT_TRUE(vars.empty());
  // The host joins the workflow's sites.
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy({}).size(), 0u);
  std::vector<Step> steps;
  steps.push_back(RoundTrip(*s));
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(steps),
            std::vector<std::string>{"shop.example"});

  std::optional<ToolStep> t = ParseToolStep(steps[0]);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->name, "add_to_cart");
  EXPECT_EQ(t->origin, kOrigin);
  EXPECT_EQ(t->host, "shop.example");
  EXPECT_FALSE(t->in_frame);
  EXPECT_EQ(t->arguments, Dict("{\"id\":\"A1\",\"qty\":2,\"tags\":[\"x\"]}"));
  EXPECT_TRUE(t->secret_arguments.empty());
  EXPECT_FALSE(t->recorded_read_only);
  EXPECT_FALSE(t->recorded_form);
}

TEST(AutomationToolStepsTest, NeverStoresSecrets) {
  TestPolicy policy;
  std::map<std::string, std::string> vars = {{"tool_note", "taken"}};
  // A value that looks secret: a {{variable}} asked at each run.
  std::optional<Step> s = BuildToolStep(
      Call("{\"id\":\"A1\",\"note\":\"my secret\"}"), policy, &vars);
  ASSERT_TRUE(s);
  std::optional<ToolStep> t = ParseToolStep(*s);
  ASSERT_TRUE(t);
  EXPECT_EQ(*t->arguments.FindString("note"), "{{tool_note_2}}");
  EXPECT_EQ(vars.count("tool_note_2"), 1u);
  EXPECT_EQ(vars["tool_note_2"], "");
  EXPECT_EQ(*s->extra.GetDict().FindString("review"), kReviewWithheld);
  EXPECT_EQ(StepJson(*s).find("my secret"), std::string::npos);
  // Text a workflow would read as a variable is asked too.
  s = BuildToolStep(Call("{\"id\":\"{{pin}}\"}"), policy, &vars);
  ASSERT_TRUE(s);
  EXPECT_EQ(StepJson(*s).find("{{pin}}"), std::string::npos);

  // An argument named like a card or one-time code: not stored, the step
  // can't replay.
  SiteTool pay = Tool("pay",
                      "{\"type\":\"object\",\"properties\":{"
                      "\"amount\":{\"type\":\"integer\",\"minimum\":0},"
                      "\"otp\":{\"type\":\"string\"}}}");
  s = BuildToolStep(Call("{\"amount\":5,\"otp\":\"123456\"}", pay), policy,
                    &vars);
  ASSERT_TRUE(s);
  t = ParseToolStep(*s);
  ASSERT_TRUE(t);
  // The tool takes a one-time code (by that argument): none of its values
  // is stored.
  EXPECT_EQ(t->secret_arguments, (std::vector<std::string>{"amount", "otp"}));
  EXPECT_FALSE(t->arguments.contains("otp"));
  EXPECT_EQ(StepJson(*s).find("123456"), std::string::npos);
  EXPECT_EQ(*s->extra.GetDict().FindString("review"), kReviewSecretArgument);

  // A tool that takes a card or code by its form's fields: nothing stored.
  SiteTool checkout = Tool("checkout",
                           "{\"type\":\"object\",\"properties\":{"
                           "\"num\":{\"type\":\"string\"}}}");
  checkout.sensitive = FieldKind::kPaymentCard;
  s = BuildToolStep(Call("{\"num\":\"4111111111111111\"}", checkout), policy,
                    &vars);
  ASSERT_TRUE(s);
  EXPECT_EQ(StepJson(*s).find("4111"), std::string::npos);
  EXPECT_EQ(ParseToolStep(*s)->secret_arguments,
            std::vector<std::string>{"num"});

  // A list with a secret-looking item isn't stored either.
  s = BuildToolStep(Call("{\"id\":\"A1\",\"tags\":[\"a\",\"secret\"]}"),
                    policy, &vars);
  ASSERT_TRUE(s);
  EXPECT_EQ(ParseToolStep(*s)->secret_arguments,
            std::vector<std::string>{"tags"});
}

TEST(AutomationToolStepsTest, RefusesCallsThatArentTheTools) {
  TestPolicy policy;
  std::map<std::string, std::string> vars;
  EXPECT_FALSE(BuildToolStep(Call("not json"), policy, &vars));
  EXPECT_FALSE(BuildToolStep(Call("[]"), policy, &vars));
  EXPECT_FALSE(BuildToolStep(Call("{\"other\":1}"), policy, &vars));
  EXPECT_FALSE(BuildToolStep(Call("{\"qty\":\"two\"}"), policy, &vars));
  EXPECT_FALSE(BuildToolStep(Call("{\"id\":{\"a\":1}}"), policy, &vars));
  RecordedCall bad = Call("{}");
  bad.origin = "file:///x";
  EXPECT_FALSE(BuildToolStep(bad, policy, &vars));
  bad = Call("{}");
  bad.tool.name = "a b";
  EXPECT_FALSE(BuildToolStep(bad, policy, &vars));
  bad = Call("{}");
  bad.in_frame = true;
  bad.frame_origin = "https://w.example";
  bad.frame_path = "no-slash";
  EXPECT_FALSE(BuildToolStep(bad, policy, &vars));
}

TEST(AutomationToolStepsTest, RecordsTheFrameByOriginAndPath) {
  TestPolicy policy;
  std::map<std::string, std::string> vars;
  RecordedCall c = Call("{\"id\":\"A1\"}");
  c.in_frame = true;
  c.frame_origin = "https://widget.example";
  c.frame_path = "/cart";
  std::optional<Step> s = BuildToolStep(c, policy, &vars);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->description,
            "Run the site's tool \xE2\x80\x9C" "add_to_cart\xE2\x80\x9D on "
            "shop.example (in its frame from widget.example)");
  std::optional<ToolStep> t = ParseToolStep(RoundTrip(*s));
  ASSERT_TRUE(t);
  EXPECT_TRUE(t->in_frame);
  EXPECT_EQ(t->frame_origin, "https://widget.example");
  EXPECT_EQ(t->frame_path, "/cart");
  EXPECT_TRUE(IsCrossOriginFrame(*t));
  // A frame of the page's own origin isn't another site's.
  c.frame_origin = kOrigin;
  t = ParseToolStep(*BuildToolStep(c, policy, &vars));
  ASSERT_TRUE(t);
  EXPECT_FALSE(IsCrossOriginFrame(*t));
}

// ---------------------------------------------------------------------------
// Reading a file
// ---------------------------------------------------------------------------

Step FromJson(const std::string& extra, const std::string& target = "t") {
  Step s;
  s.type = StepType::CALL_TOOL;
  s.target = target;
  std::optional<base::Value> v =
      base::JSONReader::Read(extra, base::JSON_PARSE_RFC);
  EXPECT_TRUE(v) << extra;
  if (v)
    s.extra = std::move(*v);
  return s;
}

std::string Extra(const std::string& tool,
                  const std::string& host = "shop.example") {
  return "{\"host\":\"" + host + "\",\"tool\":" + tool + "}";
}

TEST(AutomationToolStepsTest, ReadsOnlyWellFormedSteps) {
  EXPECT_TRUE(ParseToolStep(FromJson(Extra(
      "{\"origin\":\"https://shop.example\",\"args\":{}}"))));
  const std::string long_string(agent_tools::kMaxToolStringChars + 1, 'a');
  for (const std::string& tool : {
           std::string("1"),
           std::string("{\"args\":{}}"),
           std::string("{\"origin\":\"https://evil.example\",\"args\":{}}"),
           std::string("{\"origin\":\"https://shop.example/\",\"args\":{}}"),
           std::string("{\"origin\":\"https://shop.example\"}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":[]}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"__proto__\":1}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"a\":{\"b\":1}}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"a\":[[1]]}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"a\":\"x\\u0000y\"}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"a\":\"x {{y}}\"}}"),
           "{\"origin\":\"https://shop.example\",\"args\":{\"a\":\"" +
               long_string + "\"}}",
           std::string("{\"origin\":\"https://shop.example\",\"args\":{},"
                       "\"frame\":{\"origin\":\"javascript:x\",\"path\":\"/\"}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":{},"
                       "\"frame\":{\"origin\":\"https://w.example\"}}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":"
                       "{\"a\":1},\"secret_args\":[\"a\"]}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":{},"
                       "\"secret_args\":[]}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":{},"
                       "\"secret_args\":[\"x\",\"x\"]}"),
           std::string("{\"origin\":\"https://shop.example\",\"args\":{},"
                       "\"read_only\":\"yes\"}"),
       }) {
    EXPECT_FALSE(ParseToolStep(FromJson(Extra(tool)))) << tool;
  }
  // Too many arguments.
  std::string many = "{";
  for (size_t i = 0; i <= agent_tools::kMaxToolArguments; ++i)
    many += std::string(i ? "," : "") + "\"a" + std::to_string(i) + "\":1";
  many += "}";
  EXPECT_FALSE(ParseToolStep(FromJson(
      Extra("{\"origin\":\"https://shop.example\",\"args\":" + many + "}"))));
  // Not a tool name, not a tool step, no host.
  EXPECT_FALSE(ParseToolStep(FromJson(
      Extra("{\"origin\":\"https://shop.example\",\"args\":{}}"), "a b")));
  Step click = FromJson(Extra("{\"origin\":\"https://shop.example\","
                              "\"args\":{}}"));
  click.type = StepType::CLICK;
  EXPECT_FALSE(ParseToolStep(click));
  EXPECT_FALSE(ParseToolStep(FromJson(
      "{\"tool\":{\"origin\":\"https://shop.example\",\"args\":{}}}")));
  // A whole {{variable}} is a value; a string with one inside isn't.
  EXPECT_TRUE(ParseToolStep(FromJson(Extra(
      "{\"origin\":\"https://shop.example\",\"args\":{\"a\":\"{{tool_a}}\"}}"))));
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

ToolStep Recorded(const std::string& args = "{\"id\":\"A1\",\"qty\":2}") {
  TestPolicy policy;
  std::map<std::string, std::string> vars;
  std::optional<Step> s = BuildToolStep(Call(args), policy, &vars);
  EXPECT_TRUE(s);
  std::optional<ToolStep> t = s ? ParseToolStep(*s) : std::nullopt;
  EXPECT_TRUE(t);
  return t ? std::move(*t) : ToolStep();
}

TEST(AutomationToolStepsTest, FindsTheToolInItsFrame) {
  ToolStep main = Recorded();
  std::vector<SiteTool> tools;
  tools.push_back(Tool("search", "{}"));
  tools.push_back(Tool("add_to_cart", kCartSchema));
  tools.back().frame = 1;
  tools.push_back(Tool("add_to_cart", kCartSchema));
  std::vector<ListedFrame> frames = {{kOrigin, "/"}, {kOrigin, "/w"}};
  EXPECT_EQ(FindListedTool(main, tools, frames), 2u);
  tools.pop_back();
  EXPECT_FALSE(FindListedTool(main, tools, frames)) << "only in a frame now";

  ToolStep framed = Recorded();
  framed.in_frame = true;
  framed.frame_origin = "https://widget.example";
  framed.frame_path = "/cart";
  frames = {{kOrigin, "/"},
            {"https://widget.example", "/other"},
            {"https://widget.example", "/cart"},
            {"https://evil.example", "/cart"}};
  tools.clear();
  for (size_t f = 1; f <= 3; ++f) {
    tools.push_back(Tool("add_to_cart", kCartSchema));
    tools.back().frame = f;
    tools.back().cross_origin = true;
    tools.back().frame_origin = frames[f].origin;
  }
  EXPECT_EQ(FindListedTool(framed, tools, frames), 1u) << "at its path";
  // Only as another site's frame's tool, listed as such.
  std::vector<SiteTool> unmarked = tools;
  for (SiteTool& t : unmarked)
    t.cross_origin = false;
  EXPECT_FALSE(FindListedTool(framed, unmarked, frames));
  ToolStep own_frame = Recorded();
  own_frame.in_frame = true;
  own_frame.frame_origin = kOrigin;
  own_frame.frame_path = "/w";
  EXPECT_FALSE(FindListedTool(own_frame, tools, {{kOrigin, "/"},
                                                 {kOrigin, "/w"},
                                                 {kOrigin, "/w"},
                                                 {kOrigin, "/w"}}));
  frames[2].path = "/moved";
  EXPECT_FALSE(FindListedTool(framed, tools, frames))
      << "two frames of the site, neither at the path";
  tools.erase(tools.begin());
  EXPECT_EQ(FindListedTool(framed, tools, frames), 0u)
      << "the only frame of the site with the tool";
  EXPECT_FALSE(FindListedTool(framed, {}, frames));
}

TEST(AutomationToolStepsTest, CallsOnlyACompatibleTool) {
  const ToolStep step = Recorded();
  agent_tools::ToolArguments out;
  // As recorded.
  EXPECT_EQ(CheckReplay(step, Tool("add_to_cart", kCartSchema),
                        step.arguments, &out),
            Compat::kOk);
  EXPECT_EQ(out.json, "{\"id\":\"A1\",\"qty\":2}");
  // A recorded argument gone, or of another kind.
  EXPECT_EQ(CheckReplay(step,
                        Tool("add_to_cart",
                             "{\"type\":\"object\",\"properties\":{"
                             "\"id\":{\"type\":\"string\"}}}"),
                        step.arguments, &out),
            Compat::kMissingArgument);
  EXPECT_EQ(CheckReplay(step,
                        Tool("add_to_cart",
                             "{\"type\":\"object\",\"properties\":{"
                             "\"id\":{\"type\":\"string\"},"
                             "\"qty\":{\"type\":\"string\"}}}"),
                        step.arguments, &out),
            Compat::kMissingArgument);
  // A new required argument.
  EXPECT_EQ(CheckReplay(step,
                        Tool("add_to_cart",
                             "{\"type\":\"object\",\"properties\":{"
                             "\"id\":{\"type\":\"string\"},"
                             "\"qty\":{\"type\":\"integer\"},"
                             "\"size\":{\"type\":\"string\"}},"
                             "\"required\":[\"size\"]}"),
                        step.arguments, &out),
            Compat::kNewRequirement);
  // ... an optional one is fine (left out).
  EXPECT_EQ(CheckReplay(step,
                        Tool("add_to_cart",
                             "{\"type\":\"object\",\"properties\":{"
                             "\"id\":{\"type\":\"string\"},"
                             "\"qty\":{\"type\":\"integer\"},"
                             "\"size\":{\"type\":\"string\"}}}"),
                        step.arguments, &out),
            Compat::kOk);
  // Tighter bounds than the value.
  EXPECT_EQ(CheckReplay(step,
                        Tool("add_to_cart",
                             "{\"type\":\"object\",\"properties\":{"
                             "\"id\":{\"type\":\"string\",\"maxLength\":1},"
                             "\"qty\":{\"type\":\"integer\"}}}"),
                        step.arguments, &out),
            Compat::kBadValue);
  // Now a <form> tool.
  SiteTool form = Tool("add_to_cart", kCartSchema);
  form.form_backed = true;
  EXPECT_EQ(CheckReplay(step, form, step.arguments, &out),
            Compat::kChangedKind);
  // A secret argument wasn't stored.
  ToolStep secret = Recorded();
  secret.secret_arguments = {"otp"};
  EXPECT_EQ(CheckReplay(secret, Tool("add_to_cart", kCartSchema),
                        secret.arguments, &out),
            Compat::kSecretArgument);
  EXPECT_FALSE(CompatText(Compat::kSecretArgument).empty());
  // The variable's value as substituted goes through the same checks.
  base::DictValue resolved = Dict("{\"id\":\"" + std::string(21, 'x') + "\"}");
  EXPECT_EQ(CheckReplay(step, Tool("add_to_cart", kCartSchema), resolved,
                        &out),
            Compat::kBadValue);
}

TEST(AutomationToolStepsTest, AsksUnlessReadOnlyOrApproved) {
  const ToolStep step = Recorded();
  SiteTool reads = Tool("add_to_cart", kCartSchema, /*read_only=*/true);
  SiteTool changes = Tool("add_to_cart", kCartSchema, /*read_only=*/false);
  EXPECT_EQ(ApprovalFor(step, reads, false), Approval::kRun);
  EXPECT_EQ(ApprovalFor(step, changes, false), Approval::kAsk);
  EXPECT_EQ(ApprovalFor(step, changes, true), Approval::kRun);
  // Consequential, a form: not "reads only".
  SiteTool weighty = reads;
  weighty.consequential = true;
  EXPECT_EQ(ApprovalFor(step, weighty, false), Approval::kAsk);
  EXPECT_EQ(ApprovalFor(step, weighty, true), Approval::kRun);
  SiteTool form = reads;
  form.form_backed = true;
  EXPECT_EQ(ApprovalFor(step, form, false), Approval::kAsk);
  // A card or code tool: always.
  SiteTool card = reads;
  card.sensitive = FieldKind::kPaymentCard;
  EXPECT_EQ(ApprovalFor(step, card, true), Approval::kAsk);
  EXPECT_FALSE(TrustWouldAllow(step, card));
  EXPECT_TRUE(TrustWouldAllow(step, changes));
  // Another site's frame: always, read-only or trusted.
  ToolStep framed = Recorded();
  framed.in_frame = true;
  framed.frame_origin = "https://widget.example";
  EXPECT_EQ(ApprovalFor(framed, reads, true), Approval::kAsk);
  EXPECT_FALSE(TrustWouldAllow(framed, changes));
  framed.frame_origin = kOrigin;
  EXPECT_EQ(ApprovalFor(framed, reads, false), Approval::kRun);
  // A tool listed as another site's frame's asks too, whatever the step.
  SiteTool other = reads;
  other.cross_origin = true;
  other.frame_origin = "https://widget.example";
  EXPECT_EQ(ApprovalFor(step, other, true), Approval::kAsk);
  EXPECT_FALSE(TrustWouldAllow(step, other));
}

}  // namespace
}  // namespace tool_steps
}  // namespace automation
}  // namespace molt_ai
