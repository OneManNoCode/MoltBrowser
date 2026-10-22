// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AgentScreenshotter: the web agent's screenshot of the visible viewport
// (docs/design/AGENT_ACTIONS.md §2.6), for a loaded model that sees images.
//
// One capture, for one observation:
//   1. DecideCapture() (agent_vision.h): the switch, a model with vision,
//      the agent's own tab shown, the observed document still its primary
//      main document, an http(s) page with the observed origin in the
//      run's scope;
//   2. the geometry script in the agent's isolated world, in that
//      document: the viewport, the listed elements' boxes, what to mask;
//   3. the compositor's copy of the tab's view (RenderWidgetHostView::
//      CopyFromSurface), scaled to at most kCaptureMaxSide;
//   4. the decision again, and the geometry again: the page must not have
//      moved, scrolled or navigated during the copy;
//   5. on a worker: PrepareImageForModel() (Skia, at most
//      kScreenshotMaxSide), then the masks and the tags.
// Any refusal or failure gives no image, and the step goes on text-only.
// The screenshot exists only in memory, in the browser and then the
// sandboxed inference service; it is never stored, logged or shown.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_SCREENSHOT_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_SCREENSHOT_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_vision.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents_observer.h"

class SkBitmap;

namespace base {
class Value;
}  // namespace base

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {

class AgentScreenshotter : public content::WebContentsObserver {
 public:
  // What the capture must hold to: the observation it goes with.
  struct Request {
    Request();
    ~Request();
    Request(Request&&);
    Request& operator=(Request&&);

    // The elements to tag (their ids and selectors).
    agent_actions::Observation observation;
    // The document the snapshot ran in, and the location.origin it read.
    content::WeakDocumentPtr document;
    std::string observed_origin;
    // That origin is in the run's scope (WebAgent's allowed set).
    bool origin_allowed = false;
    // BrowserAIRuntime::LoadedModelSeesImages(), now.
    bool model_sees_images = false;
    // No request with an image failed earlier in the run.
    bool run_allows_images = false;
  };

  using Callback = base::OnceCallback<void(std::optional<ModelImage> image)>;

  // |web_contents| is the agent's own tab; nothing else is ever captured.
  explicit AgentScreenshotter(content::WebContents* web_contents);
  ~AgentScreenshotter() override;

  AgentScreenshotter(const AgentScreenshotter&) = delete;
  AgentScreenshotter& operator=(const AgentScreenshotter&) = delete;

  // Captures for |request| and runs |callback| on the UI thread with the
  // masked, tagged model image, or nullopt (refused, the page changed, the
  // copy failed). A capture in progress is abandoned (its callback never
  // runs).
  void Capture(Request request, Callback callback);

  // Abandons the capture in progress, if any.
  void Cancel();

 private:
  struct Pending;

  agent_vision::CaptureDecision Decide(const Request& request) const;
  void OnGeometryBefore(std::unique_ptr<Pending> pending, base::Value result);
  void OnCopied(std::unique_ptr<Pending> pending, SkBitmap bitmap);
  void OnGeometryAfter(std::unique_ptr<Pending> pending, base::Value result);
  void Finish(std::unique_ptr<Pending> pending,
              std::optional<ModelImage> image);
  // Runs the geometry script in the observed document; |next| gets an
  // empty value if the document is gone.
  void MeasureGeometry(
      std::unique_ptr<Pending> pending,
      void (AgentScreenshotter::*next)(std::unique_ptr<Pending>, base::Value));

  base::WeakPtrFactory<AgentScreenshotter> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_SCREENSHOT_H_
