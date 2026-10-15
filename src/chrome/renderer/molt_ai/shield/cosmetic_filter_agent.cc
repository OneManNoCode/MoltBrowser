// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/shield/cosmetic_filter_agent.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_css_origin.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "third_party/blink/public/web/web_script_source.h"

namespace molt_ai::shield {

namespace {

// Bounds on the generic class/id pass, per document.
constexpr size_t kMaxElementsPerScan = 20000;
constexpr size_t kMaxTokensPerRequest = 2000;
constexpr size_t kMaxTokenLength = 256;
constexpr int kMaxScans = 3;
constexpr base::TimeDelta kLateScanDelay = base::Seconds(2);

bool SameDocumentUrl(const GURL& a, const GURL& b) {
  return a.GetWithoutRef() == b.GetWithoutRef();
}

}  // namespace

// static
void CosmeticFilterAgent::MaybeCreate(content::RenderFrame* render_frame) {
  if (!molt_shield::IsCosmeticsEnabled()) {
    return;
  }
  new CosmeticFilterAgent(render_frame);  // Self-owned.
}

CosmeticFilterAgent::CosmeticFilterAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<molt_shield::mojom::CosmeticFiltersAgent>(
          base::BindRepeating(&CosmeticFilterAgent::BindReceiver,
                              base::Unretained(this)));
}

CosmeticFilterAgent::~CosmeticFilterAgent() = default;

void CosmeticFilterAgent::OnDestruct() {
  delete this;
}

void CosmeticFilterAgent::BindReceiver(
    mojo::PendingAssociatedReceiver<molt_shield::mojom::CosmeticFiltersAgent>
        receiver) {
  receivers_.Add(this, std::move(receiver));
}

void CosmeticFilterAgent::ApplyDocumentResources(
    const GURL& document_url,
    molt_shield::mojom::CosmeticResourcesPtr resources,
    mojo::PendingRemote<molt_shield::mojom::CosmeticFiltersHost> host) {
  pending_url_ = document_url;
  pending_ = std::move(resources);
  pending_host_ = std::move(host);
}

void CosmeticFilterAgent::ResetDocumentState() {
  ++document_sequence_;
  generic_pass_ = false;
  host_.reset();
  sent_classes_.clear();
  sent_ids_.clear();
  scans_ = 0;
  late_scan_timer_.Stop();
}

void CosmeticFilterAgent::DidCreateDocumentElement() {
  ResetDocumentState();
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!pending_ || !frame) {
    return;
  }
  molt_shield::mojom::CosmeticResourcesPtr resources = std::move(pending_);
  mojo::PendingRemote<molt_shield::mojom::CosmeticFiltersHost> host =
      std::move(pending_host_);
  // The resources were computed for the document the browser was about to
  // commit; anything else (initial empty document, a download, a 204) must
  // not get them.
  if (!SameDocumentUrl(GURL(frame->GetDocument().Url()), pending_url_)) {
    return;
  }

  if (!resources->hide_css.empty()) {
    InsertUserStyleSheet(resources->hide_css);
  }
  if (!resources->scriptlet_js.empty()) {
    // Main world, synchronously, before any page script: scriptlets must
    // patch globals before the page's own code reads them.
    frame->ExecuteScript(blink::WebScriptSource(
        blink::WebString::FromUTF8(resources->scriptlet_js)));
  }
  if (!resources->generichide && host.is_valid()) {
    host_.Bind(std::move(host));
    generic_pass_ = true;
  }
}

void CosmeticFilterAgent::DidDispatchDOMContentLoadedEvent() {
  ScanClassesAndIds();
}

void CosmeticFilterAgent::DidFinishLoad() {
  ScanClassesAndIds();
  if (generic_pass_ && scans_ < kMaxScans) {
    // Catch ad slots that scripts fill in right after load.
    late_scan_timer_.Start(
        FROM_HERE, kLateScanDelay,
        base::BindOnce(&CosmeticFilterAgent::ScanClassesAndIds,
                       weak_factory_.GetWeakPtr()));
  }
}

void CosmeticFilterAgent::ScanClassesAndIds() {
  if (!generic_pass_ || !host_.is_bound() || scans_ >= kMaxScans) {
    return;
  }
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame) {
    return;
  }
  ++scans_;
  blink::WebDocument document = frame->GetDocument();
  std::vector<std::string> classes;
  std::vector<std::string> ids;
  const blink::WebString class_attr = blink::WebString::FromASCII("class");

  auto add = [](std::vector<std::string>& out, std::set<std::string>& sent,
                std::string token) {
    if (token.empty() || token.size() > kMaxTokenLength ||
        out.size() >= kMaxTokensPerRequest) {
      return;
    }
    if (sent.insert(token).second) {
      out.push_back(std::move(token));
    }
  };

  // Pre-order walk of the light DOM, bounded.
  size_t visited = 0;
  blink::WebNode node = document.DocumentElement();
  while (!node.IsNull() && visited < kMaxElementsPerScan) {
    if (node.IsElementNode()) {
      ++visited;
      blink::WebElement element = node.To<blink::WebElement>();
      std::string id = element.GetIdAttribute().Utf8();
      if (!id.empty()) {
        add(ids, sent_ids_, std::move(id));
      }
      std::string class_list = element.GetAttribute(class_attr).Utf8();
      for (std::string_view token : base::SplitStringPiece(
               class_list, " \t\n\r\f", base::TRIM_WHITESPACE,
               base::SPLIT_WANT_NONEMPTY)) {
        add(classes, sent_classes_, std::string(token));
      }
    }
    // Advance: first child, else next sibling, else the next sibling of the
    // nearest ancestor that has one.
    blink::WebNode next = node.FirstChild();
    while (next.IsNull()) {
      next = node.NextSibling();
      if (!next.IsNull()) {
        break;
      }
      node = node.ParentNode();
      if (node.IsNull() || node.IsDocumentNode()) {
        break;  // Back at the top: done.
      }
    }
    if (next.IsNull()) {
      break;
    }
    node = next;
  }

  if (classes.empty() && ids.empty()) {
    return;
  }
  host_->HiddenClassIdSelectors(
      std::move(classes), std::move(ids),
      base::BindOnce(&CosmeticFilterAgent::OnHiddenCss,
                     weak_factory_.GetWeakPtr(), document_sequence_));
}

void CosmeticFilterAgent::OnHiddenCss(uint64_t document_sequence,
                                      const std::string& css) {
  if (document_sequence != document_sequence_ || css.empty()) {
    return;
  }
  InsertUserStyleSheet(css);
}

void CosmeticFilterAgent::InsertUserStyleSheet(const std::string& css) {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame) {
    return;
  }
  frame->GetDocument().InsertStyleSheet(blink::WebString::FromUTF8(css),
                                        /*key=*/nullptr,
                                        blink::WebCssOrigin::kUser);
}

}  // namespace molt_ai::shield
