// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/web_agent.h"

#include <cctype>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace molt_ai {

namespace {

// JSON-quote a string for safe embedding in JS source.
std::string JSQ(const std::string& s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

// Trim leading and trailing ASCII whitespace from |s| in-place.
std::string Trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Case-insensitive prefix check.
bool HasPrefixI(const std::string& line, const char* prefix) {
  return base::StartsWith(line, prefix, base::CompareCase::INSENSITIVE_ASCII);
}

// Search engine base URL (Brave Search — no tracking).
constexpr char kSearchBase[] = "https://search.brave.com/search?q=";

// Seconds to wait for page load before proceeding.
constexpr int kNavTimeoutSeconds = 12;

// Max page content chars sent to LLM per iteration.
constexpr int kMaxPageChars = 3000;

// System prompt for all LLM calls inside the agent loop.
const char kSystemPrompt[] =
    "You are a web browsing agent running inside MoltBrowser. "
    "You complete tasks by controlling the browser one action at a time.\n"
    "\n"
    "AVAILABLE ACTIONS — output EXACTLY one per turn:\n"
    "  NAVIGATE: <url>                         — load a full URL\n"
    "  SEARCH: <query>                         — search the web\n"
    "  CLICK: <css_selector>                   — click an element\n"
    "  TYPE: <css_selector> | <text>           — type text into a field\n"
    "  FILL_FORM: sel1=val1; sel2=val2         — fill multiple inputs\n"
    "  SCROLL: down | up | <pixels>            — scroll the page\n"
    "  OBSERVE: current                        — re-read the current page\n"
    "  DONE: <result summary>                  — task complete; give result\n"
    "  ERROR: <reason>                         — task impossible\n"
    "\n"
    "RULES:\n"
    "- After NAVIGATE or SEARCH the page is observed automatically.\n"
    "- After CLICK use OBSERVE to see the updated page state.\n"
    "- For FILL_FORM separate pairs with semicolons: sel=val; sel=val\n"
    "- CSS selectors: #id  .class  [name=\"x\"]  input[type=\"search\"]\n"
    "  button[type=\"submit\"]  a[href*=\"keyword\"]\n"
    "- Read the page content carefully before choosing a selector.\n"
    "- Maximum %d total steps. Be efficient.\n"
    "\n"
    "OUTPUT FORMAT (strict — no extra text):\n"
    "ACTION: <action_type>\n"
    "TARGET: <url or selector or query or direction>\n"
    "VALUE: <text if needed, else leave blank>\n"
    "REASON: <one-sentence reasoning>\n";

}  // namespace

// ----------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------

WebAgent::WebAgent(content::WebContents* web_contents,
                   BrowserAIRuntime* runtime)
    : content::WebContentsObserver(web_contents),
      web_contents_(web_contents),
      runtime_(runtime),
      dom_bridge_(std::make_unique<DOMContentBridge>(web_contents)) {}

WebAgent::~WebAgent() = default;

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

void WebAgent::Start(const std::string& goal,
                     StepCallback on_step,
                     DoneCallback on_done) {
  cancelled_  = false;
  iteration_  = 0;
  history_.clear();
  current_page_state_.clear();
  goal_    = goal;
  step_cb_ = std::move(on_step);
  done_cb_ = std::move(on_done);

  LOG(INFO) << "[WebAgent] Start: " << goal_;

  // Bootstrap: observe wherever the user is before the first LLM call.
  ObservePage();
}

void WebAgent::Cancel() {
  cancelled_ = true;
  weak_factory_.InvalidateWeakPtrs();
  LOG(INFO) << "[WebAgent] Cancelled";
}

// ----------------------------------------------------------------
// WebContentsObserver
// ----------------------------------------------------------------

void WebAgent::DidStopLoading() {
  MaybeNavDone();
}

// ----------------------------------------------------------------
// ReAct loop
// ----------------------------------------------------------------

void WebAgent::IterateOnce() {
  if (cancelled_) return;
  if (iteration_ >= kMaxIterations) {
    Finish(false, "Reached the maximum of " +
                      base::NumberToString(kMaxIterations) + " steps.");
    return;
  }
  AskLLM();
}

void WebAgent::AskLLM() {
  if (cancelled_) return;

  std::string prompt = BuildPrompt();
  BrowserAIRuntime* rt = runtime_;
  auto self = weak_factory_.GetWeakPtr();

  // RunPrompt is blocking. Post it to a background thread; reply on UI.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](BrowserAIRuntime* rt, std::string p) -> std::string {
            PromptOptions opts;
            opts.max_tokens  = 300;
            opts.temperature = 0.2f;
            opts.stream      = false;
            return rt->RunPrompt(p, opts).text;
          },
          rt, std::move(prompt)),
      base::BindOnce(
          [](base::WeakPtr<WebAgent> self, std::string response) {
            if (self) self->OnLLMDecision(std::move(response));
          },
          self));
}

