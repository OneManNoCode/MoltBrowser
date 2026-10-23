// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/site_tool_host.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "url/gurl.h"

namespace molt_ai {

namespace {

using FrameIterationAction = content::RenderFrameHost::FrameIterationAction;

// Runs |script| in |frame|'s isolated world (the agent's, as the web
// agent's other scripts). |done| gets the result, or an empty value if the
// frame has no live renderer or goes away before it answers; always from a
// task of its own then, never from inside the frame's teardown.
void EvalInIsolatedWorld(content::RenderFrameHost* frame,
                         const std::string& script,
                         base::OnceCallback<void(base::Value)> done) {
  if (!frame->IsRenderFrameLive()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(done), base::Value()));
    return;
  }
  auto [on_result, on_drop] = base::SplitOnceCallback(std::move(done));
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      mojo::WrapCallbackWithDropHandler(
          std::move(on_result),
          base::BindOnce(
              [](base::OnceCallback<void(base::Value)> late) {
                base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                    FROM_HERE, base::BindOnce(std::move(late), base::Value()));
              },
              std::move(on_drop))),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

// |main_document| if it is still the primary main document of its tab,
// active, at |origin|.
content::RenderFrameHost* CurrentMain(
    const content::WeakDocumentPtr& main_document,
    const url::Origin& origin) {
  content::RenderFrameHost* main = main_document.AsRenderFrameHostIfValid();
  if (!main || !main->IsActive() || !main->IsInPrimaryMainFrame() ||
      main->GetLastCommittedOrigin() != origin) {
    return nullptr;
  }
  return main;
}

// What agent_tools needs to know of |frame| on |main|'s page (its parent
// is set by the caller).
agent_tools::PageFrame FactsOf(content::RenderFrameHost* frame,
                               content::RenderFrameHost* main) {
  agent_tools::PageFrame f;
  f.origin = frame->GetLastCommittedOrigin().Serialize();
  // Not a frame of a fenced frame, a guest view or another inner page
  // (their main frame is their own), not being unloaded or cached.
  f.in_page = frame->GetMainFrame() == main && frame->IsActive() &&
              frame->IsRenderFrameLive();
  return f;
}

// How the user reads |frame|: its origin and path; a document without an
// http(s) URL of its own (about:blank, about:srcdoc, blob:) has no path.
std::string TextOf(content::RenderFrameHost* frame,
                   const std::string& main_origin) {
  const GURL& url = frame->GetLastCommittedURL();
  return agent_tools::FrameText(
      main_origin, url.SchemeIsHTTPOrHTTPS() ? url.path() : std::string_view());
}

}  // namespace

SiteToolHost::Listing::Listing() = default;
SiteToolHost::Listing::~Listing() = default;
SiteToolHost::Listing::Listing(Listing&&) = default;
SiteToolHost::Listing& SiteToolHost::Listing::operator=(Listing&&) = default;

struct SiteToolHost::ListRun {
  ListCallback done;
  content::WeakDocumentPtr main_document;
  url::Origin origin;
  base::TimeTicks deadline;
  // FrameText() of each document of |listing.documents| ("" for [0]).
  std::vector<std::string> frame_texts;
  // The document being listed (an index into |listing.documents|).
  size_t next = 0;
  Listing listing;
};

struct SiteToolHost::CallRun {
  CallCallback done;
  PollTarget target;
  std::string request_id;
};

SiteToolHost::SiteToolHost()
    : id_prefix_(base::UnguessableToken::Create().ToString().substr(0, 12)) {}

SiteToolHost::~SiteToolHost() = default;

std::string SiteToolHost::NextRequestId(char kind) {
  return std::string(1, kind) + id_prefix_ + "-" +
         base::NumberToString(++next_request_);
}

void SiteToolHost::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
}

