// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// SiteToolHost: lists and calls a page's site tools (WebMCP;
// agent_tools.h, docs/design/AGENT_ACTIONS.md §2.7) for the web agent, the
// AI chat's proposals and Agent mode's tool steps.
//
// The documents are the page's main document and its frames of exactly the
// same origin (agent_tools::ToolFrames(): never a same-origin frame inside
// a cross-origin one, a fenced frame, a guest view or another page), and,
// only when the caller names their origin (the user allowed it on this
// page, frame_tools.h), frames of other sites that the main site embeds
// (agent_tools::CrossOriginToolFrames()). The caller may also have the
// other such frames' tools counted, never listed, to ask the user about
// them. Each document is listed in its own isolated world (the agent's,
// in that frame's own renderer), with agent_tools' scripts; isolated-world
// scripts don't wait for promises, so each request starts the work there
// and a poll script reports it, every agent_tools::kPollIntervalMs, only
// while the document is still the one asked (content::WeakDocumentPtr,
// plus the document's mark in the scripts themselves).
//
// A tool is its frame and its name: a call runs only in the very document
// that listed it, still a frame of the same main document as
// IsToolFrameChain() (or, for another site's frame,
// IsCrossOriginToolFrameChain()) requires, at the same origin. Nothing of
// one frame is passed to another: each frame's scripts run in that frame,
// and what they report comes to the browser. The pure rules live in
// agent_tools; this class only finds the documents and keeps them.
//
// UI thread only.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_SITE_TOOL_HOST_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_SITE_TOOL_HOST_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "content/public/browser/weak_document_ptr.h"
#include "url/origin.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai {

class SiteToolHost {
 public:
  SiteToolHost();
  ~SiteToolHost();
  SiteToolHost(const SiteToolHost&) = delete;
  SiteToolHost& operator=(const SiteToolHost&) = delete;

  // What a listing found.
  struct Listing {
    Listing();
    ~Listing();
    Listing(Listing&&);
    Listing& operator=(Listing&&);

    // The tools of every document listed (agent_tools::MakeSiteTools()):
    // the main document's first, then each frame's, in ToolFrames() order.
    // agent_actions::AddSiteTools() picks the ones offered.
    std::vector<agent_tools::SiteTool> tools;
    // The document of each frame number (agent_tools::SiteTool::frame);
    // [0] is the main document.
    std::vector<content::WeakDocumentPtr> documents;
    // Where each of them is (its origin and path), in the same order: a
    // recorded workflow finds its tool's frame by them.
    std::vector<agent_tools::FrameAddress> frames;

    // Frames of other sites whose tools were only counted
    // (OtherSites::count_others): each origin once, with how many tools
    // its frames list (only origins with at least one).
    struct Counted {
      std::string origin;
      size_t tools = 0;
    };
    std::vector<Counted> counted;
  };
  using ListCallback = base::OnceCallback<void(Listing)>;

  // Which frames of other sites a listing covers (CrossOriginToolFrames()).
  struct OtherSites {
    OtherSites();
    ~OtherSites();
    OtherSites(const OtherSites&);
    OtherSites& operator=(const OtherSites&);
    OtherSites(OtherSites&&);
    OtherSites& operator=(OtherSites&&);

    // The origins (url::Origin::Serialize()) whose frames' tools are
    // listed, as SiteTool::cross_origin tools.
    std::vector<std::string> listed;
    // Whether to count the tools of the other such frames (never listing
    // them), so the caller can ask the user.
    bool count_others = false;
  };

  // Lists the tools of |main_document| (the primary main document of its
  // tab, at |origin|) and of its same-origin frames, then of the frames of
  // |other_sites|' origins (each tool marked SiteTool::cross_origin, a
  // card or code tool left out: agent_tools::MayOfferCrossOriginTool()),
  // then counts the others' tools if asked, one document after the other,
  // within agent_tools::kListTimeoutMs in all. |done| gets what was found:
  // nothing if the main document is no longer its tab's, and only the
  // documents that answered in time.
  void List(content::WeakDocumentPtr main_document,
            url::Origin origin,
            OtherSites other_sites,
            ListCallback done);
  // The same with no frame of another site.
  void List(content::WeakDocumentPtr main_document,
            url::Origin origin,
            ListCallback done);

