// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The two entry points upstream code calls (chrome_content_browser_client.cc):
//  * ChromeContentBrowserClient::BindAIManager hands every
//    blink.mojom.AIManager receiver here instead of to upstream's AIManager,
//    for documents (|rfh| set) and workers (|rfh| null);
//  * ChromeContentBrowserClient::BindTranslationManager hands every
//    blink.mojom.TranslationManager receiver here instead of to upstream's
//    TranslationManagerImpl, so a page's Translator never reaches
//    TranslateKit or the component updater.
// A document (or worker) has one MoltAIManager serving both interfaces.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom-forward.h"
#include "third_party/blink/public/mojom/on_device_translation/translation_manager.mojom-forward.h"

namespace base {
class SupportsUserData;
}  // namespace base

namespace content {
class BrowserContext;
class RenderFrameHost;
class RenderProcessHost;
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

// The same for the Translator API. Content binds TranslationManager
// without the RenderFrameHost: |context_user_data| is the document's
// associated data (or the worker host), and |process_host| the renderer
// it lives in, where its RenderFrameHost is looked up. |user_data_key| is
// BindMoltAIManager()'s, so both interfaces share one manager.
void BindMoltTranslationManager(
    content::RenderProcessHost* process_host,
    content::BrowserContext* browser_context,
    base::SupportsUserData* context_user_data,
    const void* user_data_key,
    mojo::PendingReceiver<blink::mojom::TranslationManager> receiver);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_BINDER_H_
