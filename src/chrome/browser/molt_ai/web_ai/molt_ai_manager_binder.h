// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The one entry point upstream code calls:
// ChromeContentBrowserClient::BindAIManager (chrome_content_browser_client.cc)
// hands every blink.mojom.AIManager receiver here instead of to upstream's
// AIManager, for documents (|rfh| set) and workers (|rfh| null).

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom-forward.h"

namespace base {
class SupportsUserData;
}  // namespace base

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace molt_ai::web_ai {

// Binds |receiver| to the MoltAIManager of |context_user_data| (the
// document's or worker's user data, which owns it under |user_data_key|),
// creating it on first use.
void BindMoltAIManager(content::BrowserContext* browser_context,
                       base::SupportsUserData* context_user_data,
                       content::RenderFrameHost* rfh,
                       const void* user_data_key,
                       mojo::PendingReceiver<blink::mojom::AIManager> receiver);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_