void WebAgent::OnLLMDecision(std::string response) {
  if (cancelled_) return;
  LOG(INFO) << "[WebAgent] LLM: " << response.substr(0, 200);

  if (!ParseLLMResponse(response)) {
    LOG(WARNING) << "[WebAgent] Unparseable response — observing to retry";
    pending_step_.action = "OBSERVE";
    pending_step_.target = "current";
    pending_step_.reason = "Unparseable LLM output";
  }
  DispatchAction();
}

void WebAgent::DispatchAction() {
  if (cancelled_) return;

  LOG(INFO) << "[WebAgent] step=" << (iteration_ + 1)
            << " action=" << pending_step_.action
            << " target=" << pending_step_.target;

  const std::string& a = pending_step_.action;
  if      (a == "NAVIGATE")  DoNavigate();
  else if (a == "SEARCH")    DoSearch();
  else if (a == "CLICK")     DoClick();
  else if (a == "TYPE")      DoType();
  else if (a == "FILL_FORM") DoFillForm();
  else if (a == "SCROLL")    DoScroll();
  else if (a == "OBSERVE")   DoObserve();
  else if (a == "DONE")      Finish(true,  pending_step_.target);
  else if (a == "ERROR")     Finish(false, pending_step_.target);
  else                        DoObserve();  // Unknown — observe and try again.
}

// ----------------------------------------------------------------
// Action executors
// ----------------------------------------------------------------

void WebAgent::DoNavigate() {
  if (!web_contents_) { OnActionDone(false, "No active tab"); return; }
  GURL url(pending_step_.target);
  if (!url.is_valid()) {
    OnActionDone(false, "Invalid URL: " + pending_step_.target);
    return;
  }
  waiting_for_nav_ = true;
  web_contents_->GetController().LoadURL(
      url, content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());

  // Safety: proceed after timeout even if DidStopLoading never fires.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::MaybeNavDone, weak_factory_.GetWeakPtr()),
      base::Seconds(kNavTimeoutSeconds));
}

void WebAgent::DoSearch() {
  // Percent-encode the query then navigate.
  std::string encoded;
  for (unsigned char c : pending_step_.target) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += base::StringPrintf("%%%02X", c);
    }
  }
  pending_step_.target = kSearchBase + encoded;
  DoNavigate();
}

