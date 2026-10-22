// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/ai/ai_interface_proxy.h"

#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

const char AIInterfaceProxy::kSupplementName[] = "AIInterfaceProxy";

// TODO(crbug.com/406770758): Consider refactoring to have this class own the
// execution context as a member.
AIInterfaceProxy::AIInterfaceProxy(ExecutionContext* execution_context)
    : Supplement<ExecutionContext>(*execution_context),
      task_runner_(
          execution_context->GetTaskRunner(TaskType::kInternalDefault)),
      language_detection_model_(
          MakeGarbageCollected<LanguageDetectionModel>()) {}

AIInterfaceProxy::~AIInterfaceProxy() = default;

void AIInterfaceProxy::Trace(Visitor* visitor) const {
  Supplement<ExecutionContext>::Trace(visitor);
  visitor->Trace(translation_manager_remote_);
  visitor->Trace(language_detection_driver_);
  visitor->Trace(language_detection_model_);
  visitor->Trace(ai_manager_remote_);
}

// static
scoped_refptr<base::SequencedTaskRunner> AIInterfaceProxy::GetTaskRunner(
    ExecutionContext* execution_context) {
  return AIInterfaceProxy::From(execution_context)->task_runner_;
}

// static
HeapMojoRemote<mojom::blink::TranslationManager>&
AIInterfaceProxy::GetTranslationManagerRemote(
    ExecutionContext* execution_context) {
  return From(execution_context)
      ->GetTranslationManagerRemoteImpl(execution_context);
}

// static
void AIInterfaceProxy::GetLanguageDetectionModelStatus(
    ExecutionContext* execution_context,
    GetLanguageDetectionModelStatusCallback callback) {
  // MoltBrowser: LanguageDetector isn't served (docs/design/WEB_AI_APIS.md
  // §10). Its upstream model comes from the optimization guide, which needs
  // a Google API key: without one availability() said "downloadable" and
  // create() never finished; with the user's own key it said "available".
  // Every page in every window, MoltNet included, now gets "unavailable",
  // answered here without asking the browser.
  From(execution_context)
      ->task_runner_->PostTask(
          FROM_HERE,
          blink::BindOnce(
              [](GetLanguageDetectionModelStatusCallback callback) {
                std::move(callback).Run(
                    language_detection::mojom::blink::
                        LanguageDetectionModelStatus::kNotAvailable);
              },
              std::move(callback)));
}

// static
void AIInterfaceProxy::GetLanguageDetectionModel(
    ExecutionContext* execution_context,
    GetLanguageDetectionModelCallback callback) {
  // MoltBrowser: no model (see above), so create() fails ("Model not
  // available") for every page, and Google's model file is never loaded
  // for one. Translate's page-language detection doesn't come through here.
  From(execution_context)
      ->task_runner_->PostTask(
          FROM_HERE,
          blink::BindOnce(
              [](GetLanguageDetectionModelCallback callback) {
                std::move(callback).Run(
                    base::unexpected(DetectLanguageError::kUnavailable));
              },
              std::move(callback)));
}

// static
HeapMojoRemote<mojom::blink::AIManager>& AIInterfaceProxy::GetAIManagerRemote(
    ExecutionContext* execution_context) {
  return From(execution_context)->GetAIManagerRemoteImpl(execution_context);
}

// static
AIInterfaceProxy* AIInterfaceProxy::From(ExecutionContext* execution_context) {
  AIInterfaceProxy* translation_manager_proxy =
      Supplement<ExecutionContext>::From<AIInterfaceProxy>(*execution_context);
  if (!translation_manager_proxy) {
    translation_manager_proxy =
        MakeGarbageCollected<AIInterfaceProxy>(execution_context);
    ProvideTo(*execution_context, translation_manager_proxy);
  }
  return translation_manager_proxy;
}

HeapMojoRemote<mojom::blink::TranslationManager>&
AIInterfaceProxy::GetTranslationManagerRemoteImpl(
    ExecutionContext* execution_context) {
  if (!translation_manager_remote_.is_bound()) {
    execution_context->GetBrowserInterfaceBroker().GetInterface(
        translation_manager_remote_.BindNewPipeAndPassReceiver(task_runner_));
  }
  return translation_manager_remote_;
}

HeapMojoRemote<
    language_detection::mojom::blink::ContentLanguageDetectionDriver>&
AIInterfaceProxy::GetLanguageDetectionDriverRemote(
    ExecutionContext* execution_context) {
  if (!language_detection_driver_.is_bound()) {
    execution_context->GetBrowserInterfaceBroker().GetInterface(
        language_detection_driver_.BindNewPipeAndPassReceiver(task_runner_));
  }
  return language_detection_driver_;
}

void AIInterfaceProxy::GetLanguageDetectionModelImpl(
    ExecutionContext* execution_context,
    GetLanguageDetectionModelCallback callback) {
  GetLanguageDetectionDriverRemote(execution_context)
      ->GetLanguageDetectionModel(blink::BindOnce(
          [](LanguageDetectionModel* language_detection_model,
             GetLanguageDetectionModelCallback callback, base::File model) {
            language_detection_model->LoadModelFile(std::move(model),
                                                    std::move(callback));
          },
          WrapPersistent(language_detection_model_.Get()),
          std::move(callback)));
}

HeapMojoRemote<mojom::blink::AIManager>&
AIInterfaceProxy::GetAIManagerRemoteImpl(ExecutionContext* execution_context) {
  if (!ai_manager_remote_.is_bound()) {
    execution_context->GetBrowserInterfaceBroker().GetInterface(
        ai_manager_remote_.BindNewPipeAndPassReceiver(task_runner_));
  }
  return ai_manager_remote_;
}

}  // namespace blink