  // The frame |tool_document| (the main document itself, or a frame of
  // it) if a tool listed there may still run: |main_document| is still the
  // primary main document of its tab, at |origin|; |tool_document| is the
  // same document, active, and at |origin|; and every frame from the main
  // document down to it still counts (agent_tools::IsToolFrameChain()).
  // Else null.
  static content::RenderFrameHost* ToolFrameIfCurrent(
      const content::WeakDocumentPtr& main_document,
      const content::WeakDocumentPtr& tool_document,
      const url::Origin& origin);
  // The same for |tool|, listed in |tool_document|: for another site's
  // frame (SiteTool::cross_origin), the document is at the tool's
  // frame_origin, and the frames to it are an IsCrossOriginToolFrameChain().
  static content::RenderFrameHost* ToolFrameIfCurrent(
      const content::WeakDocumentPtr& main_document,
      const content::WeakDocumentPtr& tool_document,
      const url::Origin& origin,
      const agent_tools::SiteTool& tool);

  enum class CallEnd {
    // |outcome| is the call's answer, or why it didn't run or failed.
    kSettled,
    // No answer within agent_tools::kCallTimeoutMs.
    kTimedOut,
    // The tool's document went away after the call started: a tool (a
    // <form toolname> with toolautosubmit) can load another page.
    kDocumentChanged,
  };
  using CallCallback =
      base::OnceCallback<void(CallEnd, agent_tools::CallOutcome)>;

  // Calls |tool|, listed in |tool_document| (Listing::documents[tool.frame])
  // of |main_document|'s page at |origin|, with |arguments_json| (written by
  // agent_tools::ReadArguments()). Not run (kSettled with kDocumentChanged)
  // unless ToolFrameIfCurrent(..., tool); the script then checks the
  // document's mark, its origin (the frame's own for another site's frame)
  // and that the page still lists the same tool.
  void Call(content::WeakDocumentPtr main_document,
            content::WeakDocumentPtr tool_document,
            url::Origin origin,
            const agent_tools::SiteTool& tool,
            const std::string& arguments_json,
            CallCallback done);

  // Drops every pending listing and call: their callbacks never run.
  void Cancel();

 private:
  struct ListRun;
  struct CallRun;

  // Polls |script| in the document until it answers anything but
  // {state: 'pending'} (kSettled, with that answer), |deadline| passes
  // (kTimedOut), or the document is no longer current (kDocumentChanged).
  enum class PollEnd { kSettled, kTimedOut, kDocumentChanged };
  using PollCallback = base::OnceCallback<void(PollEnd, base::Value)>;
  struct PollTarget {
    content::WeakDocumentPtr main_document;
    content::WeakDocumentPtr document;
    url::Origin origin;
    // The document's origin when it is another site's frame; else empty.
    std::string other_site;
  };
  static content::RenderFrameHost* FrameIfCurrent(const PollTarget& target);
  void Poll(std::string script,
            PollTarget target,
            base::TimeTicks deadline,
            PollCallback done);
  void OnPolled(std::string script,
                PollTarget target,
                base::TimeTicks deadline,
                PollCallback done,
                base::Value result);

  void ListNext(std::unique_ptr<ListRun> run);
  void OnListStarted(std::unique_ptr<ListRun> run,
                     std::string request_id,
                     base::Value result);
  void OnListed(std::unique_ptr<ListRun> run, PollEnd end, base::Value result);
  void OnCounted(std::unique_ptr<ListRun> run, PollEnd end, base::Value result);

  void OnCallStarted(std::unique_ptr<CallRun> run, base::Value result);
  void OnCallPolled(std::unique_ptr<CallRun> run,
                    PollEnd end,
                    base::Value result);

  // Request ids: unique to this host (a random prefix), so another host's
  // state in the same document is never taken for this one's.
  std::string NextRequestId(char kind);

  const std::string id_prefix_;
  int next_request_ = 0;

  base::WeakPtrFactory<SiteToolHost> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_SITE_TOOL_HOST_H_