void WebAgent::DoClick() {
  std::string js = base::StringPrintf(
      R"JS((() => {
        try {
          const el = document.querySelector(%s);
          if (!el) return 'not_found';
          el.scrollIntoView({block:'center',behavior:'instant'});
          el.click();
          return 'clicked';
        } catch(e) { return 'error:'+e.message; }
      })())JS",
      JSQ(pending_step_.target).c_str());

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value v) {
        if (!self) return;
        bool ok = v.is_string() && v.GetString() == "clicked";
        // After a click, observe the page to capture any changes.
        if (ok) {
          self->ObservePage();
        } else {
          std::string note = v.is_string() ? v.GetString() : "no result";
          self->OnActionDone(false, "Click failed: " + note);
        }
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoType() {
  // TARGET = CSS selector, VALUE = text to type.
  std::string js = base::StringPrintf(
      R"JS((() => {
        try {
          const el = document.querySelector(%s);
          if (!el) return 'not_found';
          const nv = Object.getOwnPropertyDescriptor(
              window.HTMLInputElement.prototype, 'value') ||
              Object.getOwnPropertyDescriptor(
              window.HTMLTextAreaElement.prototype, 'value');
          if (nv && nv.set) { nv.set.call(el, %s); }
          else { el.value = %s; }
          el.dispatchEvent(new Event('input',  {bubbles:true}));
          el.dispatchEvent(new Event('change', {bubbles:true}));
          return 'typed';
        } catch(e) { return 'error:'+e.message; }
      })())JS",
      JSQ(pending_step_.target).c_str(),
      JSQ(pending_step_.value).c_str(),
      JSQ(pending_step_.value).c_str());

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value v) {
        if (!self) return;
        bool ok = v.is_string() && v.GetString() == "typed";
        std::string note = v.is_string() ? v.GetString() : "no result";
        self->OnActionDone(ok, note);
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoFillForm() {
  // TARGET: "sel1=val1; sel2=val2; ..."
  auto pairs = base::SplitString(pending_step_.target, ";",
                                  base::TRIM_WHITESPACE,
                                  base::SPLIT_WANT_NONEMPTY);
  std::string fields_json = "[";
  bool first = true;
  for (const auto& pair : pairs) {
    auto eq = pair.find('=');
    if (eq == std::string::npos) continue;
    std::string sel = Trim(pair.substr(0, eq));
    std::string val = Trim(pair.substr(eq + 1));
    if (sel.empty()) continue;
    if (!first) fields_json += ",";
    first = false;
    fields_json += "{\"sel\":" + JSQ(sel) + ",\"val\":" + JSQ(val) + "}";
  }
  fields_json += "]";

  std::string js = base::StringPrintf(
      R"JS((() => {
        const fields = %s;
        let filled = 0;
        fields.forEach(function(f){
          try {
            const el = document.querySelector(f.sel);
            if (!el) return;
            const nv = Object.getOwnPropertyDescriptor(
                window.HTMLInputElement.prototype, 'value') ||
                Object.getOwnPropertyDescriptor(
                window.HTMLTextAreaElement.prototype, 'value');
            if (nv && nv.set) { nv.set.call(el, f.val); }
            else { el.value = f.val; }
            el.dispatchEvent(new Event('input',  {bubbles:true}));
            el.dispatchEvent(new Event('change', {bubbles:true}));
            filled++;
          } catch(e) {}
        });
        return filled;
      })())JS",
      fields_json.c_str());

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value v) {
        if (!self) return;
        int n = v.is_int() ? v.GetInt() : 0;
        self->OnActionDone(n > 0, "Filled " + base::NumberToString(n) +
                                      " fields");
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoScroll() {
  std::string dir = base::ToLowerASCII(Trim(pending_step_.target));
  std::string js;
  if (dir == "down") {
    js = "window.scrollBy(0, 600); 'scrolled'";
  } else if (dir == "up") {
    js = "window.scrollBy(0, -600); 'scrolled'";
  } else {
    int px = 0;
    base::StringToInt(dir, &px);
    js = "window.scrollBy(0," + base::NumberToString(px) + "); 'scrolled'";
  }
  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value) {
        if (self) self->OnActionDone(true, "Scrolled");
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoObserve() {
  ObservePage();
}

// ----------------------------------------------------------------
// Page observation
// ----------------------------------------------------------------

void WebAgent::ObservePage() {
  if (cancelled_) return;
  if (!web_contents_) {
    current_page_state_ = "(no page loaded)";
    OnPageObserved(StructuredPage{});
    return;
  }
  dom_bridge_->GetStructuredPage(
      base::BindOnce(&WebAgent::OnPageObserved, weak_factory_.GetWeakPtr()));
}

void WebAgent::OnPageObserved(const StructuredPage& page) {
  if (cancelled_) return;

  std::string compact = page.ToCompactText();
  if (static_cast<int>(compact.size()) > kMaxPageChars)
    compact = compact.substr(0, kMaxPageChars) + "\n[… truncated]";

  current_page_state_ = "URL: " + page.page_url +
                         "\nTitle: " + page.page_title +
                         "\n\n" + compact;

  // Bootstrap path: first observe before the very first LLM call.
  if (iteration_ == 0 && history_.empty() && pending_step_.action.empty()) {
    IterateOnce();
    return;
  }

  // All other paths — the observe completes the current action.
  pending_step_.observation = current_page_state_;
  if (pending_step_.action == "NAVIGATE" || pending_step_.action == "SEARCH") {
    OnActionDone(true, "Navigated to " + page.page_url);
  } else if (pending_step_.action == "CLICK") {
    OnActionDone(true, "Clicked and re-observed page");
  } else {
    OnActionDone(true, "Page observed");
  }
}

// ----------------------------------------------------------------
// Navigation wait
// ----------------------------------------------------------------

void WebAgent::MaybeNavDone() {
  if (!waiting_for_nav_) return;
  waiting_for_nav_ = false;
  ObservePage();
}

// ----------------------------------------------------------------
// Action completion / finish
// ----------------------------------------------------------------

void WebAgent::OnActionDone(bool ok, std::string note) {
  if (cancelled_) return;

  pending_step_.success = ok;
  pending_step_.note    = note;
  if (pending_step_.observation.empty())
    pending_step_.observation = current_page_state_;
  pending_step_.number = iteration_ + 1;

  // Skip recording the silent bootstrap observe.
  bool is_bootstrap = iteration_ == 0 && history_.empty() &&
                      pending_step_.action == "OBSERVE";
  if (!pending_step_.action.empty() && !is_bootstrap) {
    history_.push_back(pending_step_);
    if (step_cb_)
      step_cb_.Run(pending_step_);
  }

  iteration_++;
  pending_step_ = Step();

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::IterateOnce, weak_factory_.GetWeakPtr()));
}