// static
content::RenderFrameHost* SiteToolHost::ToolFrameIfCurrent(
    const content::WeakDocumentPtr& main_document,
    const content::WeakDocumentPtr& tool_document,
    const url::Origin& origin) {
  content::RenderFrameHost* main = CurrentMain(main_document, origin);
  content::RenderFrameHost* frame = tool_document.AsRenderFrameHostIfValid();
  if (!main || !frame || !frame->IsActive() ||
      frame->GetLastCommittedOrigin() != origin) {
    return nullptr;
  }
  // The frames from it up to the main document, as they are now: each must
  // still count, as when it was listed (the document of a frame can't
  // outlive its parent's, but checking costs a few pointers).
  std::vector<content::RenderFrameHost*> up;
  for (content::RenderFrameHost* f = frame; f; f = f->GetParent()) {
    up.push_back(f);
    if (up.size() > agent_tools::kMaxToolFrames + 1) {
      return nullptr;
    }
  }
  if (up.back() != main) {
    return nullptr;  // under a fenced frame, a guest view or another page
  }
  std::vector<agent_tools::PageFrame> chain;
  for (auto it = up.rbegin(); it != up.rend(); ++it) {
    agent_tools::PageFrame f = FactsOf(*it, main);
    if (!chain.empty()) {
      f.parent = chain.size() - 1;
    }
    chain.push_back(std::move(f));
  }
  return agent_tools::IsToolFrameChain(chain) ? frame : nullptr;
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

void SiteToolHost::List(content::WeakDocumentPtr main_document,
                        url::Origin origin,
                        ListCallback done) {
  auto run = std::make_unique<ListRun>();
  run->done = std::move(done);
  run->main_document = main_document;
  run->origin = origin;
  run->deadline = base::TimeTicks::Now() +
                  base::Milliseconds(agent_tools::kListTimeoutMs);
  if (content::RenderFrameHost* main = CurrentMain(main_document, origin)) {
    // The page's frames, breadth first as ForEachRenderFrameHost visits
    // them, each with its parent's index. A frame that doesn't count (in
    // another page, another origin) hides its subtree, and the walk stops
    // at kMaxPageFrames.
    const std::string main_origin = origin.Serialize();
    std::vector<content::RenderFrameHost*> hosts;
    std::vector<agent_tools::PageFrame> frames;
    main->ForEachRenderFrameHostWithAction(
        [&](content::RenderFrameHost* frame) -> FrameIterationAction {
          if (frames.size() >= agent_tools::kMaxPageFrames) {
            return FrameIterationAction::kStop;
          }
          agent_tools::PageFrame f = FactsOf(frame, main);
          if (frame != main) {
            auto parent =
                std::find(hosts.begin(), hosts.end(), frame->GetParent());
            if (parent == hosts.end()) {
              // The root of an inner page (a fenced frame, a guest view):
              // its GetParent() is null.
              return FrameIterationAction::kSkipChildren;
            }
            f.parent = static_cast<size_t>(std::distance(hosts.begin(), parent));
          }
          const bool counts = agent_tools::IsSameOriginFrame(f, main_origin);
          hosts.push_back(frame);
          frames.push_back(std::move(f));
          return counts ? FrameIterationAction::kContinue
                        : FrameIterationAction::kSkipChildren;
        });
    for (size_t index : agent_tools::ToolFrames(frames)) {
      run->listing.documents.push_back(hosts[index]->GetWeakDocumentPtr());
      run->frame_texts.push_back(
          index == 0 ? std::string() : TextOf(hosts[index], main_origin));
    }
  }
  ListNext(std::move(run));
}

void SiteToolHost::ListNext(std::unique_ptr<ListRun> run) {
  while (run->next < run->listing.documents.size() &&
         base::TimeTicks::Now() < run->deadline) {
    content::RenderFrameHost* frame =
        ToolFrameIfCurrent(run->main_document,
                           run->listing.documents[run->next], run->origin);
    if (!frame) {
      ++run->next;
      continue;
    }
    std::string request_id = NextRequestId('l');
    // A fresh, unguessable mark: it becomes the document's unless the
    // document already has one.
    const std::string script = agent_tools::BuildToolListScript(
        request_id, base::UnguessableToken::Create().ToString());
    EvalInIsolatedWorld(
        frame, script,
        base::BindOnce(&SiteToolHost::OnListStarted, weak_factory_.GetWeakPtr(),
                       std::move(run), std::move(request_id)));
    return;
  }
  ListCallback done = std::move(run->done);
  std::move(done).Run(std::move(run->listing));
}

void SiteToolHost::OnListStarted(std::unique_ptr<ListRun> run,
                                 std::string request_id,
                                 base::Value result) {
  // 'unavailable' (no WebMCP there, the common case) or a failure: no
  // tools from this document, and no polling.
  if (agent_tools::ParseToolList(result).state !=
      agent_tools::ListState::kPending) {
    ++run->next;
    ListNext(std::move(run));
    return;
  }
  PollTarget target{run->main_document, run->listing.documents[run->next],
                    run->origin};
  const base::TimeTicks deadline = run->deadline;
  Poll(agent_tools::BuildToolListPollScript(request_id), std::move(target),
       deadline,
       base::BindOnce(&SiteToolHost::OnListed, weak_factory_.GetWeakPtr(),
                      std::move(run)));
}

void SiteToolHost::OnListed(std::unique_ptr<ListRun> run,
                            PollEnd end,
                            base::Value result) {
  if (end == PollEnd::kSettled) {
    const agent_tools::ToolList list = agent_tools::ParseToolList(result);
    // Only the document asked (Poll() checked it), at the origin observed.
    if (list.state == agent_tools::ListState::kDone &&
        list.origin == run->origin.Serialize()) {
      std::vector<agent_tools::SiteTool> tools = agent_tools::MakeSiteTools(
          list, run->next, run->frame_texts[run->next]);
      run->listing.tools.insert(run->listing.tools.end(),
                                std::make_move_iterator(tools.begin()),
                                std::make_move_iterator(tools.end()));
    }
  }
  ++run->next;
  ListNext(std::move(run));
}

// ---------------------------------------------------------------------------
// Calling
// ---------------------------------------------------------------------------

void SiteToolHost::Call(content::WeakDocumentPtr main_document,
                        content::WeakDocumentPtr tool_document,
                        url::Origin origin,
                        const agent_tools::SiteTool& tool,
                        const std::string& arguments_json,
                        CallCallback done) {
  content::RenderFrameHost* frame =
      ToolFrameIfCurrent(main_document, tool_document, origin);
  if (!frame) {
    // The document that listed the tool navigated, went away, or is no
    // longer a frame that counts: not run.
    agent_tools::CallOutcome not_run;
    not_run.state = agent_tools::CallState::kDocumentChanged;
    std::move(done).Run(CallEnd::kSettled, std::move(not_run));
    return;
  }
  auto run = std::make_unique<CallRun>();
  run->done = std::move(done);
  run->target = PollTarget{std::move(main_document), std::move(tool_document),
                           origin};
  run->request_id = NextRequestId('c');
  const std::string script = agent_tools::BuildToolCallScript(
      run->request_id, origin.Serialize(), tool, arguments_json);
  EvalInIsolatedWorld(frame, script,
                      base::BindOnce(&SiteToolHost::OnCallStarted,
                                     weak_factory_.GetWeakPtr(),
                                     std::move(run)));
}

void SiteToolHost::OnCallStarted(std::unique_ptr<CallRun> run,
                                 base::Value result) {
  agent_tools::CallOutcome started = agent_tools::ParseToolCallStart(result);
  if (started.state != agent_tools::CallState::kStarted) {
    std::move(run->done).Run(CallEnd::kSettled, std::move(started));
    return;
  }
  PollTarget target = run->target;
  std::string script = agent_tools::BuildToolCallPollScript(run->request_id);
  Poll(std::move(script), std::move(target),
       base::TimeTicks::Now() + base::Milliseconds(agent_tools::kCallTimeoutMs),
       base::BindOnce(&SiteToolHost::OnCallPolled, weak_factory_.GetWeakPtr(),
                      std::move(run)));
}

void SiteToolHost::OnCallPolled(std::unique_ptr<CallRun> run,
                                PollEnd end,
                                base::Value result) {
  agent_tools::CallOutcome outcome;
  switch (end) {
    case PollEnd::kTimedOut:
      outcome.state = agent_tools::CallState::kTimedOut;
      std::move(run->done).Run(CallEnd::kTimedOut, std::move(outcome));
      return;
    case PollEnd::kDocumentChanged:
      // The call's state went with its document.
      outcome.state = agent_tools::CallState::kGone;
      std::move(run->done).Run(CallEnd::kDocumentChanged, std::move(outcome));
      return;
    case PollEnd::kSettled:
      break;
  }
  std::move(run->done).Run(CallEnd::kSettled,
                           agent_tools::ParseToolCallPoll(result));
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

void SiteToolHost::Poll(std::string script,
                        PollTarget target,
                        base::TimeTicks deadline,
                        PollCallback done) {
  content::RenderFrameHost* frame = ToolFrameIfCurrent(
      target.main_document, target.document, target.origin);
  if (!frame) {
    std::move(done).Run(PollEnd::kDocumentChanged, base::Value());
    return;
  }
  const std::string js = script;
  EvalInIsolatedWorld(
      frame, js,
      base::BindOnce(&SiteToolHost::OnPolled, weak_factory_.GetWeakPtr(),
                     std::move(script), std::move(target), deadline,
                     std::move(done)));
}

void SiteToolHost::OnPolled(std::string script,
                            PollTarget target,
                            base::TimeTicks deadline,
                            PollCallback done,
                            base::Value result) {
  // An answer from a document that is no longer the one asked (or none,
  // because it went away) is not its answer.
  if (!ToolFrameIfCurrent(target.main_document, target.document,
                          target.origin)) {
    std::move(done).Run(PollEnd::kDocumentChanged, base::Value());
    return;
  }
  const std::string* state =
      result.is_dict() ? result.GetDict().FindString("state") : nullptr;
  if (!state || *state != "pending") {
    std::move(done).Run(PollEnd::kSettled, std::move(result));
    return;
  }
  if (base::TimeTicks::Now() >= deadline) {
    std::move(done).Run(PollEnd::kTimedOut, base::Value());
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&SiteToolHost::Poll, weak_factory_.GetWeakPtr(),
                     std::move(script), std::move(target), deadline,
                     std::move(done)),
      base::Milliseconds(agent_tools::kPollIntervalMs));
}

}  // namespace molt_ai
