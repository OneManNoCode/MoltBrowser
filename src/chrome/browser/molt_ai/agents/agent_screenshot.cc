// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_screenshot.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_features.h"
#include "chrome/browser/molt_ai/runtime/model_image_prep.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {

namespace {

using agent_vision::CaptureDecision;

// How long the compositor may take for the copy.
constexpr base::TimeDelta kCopyTimeout = base::Seconds(3);

// The frames |document| owns: its iframes, frames and fenced frames, and
// guests it embeds. Their own frames are drawn inside them.
size_t ChildFrameCount(content::RenderFrameHost* document) {
  size_t count = 0;
  document->ForEachRenderFrameHostWithAction(
      [document, &count](content::RenderFrameHost* rfh) {
        if (rfh == document) {
          return content::RenderFrameHost::FrameIterationAction::kContinue;
        }
        if (rfh->GetParentOrOuterDocumentOrEmbedder() == document) {
          ++count;
        }
        return content::RenderFrameHost::FrameIterationAction::kSkipChildren;
      });
  return count;
}

// The geometry measured in the observed document found all of its frames
// (a frame in a closed shadow root can't be found, so it can't be masked).
bool FramesAllFound(const agent_vision::ScreenshotGeometry& geometry,
                    const content::WeakDocumentPtr& document) {
  content::RenderFrameHost* frame = document.AsRenderFrameHostIfValid();
  return frame &&
         agent_vision::FoundEveryFrame(geometry, ChildFrameCount(frame));
}

// On a worker: scale (Skia), then mask and tag. Work grows with the
// bitmap, which is at most kCaptureMaxSide on its long side.
std::optional<ModelImage> PrepareScreenshot(
    SkBitmap bitmap,
    agent_actions::Observation observation,
    agent_vision::ScreenshotGeometry geometry) {
  if (bitmap.drawsNothing() || bitmap.width() <= 0 || bitmap.height() <= 0)
    return std::nullopt;
  const uint32_t width = static_cast<uint32_t>(bitmap.width());
  const uint32_t height = static_cast<uint32_t>(bitmap.height());
  // The plan PrepareImageForModel() follows (the same inputs), for where
  // the screenshot sits on the canvas.
  const std::optional<ImagePlan> plan =
      PlanModelImage(width, height, agent_vision::kScreenshotMaxSide);
  if (!plan)
    return std::nullopt;
  std::optional<ModelImage> image =
      PrepareImageForModel(bitmap, agent_vision::kScreenshotMaxSide);
  if (!image || !agent_vision::AnnotateScreenshot(*image, *plan, width, height,
                                                  observation, geometry)) {
    return std::nullopt;
  }
  return image;
}

}  // namespace

struct AgentScreenshotter::Pending {
  Request request;
  Callback callback;
  // The geometry measured before the copy.
  agent_vision::ScreenshotGeometry before;
  // The copy.
  SkBitmap bitmap;
};

AgentScreenshotter::Request::Request() = default;
AgentScreenshotter::Request::~Request() = default;
AgentScreenshotter::Request::Request(Request&&) = default;
AgentScreenshotter::Request& AgentScreenshotter::Request::operator=(Request&&) =
    default;

AgentScreenshotter::AgentScreenshotter(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents) {}

AgentScreenshotter::~AgentScreenshotter() = default;

void AgentScreenshotter::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
}

CaptureDecision AgentScreenshotter::Decide(const Request& request) const {
  agent_vision::CaptureFacts facts;
  facts.switched_on = agent_features::AgentScreenshotsEnabled();
  facts.run_allows_images = request.run_allows_images;
  facts.model_sees_images = request.model_sees_images;
  // The web agent's model is BrowserAIRuntime's: the on-device model in the
  // sandboxed inference service.
  facts.on_device_model = true;
  facts.observed_origin = request.observed_origin;
  facts.origin_allowed = request.origin_allowed;
  content::WebContents* contents = web_contents();
  content::RenderFrameHost* main =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (contents && main) {
    facts.in_moltnet = tor::IsMoltNetProfile(
        Profile::FromBrowserContext(contents->GetBrowserContext()));
    facts.tab_visible =
        contents->GetVisibility() == content::Visibility::VISIBLE;
    content::RenderFrameHost* observed =
        request.document.AsRenderFrameHostIfValid();
    facts.same_document = observed == main && observed->IsActive() &&
                          observed->IsInPrimaryMainFrame();
    facts.error_document = main->IsErrorDocument();
    facts.url_scheme = std::string(main->GetLastCommittedURL().scheme());
    facts.committed_origin = main->GetLastCommittedOrigin().Serialize();
  }
  return agent_vision::DecideCapture(facts);
}

void AgentScreenshotter::Capture(Request request, Callback callback) {
  // One capture at a time: a newer observation replaces an older one.
  weak_factory_.InvalidateWeakPtrs();
  auto pending = std::make_unique<Pending>();
  pending->request = std::move(request);
  pending->callback = std::move(callback);
  const CaptureDecision decision = Decide(pending->request);
  if (decision != CaptureDecision::kCapture) {
    DVLOG(1) << "[WebAgent] No screenshot: "
             << agent_vision::CaptureDecisionName(decision);
    Finish(std::move(pending), std::nullopt);
    return;
  }
  MeasureGeometry(std::move(pending), &AgentScreenshotter::OnGeometryBefore);
}