void WebAgent::Finish(bool success, const std::string& result) {
  if (cancelled_) return;
  cancelled_ = true;
  LOG(INFO) << "[WebAgent] Done success=" << success << " " << result;
  if (done_cb_) std::move(done_cb_).Run(success, result);
}

// ----------------------------------------------------------------
// JS execution
// ----------------------------------------------------------------

void WebAgent::EvalJS(const std::string& script,
                      base::OnceCallback<void(base::Value)> cb) {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    std::move(cb).Run(base::Value());
    return;
  }
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb, base::Value v) {
            std::move(cb).Run(std::move(v));
          },
          std::move(cb)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

// ----------------------------------------------------------------
// LLM prompt construction
// ----------------------------------------------------------------

std::string WebAgent::BuildPrompt() const {
  std::string p;
  p += base::StringPrintf(kSystemPrompt, kMaxIterations);
  p += "\n---\n";
  p += "GOAL: " + goal_ + "\n";
  p += "\nHISTORY:\n" + HistoryText();
  p += "\nCURRENT PAGE:\n" + current_page_state_ + "\n";
  p += "\nSteps remaining: " +
       base::NumberToString(kMaxIterations - iteration_) + "\n";
  p += "\nOutput your next action:\n";
  return p;
}

std::string WebAgent::HistoryText() const {
  if (history_.empty()) return "(none)\n";
  std::string out;
  for (const auto& s : history_) {
    out += "Step " + base::NumberToString(s.number) + ": [" + s.action + "] ";
    out += s.target;
    if (!s.value.empty()) out += " | " + s.value;
    out += " -> " + std::string(s.success ? "OK" : "FAILED");
    if (!s.note.empty()) out += " (" + s.note + ")";
    if (!s.observation.empty()) {
      std::string snip = s.observation.substr(
          0, std::min<int>(350, static_cast<int>(s.observation.size())));
      out += "\n  Observed: " + snip +
             (s.observation.size() > 350 ? "…" : "");
    }
    out += "\n";
  }
  return out;
}

// ----------------------------------------------------------------
// LLM response parsing
// ----------------------------------------------------------------

bool WebAgent::ParseLLMResponse(const std::string& text) {
  pending_step_ = Step();

  for (const auto& raw_line :
       base::SplitString(text, "\n", base::KEEP_WHITESPACE,
                          base::SPLIT_WANT_NONEMPTY)) {
    std::string line = Trim(raw_line);
    if (HasPrefixI(line, "ACTION:")) {
      pending_step_.action = base::ToUpperASCII(Trim(line.substr(7)));
    } else if (HasPrefixI(line, "TARGET:")) {
      pending_step_.target = Trim(line.substr(7));
    } else if (HasPrefixI(line, "VALUE:")) {
      pending_step_.value = Trim(line.substr(6));
    } else if (HasPrefixI(line, "REASON:")) {
      pending_step_.reason = Trim(line.substr(7));
    }
  }

  // For DONE / ERROR the result may be on the same line: "DONE: Found $834…"
  if ((pending_step_.action == "DONE" || pending_step_.action == "ERROR") &&
      pending_step_.target.empty()) {
    for (const auto& raw_line :
         base::SplitString(text, "\n", base::KEEP_WHITESPACE,
                            base::SPLIT_WANT_NONEMPTY)) {
      std::string line = Trim(raw_line);
      std::string upper = base::ToUpperASCII(line);
      if (base::StartsWith(upper, "DONE:", base::CompareCase::SENSITIVE) ||
          base::StartsWith(upper, "ERROR:", base::CompareCase::SENSITIVE)) {
        size_t colon = line.find(':');
        if (colon != std::string::npos)
          pending_step_.target = Trim(line.substr(colon + 1));
      }
    }
  }

  // TYPE may encode selector and text as "selector | text" in TARGET.
  if (pending_step_.action == "TYPE" && pending_step_.value.empty()) {
    auto bar = pending_step_.target.find(" | ");
    if (bar != std::string::npos) {
      pending_step_.value  = Trim(pending_step_.target.substr(bar + 3));
      pending_step_.target = Trim(pending_step_.target.substr(0, bar));
    }
  }

  return !pending_step_.action.empty() && !pending_step_.target.empty();
}

}  // namespace molt_ai
