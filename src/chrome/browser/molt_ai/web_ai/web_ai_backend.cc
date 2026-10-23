// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"

#include <utility>

namespace molt_ai::web_ai {

GenerationJob::GenerationJob() = default;
GenerationJob::GenerationJob(GenerationJob&&) = default;
GenerationJob& GenerationJob::operator=(GenerationJob&&) = default;
GenerationJob::~GenerationJob() = default;

void WebAIBackend::GetDeviceFactsForSite(const std::string& site_key,
                                         DeviceCallback callback) {
  GetDeviceFacts(std::move(callback));
}

bool WebAIBackend::WebApisSwitchOn() {
  return true;
}

}  // namespace molt_ai::web_ai