void AgentScreenshotter::MeasureGeometry(
    std::unique_ptr<Pending> pending,
    void (AgentScreenshotter::*next)(std::unique_ptr<Pending>, base::Value)) {
  content::RenderFrameHost* frame =
      pending->request.document.AsRenderFrameHostIfValid();
  if (!frame) {
    (this->*next)(std::move(pending), base::Value());
    return;
  }
  const std::string script =
      agent_vision::BuildGeometryScript(pending->request.observation);
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce(
          [](base::WeakPtr<AgentScreenshotter> self,
             void (AgentScreenshotter::*next)(std::unique_ptr<Pending>,
                                              base::Value),
             std::unique_ptr<Pending> pending, base::Value result) {
            if (self)
              ((*self).*next)(std::move(pending), std::move(result));
          },
          weak_factory_.GetWeakPtr(), next, std::move(pending)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void AgentScreenshotter::OnGeometryBefore(std::unique_ptr<Pending> pending,
                                          base::Value result) {
  std::optional<agent_vision::ScreenshotGeometry> geometry =
      agent_vision::ParseGeometry(result,
                                  pending->request.observation.elements.size());
  // Measured in the observed document, which hasn't navigated since.
  if (!geometry || geometry->origin != pending->request.observed_origin ||
      Decide(pending->request) != CaptureDecision::kCapture ||
      !FramesAllFound(*geometry, pending->request.document)) {
    DVLOG(1) << "[WebAgent] No screenshot: no usable geometry";
    Finish(std::move(pending), std::nullopt);
    return;
  }
  pending->before = std::move(*geometry);

  content::RenderWidgetHostView* view =
      web_contents() ? web_contents()->GetRenderWidgetHostView() : nullptr;
  if (!view || !view->IsSurfaceAvailableForCopy()) {
    Finish(std::move(pending), std::nullopt);
    return;
  }
  const gfx::Size view_size = view->GetViewBounds().size();
  const std::optional<agent_vision::CaptureSize> size =
      agent_vision::CaptureSizeFor(view_size.width(), view_size.height());
  if (!size) {
    Finish(std::move(pending), std::nullopt);
    return;
  }

  // The compositor may answer on another sequence: copy the bitmap out
  // (it shares the pixels) and come back to this one before touching
  // anything else.
  auto next = base::BindOnce(&AgentScreenshotter::OnCopied,
                             weak_factory_.GetWeakPtr(), std::move(pending));
  auto on_copied = base::BindOnce(
      [](scoped_refptr<base::SequencedTaskRunner> runner,
         base::OnceCallback<void(SkBitmap)> next,
         const content::CopyFromSurfaceResult& result) {
        SkBitmap bitmap;
        if (result.has_value())
          bitmap = result.value().bitmap;
        runner->PostTask(FROM_HERE,
                         base::BindOnce(std::move(next), std::move(bitmap)));
      },
      base::SequencedTaskRunner::GetCurrentDefault(), std::move(next));
  viz::CopyOutputBitmapWithMetadata empty;
  view->CopyFromSurface(gfx::Rect(),  // the whole view: the visible viewport
                        gfx::Size(size->width, size->height), kCopyTimeout,
                        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                            std::move(on_copied), base::OwnedRef(empty)));
}

void AgentScreenshotter::OnCopied(std::unique_ptr<Pending> pending,
                                  SkBitmap bitmap) {
  if (bitmap.drawsNothing() ||
      Decide(pending->request) != CaptureDecision::kCapture) {
    DVLOG(1) << "[WebAgent] No screenshot: the copy failed or the page "
                "changed";
    Finish(std::move(pending), std::nullopt);
    return;
  }
  pending->bitmap = std::move(bitmap);
  MeasureGeometry(std::move(pending), &AgentScreenshotter::OnGeometryAfter);
}

void AgentScreenshotter::OnGeometryAfter(std::unique_ptr<Pending> pending,
                                         base::Value result) {
  std::optional<agent_vision::ScreenshotGeometry> after =
      agent_vision::ParseGeometry(result,
                                  pending->request.observation.elements.size());
  if (!after || !agent_vision::SameGeometry(pending->before, *after) ||
      Decide(pending->request) != CaptureDecision::kCapture ||
      !FramesAllFound(*after, pending->request.document)) {
    DVLOG(1) << "[WebAgent] No screenshot: the page moved during the copy";
    Finish(std::move(pending), std::nullopt);
    return;
  }
  SkBitmap bitmap = std::move(pending->bitmap);
  agent_actions::Observation observation = pending->request.observation;
  agent_vision::ScreenshotGeometry geometry = pending->before;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&PrepareScreenshot, std::move(bitmap),
                     std::move(observation), std::move(geometry)),
      base::BindOnce(&AgentScreenshotter::Finish, weak_factory_.GetWeakPtr(),
                     std::move(pending)));
}

void AgentScreenshotter::Finish(std::unique_ptr<Pending> pending,
                                std::optional<ModelImage> image) {
  std::move(pending->callback).Run(std::move(image));
}

}  // namespace molt_ai
