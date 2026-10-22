// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The session behind a page's Translator (blink.mojom.Translator), on the
// user's own on-device model. Owned by MoltAIManager, which creates it once
// the user allowed the site (the same consent, limits and queue as the other
// web AI APIs). Design: docs/design/WEB_AI_APIS.md, section 10.
//
// A translate() call is cut into pieces (SplitForTranslation()); each piece
// is one generation under MoltBrowser's fixed instruction, decoded under
// kTranslatorResponseSchema, so the model can only answer with
// {"translation": "..."}. Each piece's translation reaches the page once it
// parsed (translateStreaming() streams piece by piece); whitespace between
// pieces, and text with nothing to translate, is passed through as it is.
// Aborting the call cancels the running generation; destroy() or the page
// dropping the Translator ends the session.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATOR_SESSION_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATOR_SESSION_H_

#include <cstdint>
#include <optional>
#include <string>

#include "chrome/browser/molt_ai/web_ai/web_ai_sessions.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/mojom/ai/model_streaming_responder.mojom.h"
#include "third_party/blink/public/mojom/on_device_translation/translator.mojom.h"

namespace molt_ai::web_ai {

class TranslatorSession : public WebAISession,
                          public blink::mojom::Translator {
 public:
  TranslatorSession(MoltAIManager& manager,
                    uint64_t id,
                    TranslatorPair pair,
                    mojo::PendingReceiver<blink::mojom::Translator> receiver);
  ~TranslatorSession() override;

  // blink::mojom::Translator:
  void Translate(const std::string& input,
                 mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                     pending_responder) override;
  void TranslateStreaming(
      const std::string& input,
      mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
          pending_responder) override;

 private:
  void Execute(const std::string& input,
               mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                   pending_responder);
  // The generation for the piece at |request.next_piece| (one to
  // translate), which then counts as sent.
  GenerationJob JobForNextPiece(Request& request);

  // WebAISession:
  std::optional<GenerationJob> Begin(Request& request) override;
  blink::mojom::ModelExecutionContextInfoPtr Commit(Request& request,
                                                    std::string reply) override;
  std::optional<std::string> ConstrainedText(Request& request,
                                             const std::string& reply) override;
  std::optional<GenerationJob> NextJob(Request& request) override;

  const TranslatorPair pair_;
  mojo::Receiver<blink::mojom::Translator> receiver_;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_TRANSLATOR_SESSION_H_
